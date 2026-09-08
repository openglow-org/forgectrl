/*
 * commission.c - the commissioning record and the controller gate
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * One JSON document, written whole and atomically on every change:
 *
 *   schema, created, completed, sheet_id
 *   advisories: { <doc id>: { hash, accepted, method } }
 *   acceptance: { pressed_at }
 *   account:    { name, uid, created }
 *   machine:    { ... the machine wizard's facts ... }
 *   wizards:    { <id>: { version, completed, result, applied } }
 *   flags:      { <id>: { level, reason } }
 *
 * The gate is evaluated from the record and the image's required
 * table on every change and reported to the supervisor, which spawns
 * no controller for a sender while it is closed. The evaluation never
 * trusts a field it cannot parse: a damaged record reads as "nothing
 * done", which closes the gate and serves the wizard.
 */
#define _GNU_SOURCE
#include "commission.h"
#include "advisories.h"
#include "fflog.h"
#include "paths.h"
#include "sheetid.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The image's table: every wizard the machine must have completed, at
 * the version this build requires. A bump makes the wizard required
 * again on every machine that updates to this build. The form wizards
 * of the initial run are listed first; the hardware wizards join as
 * they land. */
static const commission_req_t image_required[] = {
    { "advisories",        1, NULL },
    { "account",           1, NULL },
    { "preferences",       1, NULL },
    { "machine",           1, NULL },
    { "cloud",             1, NULL },
    /* The dark validation: the machine checked with the laser locked. */
    { "switches",          1, NULL },
    { "sensors",           1, NULL },
    { "airflow",           1, NULL },
    { "cooling.aa-offset", 1, NULL },
    { "cooling.flow",      1, NULL },
    { "cooling.tec",       1, "tec" },
    { "motion",            1, NULL },
    { "cameras",           1, NULL },
    { "cloud.header",      1, "cloud_enabled" },
    /* Phase 3: the sheet. */
    { "sheet.place",       1, NULL },
    { "sheet.frame",       1, NULL },
    { "laser.focus",       1, NULL },
    { "laser.floor",       1, NULL },
    { "laser.dose-curve",  1, NULL },
    { "laser.corner",      1, NULL },
    { "cooling.flow-load", 1, NULL },
};

int (*commission_fact_hook)(const char *fact);

/* The what-changed menu (the panel's Commissioning tab). The settings a
 * card measured belong to the part it measured them on: a new tube has
 * its own floor, dose curve, and heat coefficient; a new pump or
 * coolant its own flow bands; a new fan its own floor; a new head its
 * own focus height and free travel (and the record's head facts, which
 * the boot check also flags). Those are required, so the gate holds
 * until the numbers are the new part's. The checks that only prove a
 * part are recommended. */
static const commission_change_t changes[] = {
    { "tube", "The laser tube was replaced", "the tube was replaced",
      { { "laser.floor", "required" }, { "laser.dose-curve", "required" },
        { "cooling.flow-load", "required" }, { "laser.corner", "recommended" } } },
    { "pump", "The coolant pump was replaced", "the pump was replaced",
      { { "cooling.flow", "required" } } },
    { "coolant", "The coolant was changed", "the coolant was changed",
      { { "cooling.flow", "required" } } },
    { "fan", "A fan was replaced", "a fan was replaced",
      { { "airflow", "required" } } },
    { "head", "The head was replaced", "the head was replaced",
      { { "machine", "required" }, { "laser.focus", "required" },
        { "motion", "recommended" }, { "cameras", "recommended" } } },
    { "tray", "The tray was replaced", "the tray was replaced",
      { { "laser.focus", "recommended" } } },
    { "service", "A cover was off: belts, drivers, wiring, or switches were serviced",
      "the machine was serviced",
      { { "switches", "recommended" }, { "motion", "recommended" } } },
};

static const commission_req_t *required = image_required;
static size_t nrequired = sizeof(image_required) / sizeof(*image_required);

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static json_t *rec;                 /* the record, owned here */

/* The evaluated gate, refreshed by evaluate_locked(). */
static int g_first_run = 1;
static int g_gate = 0;
static int g_override = 0;
static char g_why[256];

static void record_path(char *buf, size_t len)
{
    snprintf(buf, len, "%s/commissioning.json", ff_data_dir());
}

static void override_path(char *buf, size_t len)
{
    snprintf(buf, len, "%s/commissioning-override", ff_run_dir());
}

static void now_iso(char *buf, size_t len)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static json_t *obj_get_or_make(json_t *parent, const char *key)
{
    json_t *o = json_object_get(parent, key);
    if (!json_is_object(o)) {
        o = json_object();
        json_object_set_new(parent, key, o);
    }
    return o;
}

static json_t *fresh_record(void)
{
    char ts[32], sid[SHEETID_LEN + 1];
    now_iso(ts, sizeof(ts));
    json_t *r = json_object();
    json_object_set_new(r, "schema", json_integer(COMMISSION_SCHEMA));
    json_object_set_new(r, "created", json_string(ts));
    if (sheetid_get(sid, sizeof(sid)) == 0)
        json_object_set_new(r, "sheet_id", json_string(sid));
    json_object_set_new(r, "advisories", json_object());
    json_object_set_new(r, "wizards", json_object());
    json_object_set_new(r, "flags", json_object());
    return r;
}

/* Write the record: temp file, fsync, rename, directory fsync. Called
 * with mu held. Returns 0 on success. */
static int save_locked(void)
{
    char path[256], tmp[264];
    record_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    mkdir(ff_data_dir(), 0755);

    char *text = json_dumps(rec, JSON_INDENT(2) | JSON_SORT_KEYS);
    if (!text)
        return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        free(text);
        fflog(LOG_ERR, "commission: cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }
    size_t len = strlen(text), off = 0;
    int ok = 1;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n <= 0) {
            ok = 0;
            break;
        }
        off += (size_t)n;
    }
    ok = ok && write(fd, "\n", 1) == 1 && fsync(fd) == 0;
    close(fd);
    free(text);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        fflog(LOG_ERR, "commission: cannot save the record: %s", strerror(errno));
        return -1;
    }
    int dfd = open(ff_data_dir(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return 0;
}

static int advisories_complete_locked(void)
{
    size_t n;
    const advisory_t *docs = advisories_list(&n);
    json_t *acc = json_object_get(rec, "advisories");
    if (!json_is_object(acc))
        return 0;
    for (size_t i = 0; i < n; i++) {
        json_t *d = json_object_get(acc, docs[i].id);
        const char *h = json_string_value(json_object_get(d, "hash"));
        if (!h || strcmp(h, docs[i].hash))
            return 0;
    }
    return 1;
}

static int acceptance_done_locked(void)
{
    json_t *a = json_object_get(rec, "acceptance");
    return json_is_string(json_object_get(a, "pressed_at"));
}

static int account_set_locked(void)
{
    json_t *a = json_object_get(rec, "account");
    return json_is_string(json_object_get(a, "name"));
}

static int wizard_version_locked(const char *id)
{
    json_t *w = json_object_get(json_object_get(rec, "wizards"), id);
    json_t *v = json_object_get(w, "version");
    return json_is_integer(v) ? (int)json_integer_value(v) : 0;
}

/* Recompute the gate. Called with mu held. */
/* Does requirement i apply to this machine? Unconditional ones always;
 * a conditional one when its fact holds, in the machine block or by the
 * hook. Called with mu held. */
static int req_applies_locked(size_t i)
{
    if (!required[i].when)
        return 1;
    json_t *m = json_object_get(rec, "machine");
    json_t *f = json_object_get(m, required[i].when);
    if (f)
        return json_is_true(f);
    return commission_fact_hook ? commission_fact_hook(required[i].when) : 0;
}

static void evaluate_locked(void)
{
    char path[256];
    override_path(path, sizeof(path));
    g_override = access(path, F_OK) == 0;
    g_first_run = !json_is_string(json_object_get(rec, "completed"));

    int consent = advisories_complete_locked() && acceptance_done_locked();
    int account = account_set_locked();
    int wizards = 1;
    char missing[160] = "";
    for (size_t i = 0; i < nrequired; i++) {
        if (!req_applies_locked(i))
            continue;               /* a fact this machine lacks: not required */
        if (wizard_version_locked(required[i].id) < required[i].version) {
            wizards = 0;
            if (missing[0])
                strncat(missing, ", ", sizeof(missing) - strlen(missing) - 1);
            strncat(missing, required[i].id, sizeof(missing) - strlen(missing) - 1);
        }
    }
    /* Engine-raised required flags close the gate too. */
    const char *k;
    json_t *f;
    json_object_foreach(json_object_get(rec, "flags"), k, f) {
        const char *lvl = json_string_value(json_object_get(f, "level"));
        if (lvl && !strcmp(lvl, "required") &&
            wizard_version_locked(k) > 0) {
            /* A completed wizard flagged required must run again. */
            wizards = 0;
            if (missing[0])
                strncat(missing, ", ", sizeof(missing) - strlen(missing) - 1);
            strncat(missing, k, sizeof(missing) - strlen(missing) - 1);
        }
    }

    if (!consent)
        snprintf(g_why, sizeof(g_why), "the advisories are not accepted");
    else if (!account)
        snprintf(g_why, sizeof(g_why), "no account exists");
    else if (!wizards && !g_override)
        snprintf(g_why, sizeof(g_why), "commissioning required: %s", missing);
    else if (!wizards && g_override)
        snprintf(g_why, sizeof(g_why), "override active (required: %s)", missing);
    else
        g_why[0] = '\0';
    g_gate = consent && account && (wizards || g_override);
}

static void load_locked(void)
{
    char path[256];
    record_path(path, sizeof(path));
    json_error_t err;
    json_t *r = json_load_file(path, 0, &err);
    if (r && json_is_object(r) &&
        json_integer_value(json_object_get(r, "schema")) == COMMISSION_SCHEMA) {
        rec = r;
        /* The sheet id can arrive later than the record (a salt made
         * after a record that was seeded); fill it when it is known. */
        if (!json_is_string(json_object_get(rec, "sheet_id"))) {
            char sid[SHEETID_LEN + 1];
            if (sheetid_get(sid, sizeof(sid)) == 0)
                json_object_set_new(rec, "sheet_id", json_string(sid));
        }
        obj_get_or_make(rec, "advisories");
        obj_get_or_make(rec, "wizards");
        obj_get_or_make(rec, "flags");
        return;
    }
    if (r) {
        json_decref(r);
        fflog(LOG_WARNING, "commission: %s is not a schema %d record, "
                           "starting a new one", path, COMMISSION_SCHEMA);
    } else if (access(path, F_OK) == 0) {
        fflog(LOG_WARNING, "commission: %s unreadable (%s), starting a new "
                           "record", path, err.text);
    }
    rec = fresh_record();
}

void commission_init(void)
{
    pthread_mutex_lock(&mu);
    if (rec)
        json_decref(rec);
    load_locked();
    evaluate_locked();
    fflog(LOG_INFO, "commission: first_run=%d gate=%s override=%d%s%s",
          g_first_run, g_gate ? "open" : "closed", g_override,
          g_why[0] ? " why=" : "", g_why);
    pthread_mutex_unlock(&mu);
}

void commission_set_required(const commission_req_t *reqs, size_t n)
{
    pthread_mutex_lock(&mu);
    required = reqs ? reqs : image_required;
    nrequired = reqs ? n : sizeof(image_required) / sizeof(*image_required);
    if (rec)
        evaluate_locked();
    pthread_mutex_unlock(&mu);
}

int commission_first_run(void)
{
    pthread_mutex_lock(&mu);
    if (rec)
        evaluate_locked();
    int v = g_first_run;
    pthread_mutex_unlock(&mu);
    return v;
}

int commission_gate_open(char *why, size_t len)
{
    pthread_mutex_lock(&mu);
    if (rec)
        evaluate_locked();
    int v = g_gate;
    if (why && len)
        snprintf(why, len, "%s", g_why);
    pthread_mutex_unlock(&mu);
    return v;
}

int commission_override_active(void)
{
    char path[256];
    override_path(path, sizeof(path));
    return access(path, F_OK) == 0;
}

static int commit_locked(void)
{
    int rc = save_locked();
    evaluate_locked();
    return rc;
}

int commission_advisory_accept(const char *doc_id, const char *hash,
                                const char *method)
{
    const advisory_t *d = advisories_find(doc_id);
    if (!d || !hash || strcmp(hash, d->hash) || !method)
        return -1;
    char ts[32];
    now_iso(ts, sizeof(ts));
    pthread_mutex_lock(&mu);
    json_t *acc = obj_get_or_make(rec, "advisories");
    json_t *e = json_object();
    json_object_set_new(e, "hash", json_string(hash));
    json_object_set_new(e, "accepted", json_string(ts));
    json_object_set_new(e, "method", json_string(method));
    json_object_set_new(acc, doc_id, e);
    /* A new acceptance of any document needs the press again. */
    json_object_del(rec, "acceptance");
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    return rc;
}

int commission_acceptance_pressed(void)
{
    char ts[32];
    now_iso(ts, sizeof(ts));
    pthread_mutex_lock(&mu);
    if (!advisories_complete_locked()) {
        pthread_mutex_unlock(&mu);
        return -1;
    }
    json_t *a = obj_get_or_make(rec, "acceptance");
    json_object_set_new(a, "pressed_at", json_string(ts));
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    return rc;
}

int commission_set_account(const char *name, long uid)
{
    if (!name || !*name)
        return -1;
    char ts[32];
    now_iso(ts, sizeof(ts));
    pthread_mutex_lock(&mu);
    json_t *a = json_object();
    json_object_set_new(a, "name", json_string(name));
    json_object_set_new(a, "uid", json_integer(uid));
    json_object_set_new(a, "created", json_string(ts));
    json_object_set_new(rec, "account", a);
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    return rc;
}

int commission_clear_account(void)
{
    pthread_mutex_lock(&mu);
    json_object_del(rec, "account");
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    return rc;
}

int commission_set_machine(json_t *machine)
{
    if (!json_is_object(machine)) {
        if (machine)
            json_decref(machine);
        return -1;
    }
    pthread_mutex_lock(&mu);
    json_object_set_new(rec, "machine", machine);
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    return rc;
}

int commission_wizard_done(const char *id, int version, json_t *result,
                           json_t *applied)
{
    if (!id || version < 1) {
        if (result)
            json_decref(result);
        if (applied)
            json_decref(applied);
        return -1;
    }
    char ts[32];
    now_iso(ts, sizeof(ts));
    pthread_mutex_lock(&mu);
    json_t *w = json_object();
    json_object_set_new(w, "version", json_integer(version));
    json_object_set_new(w, "completed", json_string(ts));
    json_object_set_new(w, "result", result ? result : json_object());
    json_object_set_new(w, "applied", applied ? applied : json_object());
    json_object_set_new(obj_get_or_make(rec, "wizards"), id, w);
    /* A completed run clears any flag on the wizard. */
    json_object_del(obj_get_or_make(rec, "flags"), id);
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    return rc;
}

int commission_flag(const char *id, const char *level, const char *reason)
{
    if (!id)
        return -1;
    if (level && strcmp(level, "required") && strcmp(level, "recommended"))
        return -1;
    pthread_mutex_lock(&mu);
    json_t *flags = obj_get_or_make(rec, "flags");
    if (!level)
        json_object_del(flags, id);
    else {
        json_t *f = json_object();
        json_object_set_new(f, "level", json_string(level));
        json_object_set_new(f, "reason", json_string(reason ? reason : ""));
        json_object_set_new(flags, id, f);
    }
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    return rc;
}

int commission_complete(void)
{
    char ts[32];
    now_iso(ts, sizeof(ts));
    pthread_mutex_lock(&mu);
    json_object_set_new(rec, "completed", json_string(ts));
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    return rc;
}

int commission_record_json(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    char *text = json_dumps(rec, JSON_COMPACT | JSON_SORT_KEYS);
    pthread_mutex_unlock(&mu);
    if (!text)
        return -1;
    int n = snprintf(buf, len, "%s", text);
    free(text);
    return n >= (int)len ? -1 : n;
}

char *commission_record_dump(int indent)
{
    pthread_mutex_lock(&mu);
    char *text = json_dumps(rec, (indent ? JSON_INDENT(2) : JSON_COMPACT) | JSON_SORT_KEYS);
    pthread_mutex_unlock(&mu);
    return text;
}

json_t *commission_record_copy(void)
{
    pthread_mutex_lock(&mu);
    json_t *out = json_deep_copy(rec);
    pthread_mutex_unlock(&mu);
    return out;
}

const commission_change_t *commission_changes(size_t *n)
{
    if (n)
        *n = sizeof(changes) / sizeof(*changes);
    return changes;
}

int commission_change_apply(const char *what)
{
    const commission_change_t *c = NULL;
    for (size_t i = 0; i < sizeof(changes) / sizeof(*changes); i++)
        if (what && !strcmp(changes[i].id, what))
            c = &changes[i];
    if (!c)
        return -1;
    pthread_mutex_lock(&mu);
    json_t *flags = obj_get_or_make(rec, "flags");
    for (size_t i = 0; i < COMMISSION_CHANGE_WIZ && c->wizards[i].id; i++) {
        /* A required flag never drops to recommended. */
        json_t *old = json_object_get(flags, c->wizards[i].id);
        const char *ol = json_string_value(json_object_get(old, "level"));
        if (ol && !strcmp(ol, "required") && strcmp(c->wizards[i].level, "required"))
            continue;
        json_t *f = json_object();
        json_object_set_new(f, "level", json_string(c->wizards[i].level));
        json_object_set_new(f, "reason", json_string(c->reason));
        json_object_set_new(flags, c->wizards[i].id, f);
    }
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    if (rc == 0)
        fflog(LOG_NOTICE, "commission: %s: the wizards it needs are flagged", c->reason);
    return rc;
}

json_t *commission_changes_json(void)
{
    json_t *out = json_array();
    for (size_t i = 0; i < sizeof(changes) / sizeof(*changes); i++) {
        json_t *c = json_object();
        json_object_set_new(c, "id", json_string(changes[i].id));
        json_object_set_new(c, "title", json_string(changes[i].title));
        json_t *w = json_array();
        for (size_t k = 0; k < COMMISSION_CHANGE_WIZ && changes[i].wizards[k].id; k++)
            json_array_append_new(w, json_pack("{s:s,s:s}", "id", changes[i].wizards[k].id,
                                               "level", changes[i].wizards[k].level));
        json_object_set_new(c, "wizards", w);
        json_array_append_new(out, c);
    }
    return out;
}

int commission_advisories_complete(void)
{
    pthread_mutex_lock(&mu);
    int v = advisories_complete_locked();
    pthread_mutex_unlock(&mu);
    return v;
}

int commission_acceptance_done(void)
{
    pthread_mutex_lock(&mu);
    int v = acceptance_done_locked();
    pthread_mutex_unlock(&mu);
    return v;
}

void commission_machine_str(const char *key, char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    const char *v = json_string_value(json_object_get(json_object_get(rec, "machine"), key));
    snprintf(buf, len, "%s", v ? v : "");
    pthread_mutex_unlock(&mu);
}

int commission_machine_bool(const char *key)
{
    pthread_mutex_lock(&mu);
    int v = json_is_true(json_object_get(json_object_get(rec, "machine"), key));
    pthread_mutex_unlock(&mu);
    return v;
}

void commission_head_hash(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    json_t *head = json_object_get(json_object_get(rec, "machine"), "head");
    const char *v = json_string_value(json_object_get(head, "serial_hash"));
    snprintf(buf, len, "%s", v ? v : "");
    pthread_mutex_unlock(&mu);
}

int commission_wizard_version(const char *id)
{
    pthread_mutex_lock(&mu);
    int v = wizard_version_locked(id);
    pthread_mutex_unlock(&mu);
    return v;
}

json_t *commission_wizard_result(const char *id)
{
    pthread_mutex_lock(&mu);
    json_t *w = json_object_get(json_object_get(rec, "wizards"), id ? id : "");
    json_t *r = w ? json_object_get(w, "result") : NULL;
    json_t *out = r ? json_deep_copy(r) : NULL;
    pthread_mutex_unlock(&mu);
    return out;
}

void commission_completed_at(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    const char *c = json_string_value(json_object_get(rec, "completed"));
    snprintf(buf, len, "%s", c ? c : "");
    pthread_mutex_unlock(&mu);
}

int commission_status_json(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    evaluate_locked();
    json_t *st = json_object();
    json_object_set_new(st, "first_run", json_boolean(g_first_run));
    json_object_set_new(st, "gate", json_string(g_gate ? "open" : "closed"));
    json_object_set_new(st, "override", json_boolean(g_override));
    json_object_set_new(st, "why", json_string(g_why));
    json_object_set_new(st, "advisories_complete",
                        json_boolean(advisories_complete_locked()));
    json_object_set_new(st, "acceptance_done",
                        json_boolean(acceptance_done_locked()));
    json_object_set_new(st, "account", json_boolean(account_set_locked()));
    json_object_set_new(st, "completed",
                        json_boolean(!g_first_run));
    json_t *sid = json_object_get(rec, "sheet_id");
    json_object_set_new(st, "sheet_id",
                        json_is_string(sid) ? json_incref(sid) : json_string(""));

    /* Required: the table entries not satisfied, plus flagged ones. */
    json_t *req = json_array(), *recm = json_array();
    for (size_t i = 0; i < nrequired; i++)
        if (req_applies_locked(i) && wizard_version_locked(required[i].id) < required[i].version)
            json_array_append_new(req, json_string(required[i].id));
    const char *k;
    json_t *f;
    json_object_foreach(json_object_get(rec, "flags"), k, f) {
        const char *lvl = json_string_value(json_object_get(f, "level"));
        json_t *e = json_object();
        json_object_set_new(e, "id", json_string(k));
        json_object_set(e, "reason", json_object_get(f, "reason"));
        if (lvl && !strcmp(lvl, "required"))
            json_array_append_new(req, e);
        else
            json_array_append_new(recm, e);
    }
    json_object_set_new(st, "required", req);
    json_object_set_new(st, "recommended", recm);

    /* Per-wizard completed versions, for the page's progress rail. */
    json_t *done = json_object();
    json_object_foreach(json_object_get(rec, "wizards"), k, f)
        json_object_set(done, k, json_object_get(f, "version"));
    json_object_set_new(st, "versions", done);

    /* The advisory documents: id, title, consent, phrase, hash, and
     * whether the record holds this hash. */
    size_t n;
    const advisory_t *docs = advisories_list(&n);
    json_t *acc = json_object_get(rec, "advisories");
    json_t *dl = json_array();
    for (size_t i = 0; i < n; i++) {
        json_t *d = json_object();
        json_object_set_new(d, "id", json_string(docs[i].id));
        json_object_set_new(d, "title", json_string(docs[i].title));
        json_object_set_new(d, "consent", json_string(docs[i].consent));
        json_object_set_new(d, "phrase",
                            docs[i].phrase ? json_string(docs[i].phrase) : json_null());
        json_object_set_new(d, "hash", json_string(docs[i].hash));
        const char *h = json_string_value(
            json_object_get(json_object_get(acc, docs[i].id), "hash"));
        json_object_set_new(d, "accepted",
                            json_boolean(h && !strcmp(h, docs[i].hash)));
        json_array_append_new(dl, d);
    }
    json_object_set_new(st, "documents", dl);
    pthread_mutex_unlock(&mu);

    char *text = json_dumps(st, JSON_COMPACT);
    json_decref(st);
    if (!text)
        return -1;
    int rc = snprintf(buf, len, "%s", text);
    free(text);
    return rc >= (int)len ? -1 : rc;
}
