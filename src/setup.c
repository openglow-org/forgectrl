/*
 * setup.c - the setup record and the controller gate
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
 *   flags:      { <id>: { reason } }
 *
 * The gate is the advisories (accepted, and the press), the account,
 * the table's required steps (preferences, the machine facts, the cloud
 * decision, the coolant flow calibration: a loop whose coolant rises
 * past the default threshold with flow faults every job's flow check
 * until it is calibrated), and the intake fan count (a machine built
 * with one intake fan faults every job on the second intake until the
 * airflow gate knows): it is evaluated on every change and reported to the
 * supervisor, which spawns no controller for a sender while it is
 * closed. Everything else is recommended, never required:
 * the image's step table (a step not run, or run at an older version)
 * and the flags (a part replaced, a fault the engine saw) only fill the
 * recommended list the panel shows. The evaluation never trusts a field
 * it cannot parse: a damaged record reads as "nothing done", which
 * closes the gate and serves the wizard.
 */
#define _GNU_SOURCE
#include "setup.h"
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

/* The image's table: every step of the setup, at the version this
 * build wants. The required ones hold the gate until they are done (a
 * bump holds it again on every machine that updates to this build); a
 * bump of any other recommends the step again. The required steps of
 * the initial run are listed first. The laser floor and the bed size
 * are not here: they are offered, never recommended. */
static const setup_step_t image_steps[] = {
    { "advisories",        1, NULL, 0 },
    { "account",           1, NULL, 0 },
    { "preferences",       1, NULL, 1 },
    { "machine",           1, NULL, 1 },
    { "cloud",             1, NULL, 1 },
    /* The coolant flow bands differ from loop to loop and the default
     * threshold sits under the flowing rise of some, so the calibration
     * holds the gate. */
    { "cooling.flow",      1, NULL, 1 },
    /* The dark validation: the machine checked with the laser locked. */
    { "switches",          1, NULL, 0 },
    { "sensors",           1, NULL, 0 },
    { "airflow",           1, NULL, 0 },
    { "cooling.aa-offset", 1, NULL, 0 },
    { "motion",            1, NULL, 0 },
    { "cameras",           1, NULL, 0 },
    { "cloud.header",      1, "cloud_enabled", 0 },
    /* The lens: its free travel (dark), then the focus burn. */
    { "lens.travel",       1, NULL, 0 },
    { "laser.focus",       1, NULL, 0 },
};

int (*setup_fact_hook)(const char *fact);
int (*setup_known_hook)(const char *id);
int (*setup_fans_hook)(void);

/* The what-changed menu (the panel's Setup tab). The settings a step
 * measured belong to the part it measured them on: a new tube has its
 * own floor; a new pump or coolant its own flow bands; a new fan its
 * own floor; a new head its own free travel and focus height (and the
 * record's head facts, which the boot check also flags). Each change
 * recommends the steps that measure or prove the new part. */
static const setup_change_t changes[] = {
    { "tube", "The laser tube was replaced", "the tube was replaced",
      { "laser.floor" } },
    { "pump", "The coolant pump was replaced", "the pump was replaced",
      { "cooling.flow" } },
    { "coolant", "The coolant was changed", "the coolant was changed",
      { "cooling.flow" } },
    { "fan", "A fan was replaced", "a fan was replaced",
      { "airflow" } },
    { "head", "The head was replaced", "the head was replaced",
      { "machine", "lens.travel", "laser.focus", "motion", "cameras" } },
    { "tray", "The tray was replaced", "the tray was replaced",
      { "laser.focus" } },
    { "service", "A cover was off: belts, drivers, wiring, or switches were serviced",
      "the machine was serviced",
      { "switches", "motion" } },
};

static const setup_step_t *steps = image_steps;
static size_t nsteps = sizeof(image_steps) / sizeof(*image_steps);

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static json_t *rec;                 /* the record, owned here */

/* The evaluated gate, refreshed by evaluate_locked(). */
static int g_first_run = 1;
static int g_gate = 0;
static char g_why[256];

static void record_path(char *buf, size_t len)
{
    snprintf(buf, len, "%s/setup.json", ff_data_dir());
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
    json_object_set_new(r, "schema", json_integer(SETUP_SCHEMA));
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
        fflog(LOG_ERR, "setup: cannot write %s: %s", tmp, strerror(errno));
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
        fflog(LOG_ERR, "setup: cannot save the record: %s", strerror(errno));
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

/* Does step i apply to this machine? Unconditional ones always; a
 * conditional one when its fact holds, in the machine block or by the
 * hook. Called with mu held. */
static int step_applies_locked(size_t i)
{
    if (!steps[i].when)
        return 1;
    json_t *m = json_object_get(rec, "machine");
    json_t *f = json_object_get(m, steps[i].when);
    if (f)
        return json_is_true(f);
    return setup_fact_hook ? setup_fact_hook(steps[i].when) : 0;
}

/* A required step the gate waits for: not run, or run at an older
 * version than the table's. Called with mu held. */
static int required_missing_locked(size_t i)
{
    return steps[i].required && step_applies_locked(i) &&
           wizard_version_locked(steps[i].id) < steps[i].version;
}

/* The required steps the gate waits for, in the table's order: the page
 * opens them, done once or not, so a release that asks for one again
 * sends the operator back to it. Called with mu held. */
static json_t *missing_locked(void)
{
    json_t *m = json_array();
    for (size_t i = 0; i < nsteps; i++)
        if (required_missing_locked(i))
            json_array_append_new(m, json_string(steps[i].id));
    return m;
}

/* Recompute the gate: the advisories, the account, the required steps,
 * and the intake fan count. Called with mu held. */
static void evaluate_locked(void)
{
    g_first_run = !json_is_string(json_object_get(rec, "completed"));
    int consent = advisories_complete_locked() && acceptance_done_locked();
    int account = account_set_locked();
    char missing[160] = "";
    for (size_t i = 0; i < nsteps; i++) {
        if (!required_missing_locked(i))
            continue;
        if (missing[0])
            strncat(missing, ", ", sizeof(missing) - strlen(missing) - 1);
        strncat(missing, steps[i].id, sizeof(missing) - strlen(missing) - 1);
    }
    int fans = setup_fans_hook ? setup_fans_hook() : 1;
    if (!consent)
        snprintf(g_why, sizeof(g_why), "the advisories are not accepted");
    else if (!account)
        snprintf(g_why, sizeof(g_why), "no account exists");
    else if (missing[0])
        snprintf(g_why, sizeof(g_why), "setup required: %s", missing);
    else if (!fans)
        snprintf(g_why, sizeof(g_why), "the intake fans are not counted");
    else
        g_why[0] = '\0';
    g_gate = consent && account && !missing[0] && fans;
}

/* A record written under the file's earlier name (commissioning.json)
 * is taken over once: moved to the current name when no record is at
 * it yet, so the consent, the account, and every wizard's result stay
 * with the machine across the rename. */
static void adopt_earlier_record(const char *path)
{
    char old[256];
    snprintf(old, sizeof(old), "%s/commissioning.json", ff_data_dir());
    if (access(path, F_OK) == 0 || access(old, F_OK) != 0)
        return;
    if (rename(old, path) == 0)
        fflog(LOG_NOTICE, "setup: the record moved from %s to %s", old, path);
    else
        fflog(LOG_ERR, "setup: cannot move the record %s to %s: %s", old, path,
              strerror(errno));
}

static void load_locked(void)
{
    char path[256];
    record_path(path, sizeof(path));
    adopt_earlier_record(path);
    json_error_t err;
    json_t *r = json_load_file(path, 0, &err);
    if (r && json_is_object(r) &&
        json_integer_value(json_object_get(r, "schema")) == SETUP_SCHEMA) {
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
        fflog(LOG_WARNING, "setup: %s is not a schema %d record, "
                           "starting a new one", path, SETUP_SCHEMA);
    } else if (access(path, F_OK) == 0) {
        fflog(LOG_WARNING, "setup: %s unreadable (%s), starting a new "
                           "record", path, err.text);
    }
    rec = fresh_record();
}

void setup_init(void)
{
    pthread_mutex_lock(&mu);
    if (rec)
        json_decref(rec);
    load_locked();
    evaluate_locked();
    fflog(LOG_INFO, "setup: first_run=%d gate=%s%s%s",
          g_first_run, g_gate ? "open" : "closed",
          g_why[0] ? " why=" : "", g_why);
    pthread_mutex_unlock(&mu);
}

void setup_set_steps(const setup_step_t *s, size_t n)
{
    pthread_mutex_lock(&mu);
    steps = s ? s : image_steps;
    nsteps = s ? n : sizeof(image_steps) / sizeof(*image_steps);
    if (rec)
        evaluate_locked();
    pthread_mutex_unlock(&mu);
}

int setup_first_run(void)
{
    pthread_mutex_lock(&mu);
    if (rec)
        evaluate_locked();
    int v = g_first_run;
    pthread_mutex_unlock(&mu);
    return v;
}

int setup_gate_open(char *why, size_t len)
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

static int commit_locked(void)
{
    int rc = save_locked();
    evaluate_locked();
    return rc;
}

int setup_advisory_accept(const char *doc_id, const char *hash,
                                const char *method)
{
    const advisory_t *d = advisories_find(doc_id);
    if (!d || d->on_demand || !hash || strcmp(hash, d->hash) || !method)
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

int setup_on_demand_accept(const char *doc_id, const char *hash, const char *method)
{
    const advisory_t *d = advisories_find(doc_id);
    if (!d || !d->on_demand || !hash || strcmp(hash, d->hash) || !method)
        return -1;
    char ts[32];
    now_iso(ts, sizeof(ts));
    pthread_mutex_lock(&mu);
    json_t *acc = obj_get_or_make(rec, "on_demand");
    json_t *e = json_object();
    json_object_set_new(e, "hash", json_string(hash));
    json_object_set_new(e, "accepted", json_string(ts));
    json_object_set_new(e, "method", json_string(method));
    json_object_set_new(acc, doc_id, e);
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    return rc;
}

int setup_acceptance_pressed(void)
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

int setup_set_account(const char *name, long uid)
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

int setup_clear_account(void)
{
    pthread_mutex_lock(&mu);
    json_object_del(rec, "account");
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    return rc;
}

int setup_set_machine(json_t *machine)
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

int setup_wizard_done(const char *id, int version, json_t *result,
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
    /* A run is done at the version this build's table wants: the gate
     * and the recommended list compare the record with the table, so a
     * caller's own number never leaves a step that ran looking undone. */
    for (size_t i = 0; i < nsteps; i++)
        if (!strcmp(steps[i].id, id) && steps[i].version > version)
            version = steps[i].version;
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

int setup_flagged(const char *id)
{
    pthread_mutex_lock(&mu);
    int f = id && json_object_get(json_object_get(rec, "flags"), id) != NULL;
    pthread_mutex_unlock(&mu);
    return f;
}

int setup_flag(const char *id, const char *reason)
{
    if (!id)
        return -1;
    pthread_mutex_lock(&mu);
    json_t *flags = obj_get_or_make(rec, "flags");
    if (!reason)
        json_object_del(flags, id);
    else
        json_object_set_new(flags, id, json_pack("{s:s}", "reason", reason));
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    return rc;
}

int setup_complete(void)
{
    char ts[32];
    now_iso(ts, sizeof(ts));
    pthread_mutex_lock(&mu);
    json_object_set_new(rec, "completed", json_string(ts));
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    return rc;
}

int setup_record_json(char *buf, size_t len)
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

char *setup_record_dump(int indent)
{
    pthread_mutex_lock(&mu);
    char *text = json_dumps(rec, (indent ? JSON_INDENT(2) : JSON_COMPACT) | JSON_SORT_KEYS);
    pthread_mutex_unlock(&mu);
    return text;
}

json_t *setup_record_copy(void)
{
    pthread_mutex_lock(&mu);
    json_t *out = json_deep_copy(rec);
    pthread_mutex_unlock(&mu);
    return out;
}

const setup_change_t *setup_changes(size_t *n)
{
    if (n)
        *n = sizeof(changes) / sizeof(*changes);
    return changes;
}

int setup_change_apply(const char *what)
{
    const setup_change_t *c = NULL;
    for (size_t i = 0; i < sizeof(changes) / sizeof(*changes); i++)
        if (what && !strcmp(changes[i].id, what))
            c = &changes[i];
    if (!c)
        return -1;
    pthread_mutex_lock(&mu);
    json_t *flags = obj_get_or_make(rec, "flags");
    for (size_t i = 0; i < SETUP_CHANGE_WIZ && c->wizards[i]; i++)
        json_object_set_new(flags, c->wizards[i], json_pack("{s:s}", "reason", c->reason));
    int rc = commit_locked();
    pthread_mutex_unlock(&mu);
    if (rc == 0)
        fflog(LOG_NOTICE, "setup: %s: the steps it recommends are flagged", c->reason);
    return rc;
}

json_t *setup_changes_json(void)
{
    json_t *out = json_array();
    for (size_t i = 0; i < sizeof(changes) / sizeof(*changes); i++) {
        json_t *c = json_object();
        json_object_set_new(c, "id", json_string(changes[i].id));
        json_object_set_new(c, "title", json_string(changes[i].title));
        json_t *w = json_array();
        for (size_t k = 0; k < SETUP_CHANGE_WIZ && changes[i].wizards[k]; k++)
            json_array_append_new(w, json_pack("{s:s}", "id", changes[i].wizards[k]));
        json_object_set_new(c, "wizards", w);
        json_array_append_new(out, c);
    }
    return out;
}

int setup_advisories_complete(void)
{
    pthread_mutex_lock(&mu);
    int v = advisories_complete_locked();
    pthread_mutex_unlock(&mu);
    return v;
}

int setup_acceptance_done(void)
{
    pthread_mutex_lock(&mu);
    int v = acceptance_done_locked();
    pthread_mutex_unlock(&mu);
    return v;
}

void setup_machine_str(const char *key, char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    const char *v = json_string_value(json_object_get(json_object_get(rec, "machine"), key));
    snprintf(buf, len, "%s", v ? v : "");
    pthread_mutex_unlock(&mu);
}

int setup_machine_bool(const char *key)
{
    pthread_mutex_lock(&mu);
    int v = json_is_true(json_object_get(json_object_get(rec, "machine"), key));
    pthread_mutex_unlock(&mu);
    return v;
}

int setup_set_head(json_t *head)
{
    if (!json_is_object(head)) {
        if (head)
            json_decref(head);
        return -1;
    }
    pthread_mutex_lock(&mu);
    json_t *machine = json_object_get(rec, "machine");
    int rc = -1;
    if (json_is_object(machine)) {
        json_object_set_new(machine, "head", head);
        rc = commit_locked();
    } else
        json_decref(head);
    pthread_mutex_unlock(&mu);
    return rc;
}

void setup_head_hash(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    json_t *head = json_object_get(json_object_get(rec, "machine"), "head");
    const char *v = json_string_value(json_object_get(head, "serial_hash"));
    snprintf(buf, len, "%s", v ? v : "");
    pthread_mutex_unlock(&mu);
}

int setup_wizard_version(const char *id)
{
    pthread_mutex_lock(&mu);
    int v = wizard_version_locked(id);
    pthread_mutex_unlock(&mu);
    return v;
}

json_t *setup_wizard_result(const char *id)
{
    pthread_mutex_lock(&mu);
    json_t *w = json_object_get(json_object_get(rec, "wizards"), id ? id : "");
    json_t *r = w ? json_object_get(w, "result") : NULL;
    json_t *out = r ? json_deep_copy(r) : NULL;
    pthread_mutex_unlock(&mu);
    return out;
}

void setup_completed_at(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    const char *c = json_string_value(json_object_get(rec, "completed"));
    snprintf(buf, len, "%s", c ? c : "");
    pthread_mutex_unlock(&mu);
}

int setup_cloud_homing_ok(const char *mode)
{
    /* Limit switches are a valid setting with nothing behind them yet;
     * the step does not offer them. */
    return mode && (!strcmp(mode, "gfcloud") || !strcmp(mode, "manual") ||
                    !strcmp(mode, "none"));
}

/* Recommended: every flag with its reason, then the table's steps not
 * run, or run at an older version, that no flag already names. The
 * account, the advisories, and the required steps are the gate, never
 * listed here as not run (a flag on a required step is: a run done once
 * still holds the gate open), and a flag on a step this build no longer
 * has is left out. A new array. Called with mu held. */
static json_t *recommended_locked(void)
{
    json_t *recm = json_array();
    json_t *flags = json_object_get(rec, "flags");
    const char *k;
    json_t *f;
    json_object_foreach(flags, k, f) {
        if (setup_known_hook && !setup_known_hook(k))
            continue;
        const char *why = json_string_value(json_object_get(f, "reason"));
        json_array_append_new(recm, json_pack("{s:s,s:s}", "id", k, "reason", why ? why : ""));
    }
    for (size_t i = 0; i < nsteps; i++) {
        if (!strcmp(steps[i].id, "advisories") || !strcmp(steps[i].id, "account") ||
            steps[i].required || !step_applies_locked(i) || json_object_get(flags, steps[i].id))
            continue;
        int v = wizard_version_locked(steps[i].id);
        if (v < steps[i].version)
            json_array_append_new(recm, json_pack("{s:s,s:s}", "id", steps[i].id, "reason",
                                                  v ? "this release asks for it again"
                                                    : "not run yet"));
    }
    return recm;
}

int setup_recommended_count(void)
{
    pthread_mutex_lock(&mu);
    json_t *r = recommended_locked();
    int n = (int)json_array_size(r);
    json_decref(r);
    pthread_mutex_unlock(&mu);
    return n;
}

int setup_status_json(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    evaluate_locked();
    json_t *st = json_object();
    json_object_set_new(st, "first_run", json_boolean(g_first_run));
    json_object_set_new(st, "gate", json_string(g_gate ? "open" : "closed"));
    json_object_set_new(st, "why", json_string(g_why));
    json_object_set_new(st, "advisories_complete",
                        json_boolean(advisories_complete_locked()));
    json_object_set_new(st, "acceptance_done",
                        json_boolean(acceptance_done_locked()));
    json_object_set_new(st, "account", json_boolean(account_set_locked()));
    json_object_set_new(st, "fans_counted", json_boolean(setup_fans_hook ? setup_fans_hook() : 1));
    json_object_set_new(st, "completed",
                        json_boolean(!g_first_run));
    json_t *sid = json_object_get(rec, "sheet_id");
    json_object_set_new(st, "sheet_id",
                        json_is_string(sid) ? json_incref(sid) : json_string(""));

    json_object_set_new(st, "recommended", recommended_locked());
    json_object_set_new(st, "missing", missing_locked());
    const char *k;
    json_t *f;

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
