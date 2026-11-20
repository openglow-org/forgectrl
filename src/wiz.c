/*
 * wiz.c - the setup wizards and their routes
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The first run of the panel is a sequence of wizards, each a module
 * that can also run alone from the Setup tab. This file holds
 * the form wizards, the ones that need no hardware slot: advisories,
 * account, preferences, machine facts, and the cloud decision. Each
 * validates what the page sends, writes settings through the ordinary
 * validated path, and records its completion in the setup
 * record with what it found and what it wrote. The hardware wizards
 * (dark validation, the sheet) build on the same record and catalog.
 *
 * The one physical act here is the acceptance press: after the
 * documents, the button LED breathes teal and the page waits for a
 * release-to-press edge on the machine's button. The page can ask for it
 * again with a controller running (a document changed by an update), so
 * the press is a prompt like the panel's others (btnres.c): refused while
 * a job waits for its press, runs or is paused (job_takes_button), and
 * under its own reservation of the button for the whole wait, renewed by
 * the asking page's poll. The LED breathes and a press counts only once
 * the reservation has stood on file BTNRES_READY_S, and the wait has run
 * that long; who else waits is
 * asked again then and at the press, and a press that is somebody
 * else's ends the wait unsaid. The reservation stands WIZPKG_LINGER_S
 * after the press either way.
 *
 * The press is bound to the browser that asked for it. One request for
 * the press stands at a time, held by the browser that made it (the
 * ffpress cookie, which that browser alone has); another browser's
 * request is refused while it stands, and so are its take and its
 * cancel. A wait belongs to the request it was started for and ends when
 * that request ends; another browser's ask never joins it. The press is
 * taken in a POST by the holder alone, never by a read. While no account exists (a first run, or a reset until the
 * account step), the account step takes only the holder, and only after
 * its press was taken in this run of the daemon: an acceptance the
 * record already holds (a reset keeps it) is no press for the new
 * account. The owner at the machine cannot tell whose request the
 * breathing button answers, so the refusal on the owner's own page is
 * the sign that another browser asked first, and the page says not to
 * press. This host (loopback) is held to none of it: loopback carries the
 * machine's own processes, and an extension package cannot reach it (the
 * sandbox's network rules refuse every packet a package's account sends
 * through lo, forgefirm-sandbox ffx.nft). Making the account from none revokes
 * every scoped token, rotates the camera key and ends every session.
 */
#define _GNU_SOURCE
#include "wiz.h"
#include "advisories.h"
#include "auth.h"
#include "builtin.h"
#include "btnres.h"
#include "cool.h"
#include "extpkg.h"
#include "button.h"
#include "cam.h"
#include "setup.h"
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
#include "wizpkg.h"
#include "wizlive.h"
#include "testhook.h"
#include "camkey.h"
#include "tokens.h"
#include "sha256.h"

#include <ctype.h>
#include <fcntl.h>
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PRESS_TIMEOUT_S 600
#define CLOCK_UNSET_BEFORE 1700000000   /* a clock earlier than this is unset */

/* The press binding (see the top of this file). */
#define CLAIM_COOKIE "ffpress"
#define CLAIM_HEX 32
#define CLAIM_ACCOUNT_S 1800            /* a taken press holds the account step this long */
static pthread_mutex_t claim_mu = PTHREAD_MUTEX_INITIALIZER;
static char claim_id[CLAIM_HEX + 1];    /* "" when no request stands */
static double claim_until;              /* CLOCK_MONOTONIC seconds */
static int claim_pressed;               /* the holder's press was taken */

/* The press's wait and one request's life: PRESS_TIMEOUT_S, or a host
 * test's shorter one (GF_PRESS_TIMEOUT_S). */
static int press_timeout_s(void)
{
    const char *v = ff_test_hook("GF_PRESS_TIMEOUT_S");
    return v && atoi(v) > 0 ? atoi(v) : PRESS_TIMEOUT_S;
}

static double claim_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The ffpress cookie a request carries, into out ("" for none). */
static void claim_presented(const struct _u_request *req, char *out, size_t len)
{
    out[0] = '\0';
    const char *c = u_map_get_case(req->map_header, "Cookie");
    size_t k = strlen(CLAIM_COOKIE);
    while (c && *c) {
        while (*c == ' ' || *c == ';')
            c++;
        if (!strncmp(c, CLAIM_COOKIE, k) && c[k] == '=') {
            c += k + 1;
            size_t n = strcspn(c, "; ");
            if (n == CLAIM_HEX && n < len) {
                memcpy(out, c, n);
                out[n] = '\0';
            }
            return;
        }
        c += strcspn(c, ";");
    }
}

/* Under claim_mu: a request stands. */
static int claim_standing_locked(void)
{
    if (claim_id[0] && claim_now() >= claim_until) {
        claim_id[0] = '\0';
        claim_pressed = 0;
    }
    return claim_id[0] != '\0';
}

/* Under claim_mu: the request carries the standing request's cookie. */
static int claim_mine_locked(const struct _u_request *req)
{
    char p[CLAIM_HEX + 1];
    claim_presented(req, p, sizeof(p));
    return claim_standing_locked() && strlen(p) == CLAIM_HEX && ct_equal(p, claim_id, CLAIM_HEX);
}

static void claim_end(void)
{
    pthread_mutex_lock(&claim_mu);
    claim_id[0] = '\0';
    claim_pressed = 0;
    pthread_mutex_unlock(&claim_mu);
}

/* The press's reservation (see the top of this file), under press_mu. */
static pthread_mutex_t press_mu = PTHREAD_MUTEX_INITIALIZER;
static int press_in_force;              /* this wait came in force: the LED breathes, a press counts */
static int press_lingers;               /* a press was made: the reservation stands its linger */
static char press_why[160];             /* why this wait ended for somebody else's job */
static char press_claim[CLAIM_HEX + 1]; /* the request this wait was started for ("" this host's) */
static double press_began;              /* when this wait began, CLOCK_MONOTONIC seconds */

/* The request this wait was started for still stands and is the one
 * standing; a wait this host started is bound to none. */
static int press_request_stands(void)
{
    char bound[CLAIM_HEX + 1];
    pthread_mutex_lock(&press_mu);
    snprintf(bound, sizeof(bound), "%s", press_claim);
    pthread_mutex_unlock(&press_mu);
    if (!bound[0])
        return 1;
    pthread_mutex_lock(&claim_mu);
    int ok = claim_standing_locked() && ct_equal(bound, claim_id, CLAIM_HEX);
    pthread_mutex_unlock(&claim_mu);
    return ok;
}

/* The wait that stands was started for the request this browser holds. */
static int press_wait_mine(const struct _u_request *req)
{
    char bound[CLAIM_HEX + 1];
    pthread_mutex_lock(&press_mu);
    snprintf(bound, sizeof(bound), "%s", press_claim);
    pthread_mutex_unlock(&press_mu);
    pthread_mutex_lock(&claim_mu);
    int mine = bound[0] && claim_mine_locked(req) && ct_equal(bound, claim_id, CLAIM_HEX);
    pthread_mutex_unlock(&claim_mu);
    return mine;
}

/* button.h, button_wait_judge: the Setup page's wait, judged at every
 * reading of the button (the wait thread's 40 ms) and at a press. */
int wiz_press_judge(int at_press)
{
    char why[160], rwhy[96];
    if (!btnres_stands(BTNRES_SETUP)) {
        /* The page stopped asking, or the wait was canceled. A press
         * caught as the reservation lapsed is unsaid, and reserves the
         * button its linger again, as every press does. */
        if (at_press) {
            pthread_mutex_lock(&press_mu);
            press_lingers = 1;
            pthread_mutex_unlock(&press_mu);
            btnres_open(BTNRES_SETUP, WIZPKG_LINGER_S, rwhy, sizeof(rwhy));
        }
        return -1;
    }
    if (!press_request_stands()) {
        /* The request it was started for ended: its 600 s, the account,
         * a cancel. A press caught as it ended is unsaid, and keeps the
         * button reserved its linger as every press does. */
        if (at_press) {
            pthread_mutex_lock(&press_mu);
            press_lingers = 1;
            pthread_mutex_unlock(&press_mu);
            btnres_open(BTNRES_SETUP, WIZPKG_LINGER_S, rwhy, sizeof(rwhy));
        }
        return -1;
    }
    /* In force once the reservation has stood on file BTNRES_READY_S, and
     * no sooner than that after this wait began: a reservation another
     * wait left on file does not bring the teal back at once. */
    pthread_mutex_lock(&press_mu);
    int first = !press_in_force;
    double began = press_began;
    pthread_mutex_unlock(&press_mu);
    if (cool_button_on_file_s() < BTNRES_READY_S || claim_now() - began < BTNRES_READY_S)
        return 0;
    if ((first || at_press) && job_takes_button(why, sizeof(why))) {
        pthread_mutex_lock(&press_mu);
        snprintf(press_why, sizeof(press_why), "%s", why);
        press_lingers = at_press;
        pthread_mutex_unlock(&press_mu);
        if (at_press)
            btnres_open(BTNRES_SETUP, WIZPKG_LINGER_S, rwhy, sizeof(rwhy));
        else
            btnres_close(BTNRES_SETUP);
        return -1;
    }
    if (at_press) {
        pthread_mutex_lock(&press_mu);
        press_lingers = 1;
        pthread_mutex_unlock(&press_mu);
        btnres_open(BTNRES_SETUP, WIZPKG_LINGER_S, rwhy, sizeof(rwhy));
    } else if (first) {
        pthread_mutex_lock(&press_mu);
        press_in_force = 1;
        pthread_mutex_unlock(&press_mu);
        led_set(LED_BREATHE_TEAL);
    }
    return 1;
}

/* The wait is over unpressed: its reservation ends, but a linger. */
static void press_res_end(void)
{
    pthread_mutex_lock(&press_mu);
    int lingers = press_lingers;
    pthread_mutex_unlock(&press_mu);
    if (!lingers)
        btnres_close(BTNRES_SETUP);
}

/* button.h, button_wait_ended: however the wait ended (a press, its
 * timeout, its cancel, the judge), the teal it lit goes out and its
 * reservation ends, but the linger after a press. The wait leaves force
 * and its teal goes out in one hold of press_mu, the one the take's
 * teal is written under, so no take lights the teal again after it. */
void wiz_press_ended(void)
{
    pthread_mutex_lock(&press_mu);
    int lit = press_in_force;
    press_in_force = 0;
    if (lit)
        led_release();
    pthread_mutex_unlock(&press_mu);
    press_res_end();
}

/* The renewal the asking page's poll gives the reservation:
 * BTNRES_PANEL_TTL_S, or a host test's shorter one (GF_PRESS_RES_S). */
static double press_res_s(void)
{
    const char *v = ff_test_hook("GF_PRESS_RES_S");
    return v && atof(v) > 0 ? atof(v) : BTNRES_PANEL_TTL_S;
}

/* The page may ask for the press: in force, still waiting, and no job
 * that would take it as its own (a cloud print's armed session can be
 * reported meanwhile; the press would end the wait, unsaid). */
static int press_ready(void)
{
    char why[160];
    pthread_mutex_lock(&press_mu);
    int r = press_in_force;
    pthread_mutex_unlock(&press_mu);
    return r && !strcmp(button_state(), "waiting") && !job_takes_button(why, sizeof(why));
}

/* 0, or -1 with the refusal in why: a request from another browser
 * stands. This host is held to none of it. */
static int claim_refused(const struct _u_request *req, char *why, size_t len)
{
    if (auth_peer_local(req))
        return 0;
    pthread_mutex_lock(&claim_mu);
    int other = claim_standing_locked() && !claim_mine_locked(req);
    int left = other ? (int)(claim_until - claim_now()) + 1 : 0;
    pthread_mutex_unlock(&claim_mu);
    if (other)
        snprintf(why, len, "another browser asked for the press first: do not press the machine's button "
                 "for this page; that request ends in %d s, when its browser cancels it, or when the machine "
                 "restarts", left);
    return other ? -1 : 0;
}

/* The catalog: every wizard the page can show, in first-run order. The
 * version is the schema version the record stores on completion; the
 * step table in setup.c names the versions this image recommends. */
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
    { "airflow.intakes",     "Intake fans",         1, "dark" },
    { "switches",            "Switches",            1, "dark" },
    { "sensors",             "Sensors",             1, "dark" },
    { "airflow",             "Airflow",             1, "dark" },
    { "motion",              "Motion",              1, "dark" },
    { "cameras",             "Cameras",             1, "dark" },
    { "motion.envelope",     "Bed size",            1, "dark" },
    { "cooling.aa-offset",   "Coolant offset",      1, "dark" },
    { "cloud.header",        "Cloud header",        1, "dark" },
    { "lens.travel",         "Lens travel",         1, "dark" },
    { "laser.focus",         "Focus",               1, "live" },
    { "laser.floor",         "Laser floor",         1, "live" },
};
#define NCATALOG (sizeof(catalog) / sizeof(*catalog))

static void head_info(char *hw_id, size_t hl, char *serial, size_t sl,
                      char *version, size_t vl);
static json_t *head_facts(void);

/* A replaced head: its lens window was measured on the old head's
 * stops, so it goes, and the lens keeps the reduced window that clears
 * the stops on any head until Lens travel runs. */
static void head_window_clear(void)
{
    static const char *const keys[] = { "lens_stop_below_steps", "lens_stop_above_steps" };
    static const char *const vals[] = { "", "" };
    if (settings_set_many(keys, vals, 2) != 0)
        fflog(LOG_WARNING, "wiz: the lens window of the old head could not be cleared");
}

/* The head-change trigger: a record made with one head, booted with
 * another, recommends what a replaced head does: the machine facts, the
 * lens travel and the focus, motion and the cameras. The lens window was
 * measured on the old head's stops, so it goes (the lens keeps the
 * reduced window that clears the stops on any head until Lens travel
 * runs), and the record takes the new head's facts, so the change is
 * applied once. */
static void check_head_change(void)
{
    char recorded[32], hw[24], ser[24], ver[24], now[32] = "";
    setup_head_hash(recorded, sizeof(recorded));
    if (!recorded[0])
        return;
    head_info(hw, sizeof(hw), ser, sizeof(ser), ver, sizeof(ver));
    if (!ser[0])
        return;                     /* no head to compare: nothing to say */
    sheetid_derive(ser, now, sizeof(now));
    if (strcmp(recorded, now)) {
        fflog(LOG_NOTICE, "wiz: the head changed since the record was made");
        setup_change_apply("head");
        head_window_clear();
        setup_set_head(head_facts());
    }
}

/* The setup's recommended list names only steps this build has. */
static int catalog_known(const char *id)
{
    for (size_t i = 0; i < NCATALOG; i++)
        if (!strcmp(catalog[i].id, id))
            return 1;
    return 0;
}

/* The machine the record was made on: a seeded record takes this
 * machine's facts; one made on another machine (the dev SD card moved) asks
 * the machine step again and recommends what a replaced head does, the lens
 * window going with it, since every one of those was measured elsewhere. A
 * record with no machine facts and no seed is left alone: the machine step
 * writes them. Runs after the camera engine has resolved its sensors
 * (cam_engine_init), so the camera fact is the sensor's name; a record
 * whose camera fact is still unknown takes the name once the engine has
 * it. */
static json_t *detected_facts(void);
static void check_machine(void)
{
    char id[SHEETID_LEN + 1] = "";
    if (sheetid_get(id, sizeof(id)) == 0 && id[0]) {    /* else no serial to go by: nothing to say */
        int rc = setup_machine_check(id, detected_facts());
        if (rc == 1)
            fflog(LOG_NOTICE, "wiz: the seeded record had no machine facts: this machine's are in it");
        else if (rc == 2) {
            fflog(LOG_WARNING, "wiz: the record was made on another machine: the machine step is asked again, "
                  "and the lens window measured there is cleared");
            head_window_clear();
            setup_change_apply("head");
        }
    }
    struct cam_status st;
    cam_get_status(&st);
    if (setup_machine_camera(st.sensor) == 1)
        fflog(LOG_NOTICE, "wiz: the record's camera is %s", st.sensor);
}

void wiz_init(void)
{
    setup_known_hook = catalog_known;
    check_machine();
    check_head_change();
}

/* Finish lights the button solid green; the light goes out when the
 * control panel is first served after it, which is the operator's
 * next click. */
static int finish_lit;

void wiz_abort_all(void)
{
    button_wait_cancel();       /* the wait's end hook ends its reservation */
    claim_end();
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
    char id[SESSION_ID_HEX + 1], peer[SESSION_ADDR_MAX];
    auth_peer_text(req, peer, sizeof(peer));
    if (cookie && session_from_cookie(cookie, id, sizeof(id)) && session_valid(id, peer))
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
    const char *root = ff_test_hook("GF_SYSFS_ROOT");
    char path[256];
    snprintf(path, sizeof(path), "%s/head/info", root ? root : "/sys/glowforge");
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
        /* The head's hardware id is the module's "id" line (head_api.c). */
        if (!strcmp(line, "id"))
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

    json_object_set_new(m, "head", head_facts());
    return m;
}

/* The head's facts: present, the hardware id and the firmware, and the
 * serial's hash (the record never carries the serial). New object. */
static json_t *head_facts(void)
{
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
    return head;
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
    if (setup_status_json(cs, sizeof(cs)) < 0)
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
        json_object_set_new(w, "done", json_integer(setup_wizard_version(catalog[i].id)));
        json_array_append_new(cat, w);
    }
    json_object_set_new(st, "wizards", cat);
    /* The checks packages add: their own, recorded by nobody but them,
     * and none of them the machine's setup gate. */
    json_object_set_new(st, "extensions", wizpkg_catalog_json());
    json_object_set_new(st, "changes", setup_changes_json());
    char ds[8192], who[SESSION_ID_HEX + 1];
    requester(req, who, sizeof(who));
    json_t *dark = wizdark_status_json(ds, sizeof(ds), who) >= 0 ? json_loads(ds, 0, NULL) : NULL;
    json_object_set_new(st, "dark", dark ? dark : json_null());

    json_t *u = json_object();
    char name[USERS_NAME_MAX + 1] = "";
    users_name(name, sizeof(name));
    json_object_set_new(u, "exists", json_boolean(users_exist()));
    json_object_set_new(u, "reset_pending", json_boolean(users_reset_pending()));
    json_object_set_new(u, "fault", json_boolean(users_fault()));
    /* The name is the SSH login: a session's, or this host's, to read. */
    int may_name = users_exist() && (auth_session_ok(req) || auth_peer_local(req));
    json_object_set_new(u, "name", json_string(may_name ? name : ""));
    /* What a change of owner would have to be asked about. A package can
     * hold the last owner's credentials, so the account step offers to
     * take them; it is only asked when there is something to take. */
    if (users_reset_pending()) {
        int n = extpkg_count();
        json_object_set_new(u, "extension_packages", n < 0 ? json_null() : json_integer(n));
    }
    json_object_set_new(st, "users", u);

    json_object_set_new(st, "button", json_string(button_state()));
    json_object_set_new(st, "button_ready", json_boolean(press_ready()));
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
    char *text = setup_record_dump(dl != NULL);
    if (!text)
        return reply_error(res, 500, "record unavailable");
    if (dl) {
        char sid[SHEETID_LEN + 1] = "", fn[96];
        sheetid_get(sid, sizeof(sid));
        snprintf(fn, sizeof(fn), "attachment; filename=\"forgefirm-setup-%s.json\"",
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
    json_t *rec = setup_record_copy();
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
    /* A dev image's record says so, as every page does. */
    const char *bar = page_dev_bar_inline();
    char *body = bar[0] ? strstr(page, "<body>") : NULL;
    if (body) {
        size_t pre = (size_t)(body - page) + strlen("<body>");
        char *with = malloc(strlen(page) + strlen(bar) + 1);
        if (with) {
            memcpy(with, page, pre);
            strcpy(with + pre, bar);
            strcat(with, page + pre);
            free(page);
            page = with;
        }
    }
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
    if (setup_change_apply(what) != 0)
        return reply_error(res, 400, "what must name a change from the menu");
    /* A new head, as at boot: its window goes, and the record takes its
     * facts, so the boot check does not apply the change again (and clear
     * a window Lens travel measured on it). */
    if (!strcmp(what, "head")) {
        head_window_clear();
        setup_set_head(head_facts());
    }
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
    if (d->on_demand)
        return reply_error(res, 400, "this document is accepted where its feature is turned on");
    if (!hash || strcmp(hash, d->hash))
        return reply_error(res, 409, "the document changed; read it again");
    if (!strcmp(d->consent, "typed")) {
        if (!phrase || strcmp(phrase, d->phrase))
            return reply_error(res, 400, "type the phrase exactly as shown");
    }
    if (setup_advisory_accept(id, hash, d->consent) != 0)
        return reply_error(res, 500, "cannot record the acceptance");
    fflog(LOG_NOTICE, "wiz: advisory '%s' accepted (%s)", id, d->consent);
    return cb_wiz_status(req, res, NULL);
}

/* One ask at a time (cb_wiz_press_start): the refusal, the end of
 * another request's wait, the request and the wait's binding are one
 * step, so two browsers asking at once never both pass the refusal, and
 * no ask joins a wait another ask is ending. */
static pthread_mutex_t start_mu = PTHREAD_MUTEX_INITIALIZER;

static int press_start(const struct _u_request *req, struct _u_response *res)
{
    char why[200], id[CLAIM_HEX + 1] = "";
    if (claim_refused(req, why, sizeof(why)) != 0)
        return reply_error(res, 409, why);
    /* A job that waits for its press, runs or is paused would take this
     * press as its own. */
    if (job_takes_button(why, sizeof(why)))
        return reply_error(res, 409, why);
    /* A wait started for another request (one that lapsed, or this
     * host's) is not this browser's to join: it ends here, before this
     * ask's reservation, and this ask starts a wait of its own. */
    if (!auth_peer_local(req) && !strcmp(button_state(), "waiting") && !press_wait_mine(req))
        button_wait_cancel();
    if (btnres_open(BTNRES_SETUP, press_res_s(), why, sizeof(why)) != 0)
        return reply_error(res, 409, why);
    /* The request, this browser's: a new one, or the one it holds. */
    if (!auth_peer_local(req)) {
        unsigned char raw[CLAIM_HEX / 2];
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        int got = fd >= 0 && read(fd, raw, sizeof(raw)) == (ssize_t)sizeof(raw);
        if (fd >= 0)
            close(fd);
        if (!got) {
            press_res_end();
            return reply_error(res, 500, "no randomness for the press request");
        }
        /* One request lives press_timeout_s() from its first ask: the
         * holder's ask again starts a new wait, never a longer request. */
        double end = claim_now() + press_timeout_s();
        pthread_mutex_lock(&claim_mu);
        if (claim_mine_locked(req)) {
            snprintf(id, sizeof(id), "%s", claim_id);
            if (claim_until > end)
                claim_until = end;
        } else {
            for (size_t i = 0; i < sizeof(raw); i++)
                snprintf(id + 2 * i, 3, "%02x", raw[i]);
            snprintf(claim_id, sizeof(claim_id), "%s", id);
            claim_until = end;
        }
        claim_pressed = 0;
        pthread_mutex_unlock(&claim_mu);
    }
    /* A new wait comes in force afresh; one that stands keeps its state.
     * The wait is bound to this browser's request (a wait that stands is
     * this browser's own, above); this host's binds a new wait to none. */
    pthread_mutex_lock(&press_mu);
    int fresh = strcmp(button_state(), "waiting") != 0;
    if (fresh) {
        press_in_force = 0;
        press_lingers = 0;
        press_why[0] = '\0';
        press_began = claim_now();
    }
    if (fresh || id[0])
        snprintf(press_claim, sizeof(press_claim), "%s", id);
    pthread_mutex_unlock(&press_mu);
    if (button_wait_start(press_timeout_s()) != 0 &&
        strcmp(button_state(), "waiting") != 0) {
        press_res_end();
        return reply_error(res, 500, "cannot start the button wait");
    }
    if (id[0]) {
        char cookie[160];
        snprintf(cookie, sizeof(cookie), CLAIM_COOKIE "=%s; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=%d",
                 id, CLAIM_ACCOUNT_S);
        ulfius_add_header_to_response(res, "Set-Cookie", cookie);
    }
    json_t *o = json_object();
    json_object_set_new(o, "button", json_string(button_state()));
    json_object_set_new(o, "ready", json_boolean(press_ready()));
    return reply_obj(res, 200, o);
}

int cb_wiz_press_start(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    if (!setup_advisories_complete())
        return reply_error(res, 409, "accept every document first");
    pthread_mutex_lock(&start_mu);
    int rc = press_start(req, res);
    pthread_mutex_unlock(&start_mu);
    return rc;
}

/* GET: what the press is doing, for anyone who may read. It takes
 * nothing and lights nothing. */
int cb_wiz_press_status(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    pthread_mutex_lock(&claim_mu);
    int standing = claim_standing_locked(), mine = standing && claim_mine_locked(req);
    pthread_mutex_unlock(&claim_mu);
    json_t *o = json_object();
    json_object_set_new(o, "button", json_string(button_state()));
    json_object_set_new(o, "ready", json_boolean(press_ready()));
    json_object_set_new(o, "accepted", json_boolean(setup_acceptance_done()));
    json_object_set_new(o, "asked_by_other", json_boolean(standing && !mine && !auth_peer_local(req)));
    return reply_obj(res, 200, o);
}

/* POST: the holder's poll, which takes the press when it came. */
int cb_wiz_press_take(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char why[200];
    if (claim_refused(req, why, sizeof(why)) != 0)
        return reply_error(res, 409, why);
    int bound = !auth_peer_local(req);
    if (bound) {
        pthread_mutex_lock(&claim_mu);
        int mine = claim_mine_locked(req);
        pthread_mutex_unlock(&claim_mu);
        if (!mine) {
            /* A request cookie with no request standing for it: this
             * browser's request ended (its 600 s, the account step), and
             * a teal the machine shows now is not for this page. */
            char p[CLAIM_HEX + 1];
            claim_presented(req, p, sizeof(p));
            return reply_error(res, 409, p[0] ? "this browser's request for the press ended: do not press the "
                                                "machine's button for this page until it asks again"
                                              : "this browser has not asked for the press");
        }
    }
    const char *state = button_state();
    int accepted = 0;
    if (!strcmp(state, "pressed") && button_take_pressed()) {
        led_release();
        if (bound) {
            pthread_mutex_lock(&claim_mu);
            claim_pressed = 1;
            claim_until = claim_now() + CLAIM_ACCOUNT_S;
            pthread_mutex_unlock(&claim_mu);
        }
        if (setup_acceptance_pressed() == 0) {
            accepted = 1;
            fflog(LOG_NOTICE, "wiz: the advisories were accepted at the machine");
            /* The consent wizard is complete at this press. */
            setup_wizard_done("advisories", 1, NULL, NULL);
        }
        state = "pressed";
    } else if (!strcmp(state, "waiting")) {
        /* The press is still asked for: this poll keeps its reservation
         * (a page that stops polling loses it, and the wait with it), and
         * the teal once in force, should the cooling engine's red flash
         * have stood over it and ended dark, written under press_mu while
         * the wait is in force. A wait that ended has put its light and
         * its reservation away itself (wiz_press_ended);
         * the request stands to its own end, and its holder may ask
         * again meanwhile. */
        btnres_renew(BTNRES_SETUP, press_res_s());
        char jwhy[160];
        int job = job_takes_button(jwhy, sizeof(jwhy));
        pthread_mutex_lock(&press_mu);
        if (press_in_force && !job && !strcmp(button_state(), "waiting"))
            led_keep(LED_BREATHE_TEAL);
        pthread_mutex_unlock(&press_mu);
    }
    /* pressed: this browser's press was taken (this host's: the press
     * is on record). */
    int pressed = accepted || setup_acceptance_done();
    if (bound) {
        pthread_mutex_lock(&claim_mu);
        pressed = claim_mine_locked(req) && claim_pressed;
        pthread_mutex_unlock(&claim_mu);
    }
    json_t *o = json_object();
    json_object_set_new(o, "button", json_string(state));
    json_object_set_new(o, "ready", json_boolean(press_ready()));
    json_object_set_new(o, "accepted", json_boolean(accepted || setup_acceptance_done()));
    json_object_set_new(o, "pressed", json_boolean(pressed));
    pthread_mutex_lock(&press_mu);
    if (!strcmp(state, "cancelled") && press_why[0])
        json_object_set_new(o, "why", json_string(press_why));
    pthread_mutex_unlock(&press_mu);
    return reply_obj(res, 200, o);
}

int cb_wiz_press_cancel(const struct _u_request *req, struct _u_response *res, void *ud)
{
    (void)ud;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char why[200];
    if (claim_refused(req, why, sizeof(why)) != 0)
        return reply_error(res, 409, why);
    button_wait_cancel();       /* the wait's end hook ends its reservation */
    led_release();
    claim_end();
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
    if (!setup_acceptance_done())
        return reply_error(res, 409, "accept the advisories first");
    /* Once an account exists, only a logged-in session (or the reset)
     * may replace it. */
    if (users_exist() && !auth_session_ok(req))
        return reply_error(res, 403, "log in to change the account");
    if (users_fault() && !auth_peer_local(req))
        return reply_error(res, 409, "the account record cannot be read: hold the machine's button "
                           "through a restart to make the account again");
    /* With no account, the browser whose press was taken, and no other. */
    int from_none = !users_exist();
    if (from_none && !auth_peer_local(req)) {
        pthread_mutex_lock(&claim_mu);
        int held = claim_mine_locked(req) && claim_pressed;
        pthread_mutex_unlock(&claim_mu);
        if (!held)
            return reply_error(res, 409, "press the machine's button for this browser first");
    }
    const char *name = param(req, "name");
    const char *pw = param(req, "password");
    const char *wipe = param(req, "wipe_extensions");
    char reason[128];
    /* Whether this is a change of owner has to be read before the account
     * is made: making it is what ends the reset. */
    int changing_owner = users_reset_pending();
    if (users_create(name, pw, reason, sizeof(reason)) != 0)
        return reply_error(res, 400, reason);
    setup_set_account(name, USERS_UID);
    setup_wizard_done("account", 1, NULL, NULL);
    claim_end();

    /* An account made from none starts clean: no scoped token, camera
     * key or session made before it (by whoever reached the machine
     * first, or by the last owner) outlives it. */
    int revoked = 1, rotated = 1;
    if (from_none) {
        char terr[96];
        if (tokens_revoke_all(terr, sizeof(terr)) != 0) {
            revoked = 0;
            fflog(LOG_ERR, "wiz: the scoped tokens could not be revoked for the new account: %s", terr);
        }
        if (camkey_rotate() != 0) {
            rotated = 0;
            fflog(LOG_ERR, "wiz: the camera key could not be rotated for the new account");
        }
        session_end(NULL);
        fflog(LOG_NOTICE, "wiz: a new account: every scoped token revoked, the camera key rotated, every session ended");
    }

    /* The extension tree, when the new owner asked for it: every package,
     * its data, and every key the last owner added. They can hold that
     * owner's tokens and credentials, and a key of theirs would go on
     * making their packages read as trusted. The account is already the
     * new owner's, so a failure here is said and is not a refusal of it. */
    int wiped_pkgs = 0, wiped_keys = 0, wipe_failed = 0;
    char wipe_why[160] = "";
    if (changing_owner && wipe && !strcmp(wipe, "1")) {
        if (extpkg_wipe(&wiped_pkgs, &wiped_keys, wipe_why, sizeof(wipe_why)) != 0) {
            wipe_failed = 1;
            fflog(LOG_ERR, "wiz: the extension tree could not be wiped for the new owner: %s", wipe_why);
        } else {
            fflog(LOG_WARNING, "wiz: a new owner took the machine: %d extension package(s) and %d owner key(s) removed",
                  wiped_pkgs, wiped_keys);
        }
    }

    /* Log this browser in. */
    char sid[SESSION_ID_HEX + 1], cookie[256], peer[SESSION_ADDR_MAX];
    auth_peer_text(req, peer, sizeof(peer));
    if (session_create(name, peer, sid, sizeof(sid)) == 0) {
        session_cookie_set(sid, cookie, sizeof(cookie));
        ulfius_add_header_to_response(res, "Set-Cookie", cookie);
    }
    json_t *o = json_object();
    json_object_set_new(o, "ok", json_true());
    json_object_set_new(o, "name", json_string(name));
    if (from_none) {
        json_object_set_new(o, "tokens_revoked", json_boolean(revoked));
        json_object_set_new(o, "camera_key_rotated", json_boolean(rotated));
    }
    if (changing_owner && wipe && !strcmp(wipe, "1")) {
        json_t *w = json_object();
        json_object_set_new(w, "ok", json_boolean(!wipe_failed));
        json_object_set_new(w, "packages", json_integer(wiped_pkgs));
        json_object_set_new(w, "keys", json_integer(wiped_keys));
        if (wipe_failed)
            json_object_set_new(w, "error", json_string(wipe_why));
        json_object_set_new(o, "wiped", w);
    }
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
    setup_wizard_done("preferences", 1, result, applied);
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
    setup_set_machine(facts);
    setup_wizard_done("machine", 1, result, applied);
    return cb_wiz_status(req, res, NULL);
}

/* -------------------------------------------------------------- cloud */

/* The cloud step turning the cloud off, as a settings request. */
static const char *cloud_off_param(void *ctx, const char *key)
{
    (void)ctx;
    return !strcmp(key, "cloud_enabled") ? "0" : NULL;
}

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
        if (homing && !setup_cloud_homing_ok(homing)) {
            json_decref(applied);
            json_decref(result);
            return reply_error(res, 400, "homing_mode must be gfcloud, manual, or none");
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
        /* Off: nothing may point at the cloud. The table of built-ins
         * knows what does, and what each falls back to. */
        const char *keys[5] = { "cloud_enabled" }, *vals[5] = { "0" };
        size_t n = 1 + builtin_request_sweep(cloud_off_param, NULL, keys + 1, vals + 1, 4);
        if (apply_settings(keys, vals, n, applied, err, sizeof(err)) != 0) {
            json_decref(applied);
            json_decref(result);
            return reply_error(res, 400, err);
        }
        json_object_set_new(result, "enabled", json_false());
    }
    setup_wizard_done("cloud", 1, result, applied);
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
    if (!setup_advisories_complete() || !setup_acceptance_done())
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
    /* The guided run ends here, finished or set aside ("Start using the
     * machine", "Finish later"): it needs the gate open (the advisories,
     * the account, the required steps, the intake fan count) and nothing
     * else. The button lights green only for a finished one. */
    char why[256];
    if (!setup_gate_open(why, sizeof(why)))
        return reply_error(res, 409, why[0] ? why : "the setup's required steps are not done");
    if (setup_complete() != 0)
        return reply_error(res, 500, "cannot write the record");
    int left = setup_recommended_count();
    if (left == 0) {
        led_set(LED_SOLID_GREEN);
        finish_lit = 1;
        fflog(LOG_NOTICE, "wiz: setup complete");
    } else
        fflog(LOG_NOTICE, "wiz: setup set aside with %d step%s recommended", left, left == 1 ? "" : "s");
    return cb_wiz_status(req, res, NULL);
}
