/*
 * wiz.c - the commissioning wizards and their routes
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The first run of the panel is a sequence of wizards, each a module
 * that can also run alone from the Commissioning tab. This file holds
 * the form wizards, the ones that need no hardware slot: advisories,
 * account, preferences, machine facts, and the cloud decision. Each
 * validates what the page sends, writes settings through the ordinary
 * validated path, and records its completion in the commissioning
 * record with what it found and what it wrote. The hardware wizards
 * (dark validation, the sheet) build on the same record and catalog.
 *
 * The one physical act here is the acceptance press: after the
 * documents, the button LED breathes teal and the page waits for a
 * release-to-press edge on the machine's button. No controller runs
 * during the first run, so the press can arm nothing.
 */
#define _GNU_SOURCE
#include "wiz.h"
#include "advisories.h"
#include "auth.h"
#include "button.h"
#include "cam.h"
#include "commission.h"
#include "fflog.h"
#include "hooks.h"
#include "led.h"
#include "recordhtml.h"
#include "session.h"
#include "settings.h"
#include "sheetid.h"
#include "tls.h"
#include "users.h"
#include "wizdark.h"
#include "wizlive.h"

#include <ctype.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PRESS_TIMEOUT_S 600
#define CLOCK_UNSET_BEFORE 1700000000   /* a clock earlier than this is unset */

/* The catalog: every wizard the page can show, in first-run order. The
 * version is the schema version the record stores on completion; the
 * required table in commission.c names the versions this image needs. */
typedef struct {
    const char *id;
    const char *title;
    int version;
    const char *cls;        /* form | dark | live */
} wiz_def_t;

static const wiz_def_t catalog[] = {
    { "advisories",          "Advisories",          1, "form" },
    { "account",             "Your account",        1, "form" },
    { "preferences",         "Preferences",         1, "form" },
    { "machine",             "Your machine",        1, "form" },
    { "cloud",               "Cloud mode",          1, "form" },
    { "switches",            "Switches",            1, "dark" },
    { "sensors",             "Sensors",             1, "dark" },
    { "airflow",             "Airflow",             1, "dark" },
    { "motion",              "Motion",              1, "dark" },
    { "cameras",             "Cameras",             1, "dark" },
    { "cooling.aa-offset",   "Coolant offset",      1, "dark" },
    { "cooling.flow",        "Coolant flow",        1, "dark" },
    { "cooling.tec",         "TEC",                 1, "dark" },
    { "cooling.flow-verify", "Flow check",          1, "dark" },
    { "cloud.header",        "Cloud header",        1, "dark" },
    { "sheet.place",         "Place the sheet",     1, "live" },
    { "sheet.frame",         "First fire",          1, "live" },
    { "laser.focus",         "Focus",               1, "live" },
    { "laser.floor",         "Laser floor",         1, "live" },
    { "laser.dose-curve",    "Dose curve",          1, "live" },
    { "laser.corner",        "Corner rolloff",      1, "live" },
    { "cooling.flow-load",   "Flow under load",     1, "live" },
};
#define NCATALOG (sizeof(catalog) / sizeof(*catalog))

static void head_info(char *hw_id, size_t hl, char *serial, size_t sl,
                      char *version, size_t vl);

/* The head-change trigger: a record made with one head, booted with
 * another, requires the machine facts again (and, later, the wizards
 * that depend on the head). */
static void check_head_change(void)
{
    char recorded[32], hw[24], ser[24], ver[24], now[32] = "";
    commission_head_hash(recorded, sizeof(recorded));
    if (!recorded[0])
        return;
    head_info(hw, sizeof(hw), ser, sizeof(ser), ver, sizeof(ver));
    if (!ser[0])
        return;                     /* no head to compare: nothing to say */
    sheetid_derive(ser, now, sizeof(now));
    if (strcmp(recorded, now)) {
        fflog(LOG_NOTICE, "wiz: the head changed since the record was made");
        commission_flag("machine", "required", "the head changed");
    }
}

void wiz_init(void)
{
    check_head_change();
}

/* Finish lights the button solid green; the light goes out when the
 * control panel is first served after it, which is the operator's
 * next click. */
static int finish_lit;

void wiz_abort_all(void)
{
    button_wait_cancel();
    finish_lit = 0;
    led_release();
}

/* ------------------------------------------------------------ helpers */

static int reply_json(struct _u_response *res, unsigned status, const char *body)
{
    ulfius_set_string_body_response(res, status, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    return U_CALLBACK_CONTINUE;
}

static int reply_error(struct _u_response *res, unsigned status, const char *msg)
{
    json_t *o = json_object();
    json_object_set_new(o, "error", json_string(msg));
    char *text = json_dumps(o, JSON_COMPACT);
    json_decref(o);
    reply_json(res, status, text ? text : "{\"error\":\"error\"}");
    free(text);
    return U_CALLBACK_CONTINUE;
}

static int reply_obj(struct _u_response *res, unsigned status, json_t *o)
{
    char *text = json_dumps(o, JSON_COMPACT);
    json_decref(o);
    reply_json(res, status, text ? text : "{}");
    free(text);
    return U_CALLBACK_CONTINUE;
}

/* A form or query parameter. */
/* The login session behind a request, for a wizard run's owner: ""
 * when the request carries none (a tool with the token). */
static void requester(const struct _u_request *req, char *out, size_t len)
{
    const char *cookie = u_map_get_case(req->map_header, "Cookie");
    char id[SESSION_ID_HEX + 1];
    if (cookie && session_from_cookie(cookie, id, sizeof(id)) && session_valid(id))
        snprintf(out, len, "%s", id);
    else
        out[0] = '\0';
}

/* The record is the owner's to read: a login session (the panel's
 * links open it as a page and as a download) or the token. */
static int record_ok(const struct _u_request *req, struct _u_response *res)
{
    if (!auth_origin_ok(req, res))
        return 0;
    if (auth_session_ok(req) || auth_write_permitted(req))
        return 1;
    reply_error(res, 403, "login required");
    return 0;
}

static const char *param(const struct _u_request *req, const char *key)
{
    const char *v = u_map_get(req->map_post_body, key);
    if (!v)
        v = u_map_get(req->map_url, key);
    return v;
}

static void head_info(char *hw_id, size_t hl, char *serial, size_t sl,
                      char *version, size_t vl)
{
    hw_id[0] = serial[0] = version[0] = '\0';
    const char *root = getenv("GF_SYSFS_ROOT");
    char path[256];
    snprintf(path, sizeof(path), "%s/head/info", root && *root ? root : "/sys/glowforge");
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = '\0';
        if (!strcmp(line, "hw_id"))
            snprintf(hw_id, hl, "%s", eq);
        else if (!strcmp(line, "serial"))
            snprintf(serial, sl, "%s", eq);
        else if (!strcmp(line, "version"))
            snprintf(version, vl, "%s", eq);
    }
    fclose(f);
}

/* The facts the machine wizard detects on its own. Returns a new object. */
static json_t *detected_facts(void)
{
    json_t *m = json_object();
    char fw[48] = "";
    read_fw_version(fw, sizeof(fw));
    json_object_set_new(m, "firmware", json_string(fw));
    json_object_set_new(m, "build", json_string(strstr(fw, "(dev)") ? "dev" : "release"));

    struct cam_status st;
    cam_get_status(&st);
    json_object_set_new(m, "camera",
                        json_string(st.sensor && st.sensor[0] ? st.sensor : "unknown"));

    char hw[24], ser[24], ver[24];
    head_info(hw, sizeof(hw), ser, sizeof(ser), ver, sizeof(ver));
    json_t *head = json_object();
    json_object_set_new(head, "present", json_boolean(hw[0] != '\0'));
    json_object_set_new(head, "hw_id", json_string(hw));
    json_object_set_new(head, "version", json_string(ver));
    char sh[SHEETID_LEN + 1] = "";
    if (ser[0])
        sheetid_derive(ser, sh, sizeof(sh));
    json_object_set_new(head, "serial_hash", json_string(sh));
    json_object_set_new(m, "head", head);
    return m;
}

/* The model choice and the TEC flag the machine wizard asks for. */
static int valid_model(const char *m)
{
    return m && (!strcmp(m, "basic") || !strcmp(m, "plus") || !strcmp(m, "pro"));
}

/* Apply a set of settings through the validated path. keys/vals are
 * parallel; a NULL val skips the key. Fills applied with {key: {from,
 * to}}. Returns 0, or -1 with the offending key in err. */
static int apply_settings(const char *const *keys, const char *const *vals,
                          size_t n, json_t *applied, char *err, size_t elen)
{
    const char *k[16], *v[16];
    size_t m = 0;
    for (size_t i = 0; i < n && m < 16; i++) {
        if (!vals[i])
            continue;
        if (!setting_valid(keys[i], vals[i])) {
            snprintf(err, elen, "invalid value for %s", keys[i]);
            return -1;
        }
        char from[192] = "";
        settings_get(keys[i], from, sizeof(from));
        json_t *e = json_object();
        json_object_set_new(e, "from", json_string(from));
        json_object_set_new(e, "to", json_string(vals[i]));
        json_object_set_new(applied, keys[i], e);
        k[m] = keys[i];
        v[m] = vals[i];
        m++;
    }
    if (m && settings_set_many(k, v, m) != 0) {
        snprintf(err, elen, "cannot write the settings");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------- status */

int cb_wiz_status(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;

    char cs[8192];
    if (commission_status_json(cs, sizeof(cs)) < 0)
        return reply_error(res, 500, "record unavailable");
    json_error_t jerr;
    json_t *st = json_loads(cs, 0, &jerr);
    if (!st)
        return reply_error(res, 500, "record unavailable");

    json_t *cat = json_array();
    for (size_t i = 0; i < NCATALOG; i++) {
        json_t *w = json_object();
        json_object_set_new(w, "id", json_string(catalog[i].id));
        json_object_set_new(w, "title", json_string(catalog[i].title));
        json_object_set_new(w, "version", json_integer(catalog[i].version));
        json_object_set_new(w, "class", json_string(catalog[i].cls));
        json_object_set_new(w, "done", json_integer(commission_wizard_version(catalog[i].id)));
        json_array_append_new(cat, w);
    }
    json_object_set_new(st, "wizards", cat);
    json_object_set_new(st, "changes", commission_changes_json());
    char ds[8192], who[SESSION_ID_HEX + 1];
    requester(req, who, sizeof(who));
    json_t *dark = wizdark_status_json(ds, sizeof(ds), who) >= 0 ? json_loads(ds, 0, NULL) : NULL;
    json_object_set_new(st, "dark", dark ? dark : json_null());

    json_t *u = json_object();
    char name[USERS_NAME_MAX + 1] = "";
    users_name(name, sizeof(name));
    json_object_set_new(u, "exists", json_boolean(users_exist()));
    json_object_set_new(u, "reset_pending", json_boolean(users_reset_pending()));
    json_object_set_new(u, "name", json_string(users_exist() ? name : ""));
    json_object_set_new(st, "users", u);

    json_object_set_new(st, "button", json_string(button_state()));
    json_object_set_new(st, "tls_fingerprint", json_string(tls_fingerprint()));
    json_object_set_new(st, "session", json_boolean(auth_session_ok(req)));

    json_t *clock = json_object();
    time_t now = time(NULL);
    char ts[32];
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
    json_object_set_new(clock, "utc", json_string(ts));
    json_object_set_new(clock, "set", json_boolean(now >= CLOCK_UNSET_BEFORE));
    json_object_set_new(st, "clock", clock);

    json_object_set_new(st, "machine", detected_facts());
    return reply_obj(res, 200, st);
}

int cb_wiz_record(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!record_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *dl = u_map_get(req->map_url, "download");
    char *text = commission_record_dump(dl != NULL);
    if (!text)
        return reply_error(res, 500, "record unavailable");
    if (dl) {
        char sid[SHEETID_LEN + 1] = "", fn[96];
        sheetid_get(sid, sizeof(sid));
        snprintf(fn, sizeof(fn), "attachment; filename=\"forgefirm-commissioning-%s.json\"",
                 sid[0] ? sid : "record");
        ulfius_add_header_to_response(res, "Content-Disposition", fn);
    }
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    ulfius_set_string_body_response(res, 200, text);
    free(text);
    return U_CALLBACK_COMPLETE;
}

/* The printable summary: the record rendered by recordhtml.c with the
 * documents' and the wizards' titles. */
int cb_wiz_record_html(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!record_ok(req, res))
        return U_CALLBACK_COMPLETE;
    json_t *rec = commission_record_copy();
    if (!rec)
        return reply_error(res, 500, "record unavailable");
    json_t *docs = json_array(), *wiz = json_array();
    size_t n;
    const advisory_t *dl = advisories_list(&n);
    for (size_t i = 0; i < n; i++)
        json_array_append_new(docs, json_pack("{s:s,s:s}", "id", dl[i].id, "title", dl[i].title));
    for (size_t i = 0; i < NCATALOG; i++)
        json_array_append_new(wiz, json_pack("{s:s,s:s}", "id", catalog[i].id,
                                             "title", catalog[i].title));
    char fw[64];
    read_fw_version(fw, sizeof(fw));
    char *page = record_html(rec, docs, wiz, fw);
    json_decref(rec);
    json_decref(docs);
    json_decref(wiz);
    if (!page)
        return reply_error(res, 500, "record unavailable");
    ulfius_add_header_to_response(res, "Content-Type", "text/html; charset=utf-8");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    ulfius_set_string_body_response(res, 200, page);
    free(page);
    return U_CALLBACK_COMPLETE;
}

/* What changed: the owner names a replaced part or a service; the
 * wizards it maps to are flagged and the status comes back. */
int cb_wiz_changed(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *what = param(req, "what");
    if (commission_change_apply(what) != 0)
        return reply_error(res, 400, "what must name a change from the menu");
    return cb_wiz_status(req, res, NULL);
}

/* --------------------------------------------------------- advisories */

int cb_advisory_get(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const advisory_t *d = advisories_find(u_map_get(req->map_url, "id"));
    if (!d)
        return reply_error(res, 404, "no such document");
    ulfius_set_binary_body_response(res, 200, (const char *)d->text, d->len);
    ulfius_add_header_to_response(res, "Content-Type", "text/markdown; charset=utf-8");
    ulfius_add_header_to_response(res, "ETag", d->hash);
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    return U_CALLBACK_CONTINUE;
}

int cb_wiz_agree(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *id = param(req, "doc");
    const char *hash = param(req, "hash");
    const char *phrase = param(req, "phrase");
    const advisory_t *d = advisories_find(id);
    if (!d)
        return reply_error(res, 400, "unknown document");
    if (!hash || strcmp(hash, d->hash))
        return reply_error(res, 409, "the document changed; read it again");
    if (!strcmp(d->consent, "typed")) {
        if (!phrase || strcmp(phrase, d->phrase))
            return reply_error(res, 400, "type the phrase exactly as shown");
    }
    if (commission_advisory_accept(id, hash, d->consent) != 0)
        return reply_error(res, 500, "cannot record the acceptance");
    fflog(LOG_NOTICE, "wiz: advisory '%s' accepted (%s)", id, d->consent);
    return cb_wiz_status(req, res, NULL);
}

int cb_wiz_press_start(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    if (!commission_advisories_complete())
        return reply_error(res, 409, "accept every document first");
    if (button_wait_start(PRESS_TIMEOUT_S) != 0 &&
        strcmp(button_state(), "waiting") != 0)
        return reply_error(res, 500, "cannot start the button wait");
    led_set(LED_BREATHE_TEAL);
    json_t *o = json_object();
    json_object_set_new(o, "button", json_string(button_state()));
    return reply_obj(res, 200, o);
}

int cb_wiz_press_status(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *state = button_state();
    int accepted = 0;
    if (!strcmp(state, "pressed") && button_take_pressed()) {
        led_release();
        if (commission_acceptance_pressed() == 0) {
            accepted = 1;
            fflog(LOG_NOTICE, "wiz: the advisories were accepted at the machine");
            /* The consent wizard is complete at this press. */
            commission_wizard_done("advisories", 1, NULL, NULL);
        }
        state = "pressed";
    } else if (!strcmp(state, "timeout") || !strcmp(state, "cancelled")) {
        led_release();
    }
    json_t *o = json_object();
    json_object_set_new(o, "button", json_string(state));
    json_object_set_new(o, "accepted", json_boolean(accepted || commission_acceptance_done()));
    return reply_obj(res, 200, o);
}

int cb_wiz_press_cancel(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    button_wait_cancel();
    led_release();
    json_t *o = json_object();
    json_object_set_new(o, "button", json_string(button_state()));
    return reply_obj(res, 200, o);
}

/* ------------------------------------------------------------ account */

int cb_wiz_account(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    if (!commission_acceptance_done())
        return reply_error(res, 409, "accept the advisories first");
    /* Once an account exists, only a logged-in session (or the reset)
     * may replace it. */
    if (users_exist() && !auth_session_ok(req))
        return reply_error(res, 403, "log in to change the account");
    const char *name = param(req, "name");
    const char *pw = param(req, "password");
    char reason[128];
    if (users_create(name, pw, reason, sizeof(reason)) != 0)
        return reply_error(res, 400, reason);
    commission_set_account(name, USERS_UID);
    commission_wizard_done("account", 1, NULL, NULL);

    /* Log this browser in. */
    char sid[SESSION_ID_HEX + 1], cookie[256];
    if (session_create(name, sid, sizeof(sid)) == 0) {
        session_cookie_set(sid, cookie, sizeof(cookie));
        ulfius_add_header_to_response(res, "Set-Cookie", cookie);
    }
    json_t *o = json_object();
    json_object_set_new(o, "ok", json_true());
    json_object_set_new(o, "name", json_string(name));
    return reply_obj(res, 200, o);
}

/* -------------------------------------------------------- preferences */

int cb_wiz_preferences(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *units = param(req, "ui_units");
    const char *country = param(req, "wifi_country");
    const char *clock = param(req, "clock");
    json_t *applied = json_object(), *result = json_object();
    char err[96];
    const char *keys[] = { "ui_units", "wifi_country" };
    const char *vals[] = { units, country };
    if (apply_settings(keys, vals, 2, applied, err, sizeof(err)) != 0) {
        json_decref(applied);
        json_decref(result);
        return reply_error(res, 400, err);
    }
    if (country)
        apply_wifi(1);

    /* The clock: only an unset clock takes the browser's time. A synced
     * clock is never moved by a page. */
    time_t now = time(NULL);
    int clock_set = 0;
    if (clock && *clock && now < CLOCK_UNSET_BEFORE) {
        char *end;
        long long t = strtoll(clock, &end, 10);
        if (*end == '\0' && t >= CLOCK_UNSET_BEFORE && t < 4102444800LL) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "date -s @%lld >/dev/null 2>&1", t);
            clock_set = system(cmd) == 0;
            if (clock_set)
                fflog(LOG_NOTICE, "wiz: clock set from the browser");
        }
    }
    json_object_set_new(result, "clock_was_set", json_boolean(now >= CLOCK_UNSET_BEFORE));
    json_object_set_new(result, "clock_set_from_browser", json_boolean(clock_set));
    commission_wizard_done("preferences", 1, result, applied);
    return cb_wiz_status(req, res, NULL);
}

/* ------------------------------------------------------------ machine */

int cb_wiz_machine(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *model = param(req, "model");
    const char *tec = param(req, "tec");
    if (!valid_model(model))
        return reply_error(res, 400, "model must be basic, plus, or pro");
    int tec_on = tec && !strcmp(tec, "1");
    if (tec_on && strcmp(model, "pro"))
        return reply_error(res, 400, "only a Pro has a thermoelectric cooler");

    json_t *applied = json_object();
    char err[96];
    const char *keys[] = { "cool_tec_present" };
    const char *vals[] = { tec_on ? "1" : "0" };
    if (apply_settings(keys, vals, 1, applied, err, sizeof(err)) != 0) {
        json_decref(applied);
        return reply_error(res, 400, err);
    }
    json_t *facts = detected_facts();
    json_object_set_new(facts, "model", json_string(model));
    json_object_set_new(facts, "tec", json_boolean(tec_on));
    json_t *result = json_deep_copy(facts);
    commission_set_machine(facts);
    commission_wizard_done("machine", 1, result, applied);
    return cb_wiz_status(req, res, NULL);
}

/* -------------------------------------------------------------- cloud */

int cb_wiz_cloud(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *enabled = param(req, "enabled");
    const char *phrase = param(req, "phrase");
    const char *homing = param(req, "homing_mode");
    const char *timeout = param(req, "gfcloud_home_timeout_s");
    const char *serial = param(req, "gf_serial");
    const char *password = param(req, "gf_password");
    int on = enabled && !strcmp(enabled, "1");

    json_t *applied = json_object(), *result = json_object();
    char err[96];
    if (on) {
        const advisory_t *d = advisories_find("cloud-service");
        if (!phrase || !d || strcmp(phrase, WIZ_CLOUD_PHRASE)) {
            json_decref(applied);
            json_decref(result);
            return reply_error(res, 400, "type I UNDERSTAND to turn cloud mode on");
        }
        if (homing && strcmp(homing, "gfcloud") && strcmp(homing, "none")) {
            json_decref(applied);
            json_decref(result);
            return reply_error(res, 400, "homing_mode must be gfcloud or none");
        }
        const char *keys[] = { "cloud_enabled", "homing_mode", "gfcloud_home_timeout_s",
                               "gf_serial", "gf_password" };
        const char *vals[] = { "1", homing ? homing : "gfcloud", timeout,
                               serial && *serial ? serial : NULL,
                               password && *password ? password : NULL };
        if (apply_settings(keys, vals, 5, applied, err, sizeof(err)) != 0) {
            json_decref(applied);
            json_decref(result);
            return reply_error(res, 400, err);
        }
        /* The record never holds the cloud password. */
        json_object_del(applied, "gf_password");
        json_object_set_new(result, "enabled", json_true());
        json_object_set_new(result, "signin", json_string("not tested"));
    } else {
        /* Off: nothing may point at the cloud. */
        char cur[16] = "";
        const char *keys[] = { "cloud_enabled", "homing_mode", "controller_mode" };
        const char *vals[3] = { "0", NULL, NULL };
        if (settings_get("homing_mode", cur, sizeof(cur)) == 0 && !strcmp(cur, "gfcloud"))
            vals[1] = "none";
        if (settings_get("controller_mode", cur, sizeof(cur)) == 0 && !strcmp(cur, "cloud"))
            vals[2] = "grbl";
        if (apply_settings(keys, vals, 3, applied, err, sizeof(err)) != 0) {
            json_decref(applied);
            json_decref(result);
            return reply_error(res, 400, err);
        }
        json_object_set_new(result, "enabled", json_false());
    }
    commission_wizard_done("cloud", 1, result, applied);
    return cb_wiz_status(req, res, NULL);
}

/* ----------------------------------------------------------- complete */

/* ------------------------------------------------ the dark wizards */

int cb_wiz_dark_start(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *id = u_map_get(req->map_url, "id");
    if (!wizdark_known(id))
        return reply_error(res, 404, "no such wizard");
    if (!commission_advisories_complete() || !commission_acceptance_done())
        return reply_error(res, 409, "accept the advisories first");
    char err[128], who[SESSION_ID_HEX + 1];
    requester(req, who, sizeof(who));
    if (wizdark_start(id, who, err, sizeof(err)) != 0)
        return reply_error(res, 409, err);
    json_t *o = json_object();
    json_object_set_new(o, "started", json_true());
    json_object_set_new(o, "id", json_string(id));
    return reply_obj(res, 200, o);
}

#define MIRROR_MSG "another browser is running this step; take it over to answer"

int cb_wiz_dark_answer(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *id = u_map_get(req->map_url, "id");
    const char *seq = param(req, "seq");
    const char *value = param(req, "value");
    if (!seq)
        return reply_error(res, 400, "seq is required");
    char who[SESSION_ID_HEX + 1];
    requester(req, who, sizeof(who));
    int rc = wizdark_answer(id, atoi(seq), value ? value : "", who);
    if (rc == -2)
        return reply_error(res, 409, MIRROR_MSG);
    if (rc != 0)
        return reply_error(res, 409, "no such prompt is open");
    return reply_json(res, 200, "{\"ok\":true}");
}

int cb_wiz_dark_abort(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char who[SESSION_ID_HEX + 1];
    requester(req, who, sizeof(who));
    if (wizdark_abort(who) == -2)
        return reply_error(res, 409, MIRROR_MSG);
    return reply_json(res, 200, "{\"ok\":true}");
}

/* A second browser takes the running step over: its session becomes
 * the owner, and the first browser mirrors from then on. */
int cb_wiz_dark_takeover(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char who[SESSION_ID_HEX + 1];
    requester(req, who, sizeof(who));
    wizdark_take_over(who);
    return reply_json(res, 200, "{\"ok\":true}");
}

int cb_wiz_dark_status(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char buf[8192], who[SESSION_ID_HEX + 1];
    requester(req, who, sizeof(who));
    if (wizdark_status_json(buf, sizeof(buf), who) < 0)
        return reply_error(res, 500, "status unavailable");
    return reply_json(res, 200, buf);
}

int cb_wiz_shot(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    size_t len = 0;
    uint8_t *jpeg = wizdark_shot(u_map_get(req->map_url, "cam"), &len);
    if (!jpeg)
        return reply_error(res, 404, "no snapshot yet");
    ulfius_set_binary_body_response(res, 200, (const char *)jpeg, len);
    ulfius_add_header_to_response(res, "Content-Type", "image/jpeg");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    free(jpeg);
    return U_CALLBACK_CONTINUE;
}

/* ------------------------------------------------ the sheet previews */

int cb_wiz_sheet_svg(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *id = u_map_get(req->map_url, "card");
    sheet_text_t out;
    sheet_text_init(&out);
    if (wizlive_preview(id ? id : "", &out) != 0) {
        sheet_text_free(&out);
        return reply_error(res, 404, "no such card");
    }
    ulfius_set_binary_body_response(res, 200, out.buf, out.len);
    ulfius_add_header_to_response(res, "Content-Type", "image/svg+xml");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    sheet_text_free(&out);
    return U_CALLBACK_CONTINUE;
}

int cb_wiz_sheet_gcode(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *id = u_map_get(req->map_url, "card");
    sheet_text_t out;
    sheet_text_init(&out);
    if (wizlive_program(id ? id : "", &out) != 0) {
        sheet_text_free(&out);
        return reply_error(res, 404, "no such card");
    }
    ulfius_set_binary_body_response(res, 200, out.buf, out.len);
    ulfius_add_header_to_response(res, "Content-Type", "text/plain; charset=utf-8");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    sheet_text_free(&out);
    return U_CALLBACK_CONTINUE;
}

void wiz_panel_opened(void)
{
    if (finish_lit) {
        finish_lit = 0;
        led_release();
    }
}

int cb_wiz_complete(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char why[256];
    if (!commission_gate_open(why, sizeof(why)) || commission_override_active()) {
        if (!commission_gate_open(why, sizeof(why)))
            return reply_error(res, 409, why[0] ? why : "not every step is complete");
    }
    if (commission_complete() != 0)
        return reply_error(res, 500, "cannot write the record");
    led_set(LED_SOLID_GREEN);
    finish_lit = 1;
    fflog(LOG_NOTICE, "wiz: commissioning complete");
    return cb_wiz_status(req, res, NULL);
}
