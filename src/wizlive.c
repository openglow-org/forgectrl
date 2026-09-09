/*
 * wizlive.c - the sheet wizards: the machine's numbers burned into wood
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The live half of the commissioning: one sheet of wood, the head at
 * its home as the datum, and one press per card. Every wizard here
 * runs on the runner's worker (wizdark.c) with the same prompts and
 * record, takes the controller in loopback posture, references the
 * lens on its hall sensor, streams its program through jobstream with
 * the emission witnesses sampled, and writes what it measured through
 * the validated settings path. The laser latch opens only through the
 * controller's arm gate with the operator's press; every cooling gate
 * stands; a lid opened mid-card cancels the program, as any job.
 *
 * Wizards: sheet.place (the datum), sheet.frame (the first emission,
 * the mark dose), laser.focus, laser.floor, laser.dose-curve,
 * laser.corner, cooling.flow-load. The sheet carries the patterns only:
 * the numbers they yield live in the record and on the page.
 */
#define _GNU_SOURCE
#include "wizcalc.h"
#include "wizrun.h"
#include "cam.h"
#include "commission.h"
#include "cool.h"
#include "curverec.h"
#include "accel.h"
#include "fflog.h"
#include "hooks.h"
#include "jobstream.h"
#include "led.h"
#include "settings.h"
#include "sheet.h"
#include "sheetid.h"
#include "status.h"
#include "super.h"
#include "wizlive.h"

#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* The lens. Z is the focal point's height above the tray: Z 0 focuses
 * on the bed, Z 3 on the top of 3 mm material. The lens is a 2 in lens
 * under a collimated beam, so the focal point moves 1:1 with the lens
 * and +Z is lens up. Every head shares the screw (36 half-steps over the
 * carriage's 0.485 in, the head drawing) and the travel; what differs
 * from head to head is the step along the travel at which the hall
 * sensor trips. That rising edge (the first position reading home going
 * up) is the one reference every live session takes, and the focus card
 * measures the one number the head needs: the focal height when the
 * lens sits on it (lens_hall_edge_z_mm). The wizard counts the lens in
 * half-steps from that edge, up positive. The focus card also finds the
 * head's two stops by the head accelerometer, one half-step at a time
 * from the edge, without a slip (lens_stop_below_steps and
 * lens_stop_above_steps, the free half-steps each way); when they cannot
 * be found on a head, every move keeps a fallback window that clears the
 * stops on any head, and the user is told. */
#define TRAVEL_MM     12.32     /* the carriage's travel, 0.485 in */
#define TRAVEL_HALF   36        /* the travel in half-steps of the screw, a half-step of leeway at each end */
#define Z_MM_PER_HALF (TRAVEL_MM / TRAVEL_HALF)   /* the screw's scale, 1/$102 */
#define Z_STEP_MS     180       /* the direct-step cadence of every lens reference and the finder */
#define EDGE_Z_DEFAULT 3.35     /* the bench reference machine's edge, until this head's is measured */
#define LENS_DOWN     10        /* the fallback window: half-steps below the edge every head reaches */
#define LENS_UP       12        /* and above it (the bench reference machine's stops sit 18 below and 20 above) */
static double edge_z(void);
static double z_of_k(double k);
static void lens_window(int *down, int *up);
#define FLOOR_STEP    2.0
/* Every look at the sheet carries this: the cards share one datum. */
#define STILL "Look, but do not move the sheet: every card after this is placed from the same datum."

#define CORNER_S      300
#define CORNER_FEED   2000
#define PATCH_S       600
#define PATCH_FEED    1500
#define WAIT_TIMEOUT_S 600.0    /* the arm and the press */
#define PICK_TIMEOUT_S 1800     /* a look at the sheet after a burn: the burn is not repeated */
#define RUN_TIMEOUT_S 1500.0
#define DOSE_DARK_S    20.0     /* the recorder's end rule */
#define LOAD_TAIL_S   100.0     /* the tube's heat reaches the sensor 10 to 60 s late */
#define SETTLE_MIN_S   60.0
#define SETTLE_MAX_S  240.0
#define SETTLE_SLOPE   0.02     /* C per second over the last 30 s: quiet */
#define TP_DELTA_MIN  100       /* the thermopile must rise this much under the mark */
#define S25_MAX      37500      /* 25 minutes of 25 Hz samples */
#define S1_MAX        1600

/* ------------------------------------------------------------- facts */

typedef struct {
    int have, alone;
    long ox, oy;
    double thickness;
} place_t;

static void place_read(place_t *p)
{
    memset(p, 0, sizeof(*p));
    json_t *r = commission_wizard_result("sheet.place");
    if (!r)
        return;
    p->have = 1;
    p->ox = (long)json_integer_value(json_object_get(r, "origin_x"));
    p->oy = (long)json_integer_value(json_object_get(r, "origin_y"));
    p->alone = json_is_true(json_object_get(r, "alone"));
    p->thickness = json_number_value(json_object_get(r, "thickness_mm"));
    json_decref(r);
}

static double result_num(const char *wiz, const char *key, double def)
{
    json_t *r = commission_wizard_result(wiz);
    if (!r)
        return def;
    json_t *v = json_object_get(r, key);
    double out = json_is_number(v) ? json_number_value(v) : def;
    json_decref(r);
    return out;
}

static int mark_s(void)
{
    int s = (int)result_num("sheet.frame", "mark_s", SHEET_MARK_S);
    return s < 50 ? 50 : s > 1000 ? 1000 : s;
}

static int mark_feed(void)
{
    int f = (int)result_num("sheet.frame", "mark_feed", SHEET_MARK_FEED);
    return f < 100 ? 100 : f > 6000 ? 6000 : f;
}

/* The Z the cards run at: the focal point on the sheet's surface, which
 * is Z = the thickness, kept inside the window the lens reaches from
 * its reference. */
static double focus_z(const place_t *p)
{
    int down, up;
    lens_window(&down, &up);
    double lo = z_of_k(-down), hi = z_of_k(up);
    return p->thickness < lo ? lo : p->thickness > hi ? hi : p->thickness;
}

static void now_utc(char *buf, size_t len, const char *fmt)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, len, fmt, &tm);
}

typedef struct {
    char version[48], build[8], model[16], camera[24], sheet_id[24], started[24];
} header_bufs_t;

static void header_facts(sheet_header_t *h, header_bufs_t *b)
{
    read_fw_version(b->version, sizeof(b->version));
    snprintf(b->build, sizeof(b->build), "%s", strstr(b->version, "(dev)") ? "dev" : "release");
    char *p = strstr(b->version, " (dev)");
    if (p)
        *p = '\0';
    commission_machine_str("model", b->model, sizeof(b->model));
    if (!b->model[0])
        snprintf(b->model, sizeof(b->model), "model ?");
    struct cam_status st;
    cam_get_status(&st);
    snprintf(b->camera, sizeof(b->camera), "%s", st.sensor && st.sensor[0] ? st.sensor : "camera ?");
    if (sheetid_get(b->sheet_id, sizeof(b->sheet_id)) != 0)
        b->sheet_id[0] = '\0';
    now_utc(b->started, sizeof(b->started), "%Y-%m-%d %H:%MZ");
    h->version = b->version;
    h->build = b->build;
    h->model = b->model;
    h->camera = b->camera;
    h->sheet_id = b->sheet_id;
    h->started = b->started;
}

/* The smallest piece that holds the sheet, in the user's units: whole
 * inches rounded up, or millimeters as they are. */
static const char *sheet_size_text(char *buf, size_t n)
{
    if (wiz_imperial())
        snprintf(buf, n, "%.0f x %.0f in", ceil(SHEET_W / 25.4 - 0.05), ceil(SHEET_H / 25.4 - 0.05));
    else
        snprintf(buf, n, "%.0f x %.0f mm", (double)SHEET_W, (double)SHEET_H);
    return buf;
}

/* The frame's size in the user's units. */
static const char *frame_size_text(char *buf, size_t n)
{
    if (wiz_imperial())
        snprintf(buf, n, "%.1f x %.1f in", SHEET_FRAME_W / 25.4, SHEET_FRAME_H / 25.4);
    else
        snprintf(buf, n, "%.0f x %.0f mm", (double)SHEET_FRAME_W, (double)SHEET_FRAME_H);
    return buf;
}

/* A wait the page can follow: the phase names it and counts the
 * seconds, of the most it takes. 0 held, -1 aborted, -2 timed out. */
static int wait_named(const char *what, int (*cond)(void *), void *ctx, int timeout_s)
{
    for (int i = 0; i < timeout_s; i++) {
        wiz_phase("%s: %d s (%d at most)", what, i, timeout_s);
        int rc = wiz_wait_for(cond, ctx, 1);
        if (rc != -2)
            return rc;
    }
    return -2;
}

/* --------------------------------------------------- counters, steps */

static int read_counters(long *x, long *y, long *z)
{
    const char *r = getenv("GF_SYSFS_ROOT");
    char path[192];
    snprintf(path, sizeof(path), "%s/cnc/position", r && *r ? r : "/sys/glowforge");
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int32_t v[3];
    ssize_t n = read(fd, v, sizeof(v));
    close(fd);
    if (n < (ssize_t)sizeof(v))
        return -1;
    *x = v[0];
    *y = v[1];
    *z = v[2];
    return 0;
}

static int steps_per_mm(double *sx, double *sy, double *sz)
{
    char buf[8192];
    if (grbl_settings_text(buf, sizeof(buf)) < 0)
        return -1;
    *sx = *sy = *sz = 0;
    for (char *l = strtok(buf, "\r\n"); l; l = strtok(NULL, "\r\n")) {
        if (!strncmp(l, "$100=", 5))
            *sx = atof(l + 5);
        else if (!strncmp(l, "$101=", 5))
            *sy = atof(l + 5);
        else if (!strncmp(l, "$102=", 5))
            *sz = atof(l + 5);
    }
    return *sx > 0 && *sy > 0 && *sz > 0 ? 0 : -1;
}

/* ----------------------------------------------------- the lens frame */

/* The controller's Z steps per mm, read when a session is live. */
static double z_spm = 1.0 / Z_MM_PER_HALF;

/* The focal height when the lens is on the hall edge: the focus card's
 * number for this head, else the bench reference machine's. */
static double edge_z(void)
{
    double z = wiz_setting_num("lens_hall_edge_z_mm", -1000.0);
    return z < -500.0 ? EDGE_Z_DEFAULT : z;
}

/* Lens half-steps from the hall edge (up positive) into Z. */
static double z_of_k(double k)
{
    return edge_z() + k / z_spm;
}

/* The window the lens moves in, half-steps below and above the edge:
 * the live session's own finding, else the focus card's settings for
 * this head, else the fallback. */
static int session_down, session_up;

static void lens_window(int *down, int *up)
{
    if (session_down > 0 && session_up > 0) {
        *down = session_down;
        *up = session_up;
        return;
    }
    double b = wiz_setting_num("lens_stop_below_steps", 0);
    double a = wiz_setting_num("lens_stop_above_steps", 0);
    *down = b >= 1 && b <= 40 ? (int)b : LENS_DOWN;
    *up = a >= 1 && a <= 40 ? (int)a : LENS_UP;
}

/* ---------------------------------------------------- the lens stops */

/* The head's stops, found by the head accelerometer one half-step at a
 * time from the hall edge: a free step rings on every second half-step,
 * and at a stop the ring dies two to four steps before the rotor slips
 * (the bench reference machine, 2026-09-06). Contact is called on the
 * first quiet strong-parity step, the lens backs off two, and the count
 * back to the edge must match the steps taken. A slip, an unreadable
 * ring, or no contact within reach means the stops are not findable on
 * this head: the fallback window stands and the user is told. */
#define FIND_MAX       30
#define FIND_MIN       8        /* a stop nearer the edge than this is a misread: the factory's own
                                   zero sits 8 half-steps below the edge on every head */
#define FIND_FLOOR     6000L    /* the strong level a readable ring has at least, summed */
#define FIND_BACK      2
#define FIND_QUIET_S   10       /* the fans off this long before the listening */
#define FIND_WINDOW_MS 170
#define FIND_TOL       1

typedef struct {
    int found;
    int below, above;               /* free half-steps from the edge, when found */
    int contact_below, contact_above;
    char why[120];
} zstops_t;

static int z_hall_home(void)
{
    long h = wiz_rd_long("head/hall_sensor", -1);
    return h < 0 ? -1 : h == 0;
}

/* Step `up` until the hall reads `home`; the steps taken, or -1. */
static int z_until(int home, int up, int max)
{
    for (int c = 0; c < max; c++) {
        int h = z_hall_home();
        if (h < 0)
            return -1;
        if (h == home)
            return c;
        if (wiz_wr_attr("cnc/z_step", up ? "1" : "0") != 0)
            return -1;
        wiz_msleep(Z_STEP_MS);
        if (wiz_aborted())
            return -1;
    }
    return -1;
}

/* One half-step with the accelerometer listened to through the cadence
 * window: the three axes' peak-to-peak, summed. */
static int z_step_listen(int up, long *sum)
{
    long lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 }, v[3];
    int n = 0;
    struct timespec t0, t;
    if (wiz_wr_attr("cnc/z_step", up ? "1" : "0") != 0)
        return -1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (crash_hw_burst(&v[0], &v[1], &v[2]) != 0)
            return -1;
        for (int i = 0; i < 3; i++) {
            if (n == 0 || v[i] < lo[i])
                lo[i] = v[i];
            if (n == 0 || v[i] > hi[i])
                hi[i] = v[i];
        }
        n++;
        clock_gettime(CLOCK_MONOTONIC, &t);
        long ms = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms >= FIND_WINDOW_MS)
            break;
    }
    wiz_msleep(Z_STEP_MS - FIND_WINDOW_MS);
    if (wiz_aborted())
        return -1;
    *sum = (hi[0] - lo[0]) + (hi[1] - lo[1]) + (hi[2] - lo[2]);
    return 0;
}

/* One leg from the edge toward a stop; the lens is backed off after
 * whatever was seen. 0 with *contact, else -1 with `why`. */
static int z_find_leg(int up, int *contact, char *why, size_t wlen)
{
    long sums[FIND_MAX];
    int n = 0, slipped = 0, call = 0, rc = 0;
    const char *which = up ? "top" : "bottom";
    while (n < FIND_MAX && !call) {
        wiz_phase("finding the %s stop by the head accelerometer: %d half-steps %s the edge",
                  which, n + 1, up ? "above" : "below");
        if (z_step_listen(up, &sums[n]) != 0) {
            snprintf(why, wlen, "the head accelerometer could not be read");
            rc = -1;
            break;
        }
        n++;
        call = wizcalc_stop_call(sums, n, &slipped);
    }
    for (int i = 0; i < FIND_BACK; i++) {
        wiz_wr_attr("cnc/z_step", up ? "0" : "1");
        wiz_msleep(Z_STEP_MS);
    }
    /* The ring, step by step in hundreds, for the record and for a head
     * whose stops could not be found; the log line is short, so two of
     * them. */
    for (int from = 0; from < n; from += 15) {
        char ring[15 * 5 + 1];
        int len = 0;
        for (int i = from; i < n && i < from + 15; i++)
            len += snprintf(ring + len, sizeof(ring) - len, "%s%ld", i > from ? " " : "",
                            (sums[i] + 50) / 100);
        wiz_log("%s ring %d-%d (x100): %s", which, from + 1, n < from + 15 ? n : from + 15, ring);
    }
    if (rc != 0)
        return -1;
    if (!call) {
        snprintf(why, wlen, "no %s stop within %d half-steps of the edge", which, FIND_MAX);
        return -1;
    }
    if (slipped) {
        snprintf(why, wlen, "the lens slipped at the %s stop before its ring died", which);
        return -1;
    }
    if (!wizcalc_ring_readable(sums, call, FIND_FLOOR)) {
        snprintf(why, wlen, "the lens steps do not ring clearly enough on this head");
        return -1;
    }
    if (call - FIND_BACK < FIND_MIN) {
        snprintf(why, wlen, "the %s stop was called %d half-steps from the edge, nearer than any head's",
                 which, call - FIND_BACK);
        return -1;
    }
    wiz_log("the %s stop: the ring died %d half-steps %s the edge", which, call, up ? "above" : "below");
    *contact = call;
    return 0;
}

/* Both stops, from the edge, at the run current in half-step mode; the
 * lens ends on the edge. The finding is filed in `r` under "stops". */
static int z_find_stops(zstops_t *st, json_t *r)
{
    memset(st, 0, sizeof(*st));
    int opened = 0;
    wiz_wr_attr("head/z_current", "0");
    wiz_wr_attr("head/z_mode", "1");
    wiz_wr_attr("head/z_enable", "0");
    /* Every fan off, and a set time for them to run down, before the
     * accelerometer is listened to: a running fan is in the reading. */
    cool_quiet_hold(1);
    for (int i = 0; i < FIND_QUIET_S; i++) {
        wiz_phase("the fans stop before the listening: %d s of %d", i, FIND_QUIET_S);
        wiz_msleep(1000);
        if (wiz_aborted()) {
            snprintf(st->why, sizeof(st->why), "aborted");
            goto done;
        }
    }
    /* The listener takes the crash watch's bus handle, or opens its
     * own for the finding and closes it after. */
    opened = crash_hw_listen(1) == 0;
    if (!opened) {
        snprintf(st->why, sizeof(st->why), "the head accelerometer does not answer on its bus");
        goto done;
    }
    int c;
    if (z_find_leg(0, &st->contact_below, st->why, sizeof(st->why)) != 0)
        goto done;
    /* The count back to the edge is the steps taken, or something slipped. */
    c = z_until(1, 1, 80);
    if (c < 0 || c < st->contact_below - FIND_BACK - FIND_TOL || c > st->contact_below - FIND_BACK + FIND_TOL) {
        snprintf(st->why, sizeof(st->why), "the count back from the bottom stop was %d for %d steps taken",
                 c, st->contact_below - FIND_BACK);
        goto done;
    }
    if (z_find_leg(1, &st->contact_above, st->why, sizeof(st->why)) != 0)
        goto done;
    /* Down until the hall lets go (the steps taken plus its band), then
     * up to the edge again. */
    c = z_until(0, 0, 80);
    if (c < 0 || c - (st->contact_above - FIND_BACK) < 2 || c - (st->contact_above - FIND_BACK) > 9) {
        snprintf(st->why, sizeof(st->why), "the count back from the top stop was %d for %d steps taken",
                 c, st->contact_above - FIND_BACK);
        goto done;
    }
    if (z_until(1, 1, 60) < 0) {
        snprintf(st->why, sizeof(st->why), "the hall edge was lost after the top stop");
        goto done;
    }
    st->below = st->contact_below - FIND_BACK;
    st->above = st->contact_above - FIND_BACK;
    st->found = 1;
done:
    if (opened)
        crash_hw_listen(0);
    cool_quiet_hold(0);
    if (!st->found) {
        wiz_log("the stops could not be found: %s", st->why);
        /* Whatever happened, the lens ends on the edge. */
        if (z_hall_home() == 1)
            z_until(0, 0, 60);
        z_until(1, 1, 80);
    }
    json_object_set_new(r, "stops",
                        json_pack("{s:b,s:i,s:i,s:i,s:i,s:s}", "found", st->found, "below", st->below,
                                  "above", st->above, "contact_below", st->contact_below,
                                  "contact_above", st->contact_above, "why", st->why));
    return 0;
}

/* ------------------------------------------------------- the session */

typedef struct {
    int stopped, local;
    place_t place;
    double spmx, spmy, spmz;
    long cx, cy, cz;            /* the counters at the start */
    float dx, dy;               /* the card's offset when alone */
    int z_taken;                /* the lens unlocked for the session */
    int find_stops;             /* the focus card: find the stops after the reference */
    zstops_t stops;
} live_t;

/* The cooling engine holds the new controller's report and allows fire:
 * a controller that has just started has no verdict of its own yet, and
 * its arm gate refuses (ALARM:3) on a verdict that is not fresh. The
 * engine takes the report and re-computes within a second; the driver
 * re-reads the verdict twice a second. */
static int engine_ready(void *ctx)
{
    (void)ctx;
    double age = cool_report_age();
    if (age < 0 || age > 3.0)
        return 0;
    char st[COOL_STATUS_JSON_MAX];
    if (cool_status_json(st, sizeof(st)) != 0)
        return 0;
    return strstr(st, "\"fire_ok\":true") != NULL && strstr(st, "\"hold\":false") != NULL;
}

/* Take the machine: the controller stopped, the lens referenced, the
 * counters read, the controller back in loopback posture with its
 * settings published and the cooling engine ready for its arm. `card`
 * alone-offsets when the placement says so. */
static int live_begin(live_t *L, const sheet_card_t *card, int need_place, int zpasses, json_t *r)
{
    int find_stops = L->find_stops;
    memset(L, 0, sizeof(*L));
    L->find_stops = find_stops;
    place_read(&L->place);
    if (need_place && !L->place.have) {
        wiz_finish_err("place the sheet first (the Place the sheet step)");
        return -1;
    }
    if (card && L->place.alone)
        sheet_alone_offset(card, &L->dx, &L->dy);
    wiz_phase("stopping the motion controller");
    if (wiz_controller_stop() != 0) {
        wiz_finish_err("could not stop the motion controller");
        return -1;
    }
    L->stopped = 1;
    if (zpasses > 0) {
        wiz_phase("the lens finds its reference on the hall sensor");
        int rc = wiz_z_reference(r, zpasses);
        if (rc != 0) {
            wiz_finish_err(rc == -2 ? "the lens never reached the hall sensor in 200 steps"
                           : rc == -3 ? "the lens reference passes disagree"
                                      : "the lens reference could not run");
            return -1;
        }
        if (L->find_stops) {
            z_find_stops(&L->stops, r);
            session_down = L->stops.found ? L->stops.below : LENS_DOWN;
            session_up = L->stops.found ? L->stops.above : LENS_UP;
        }
    }
    if (read_counters(&L->cx, &L->cy, &L->cz) != 0) {
        wiz_finish_err("cannot read the position counters");
        return -1;
    }
    super_set_local(1);
    L->local = 1;
    wiz_controller_start();
    L->stopped = 0;
    if (wait_named("starting the motion controller for the card", wiz_grbl_up, NULL, 60) != 0) {
        wiz_finish_err("the controller did not come up");
        return -1;
    }
    for (int i = 0; i < 50 && steps_per_mm(&L->spmx, &L->spmy, &L->spmz) != 0; i++)
        wiz_msleep(100);
    if (L->spmx <= 0) {
        wiz_finish_err("the controller's settings are not published");
        return -1;
    }
    if (L->spmz > 0)
        z_spm = L->spmz;
    int rc = wait_named("the cooling engine takes the controller's report", engine_ready, NULL, 20);
    if (rc == -1)
        return -1;
    if (rc != 0) {
        wiz_finish_err("the cooling engine did not take the controller's report, or holds fire");
        return -1;
    }
    wiz_msleep(1200);               /* the driver's next read of the verdict */
    /* The driver starts with the lens locked out of the pulse path (the
     * factory's posture: a sender's Z never moves the lens) and the lens
     * motor at its hold current. The cards move the lens from the
     * reference the session just took, inside the travel, so the session
     * lifts the lock and drives the motor at its run current; the end
     * puts both back. */
    /* Taken as soon as the first write lands, so a later one failing
     * still hands the motor back at the end: the lock is lifted by then,
     * and leaving it lifted would let a sender's Z drive the lens. */
    L->z_taken = 1;
    if (wiz_wr_attr("cnc/motor_lock", "0") != 0 || wiz_wr_attr("head/z_current", "0") != 0 ||
        wiz_wr_attr("head/z_mode", "1") != 0 || wiz_wr_attr("head/z_enable", "0") != 0) {
        wiz_finish_err("cannot take the lens motor for the session");
        return -1;
    }
    return 0;
}

static void live_end(live_t *L)
{
    /* Whatever the exit, the pulse engine finishes before the controller
     * is stopped; a stop at Idle would discard the tail. */
    wiz_phase("the machine goes back to its normal posture");
    wiz_kernel_wait_idle(600.0);
    session_down = session_up = 0;
    if (L->z_taken) {
        wiz_wr_attr("cnc/motor_lock", "8");
        wiz_wr_attr("head/z_current", "1");
    }
    if (L->local) {
        wiz_controller_stop();
        super_set_local(0);
        wiz_controller_start();
    } else if (L->stopped)
        wiz_controller_start();
}

/* The head's position in the sheet frame, from the counters and the
 * placement origin. */
static void sheet_pos(const live_t *L, double *x, double *y)
{
    *x = (L->cx - L->place.ox) / L->spmx;
    *y = (L->cy - L->place.oy) / L->spmy;
}

/* ------------------------------------------------ the program generator */

typedef struct {
    char head[6][80];
    int nhead, ihead;
    const char *body;
    size_t off;
    int tail;               /* the lines after the body: Z up, the datum, M2 */
    /* the settle directive */
    int settling;
    double settle_t0, last_dwell, now;
    /* the samples: 25 Hz when store25 (2: from the ";record" directive
     * on, so a card's text stays out of its ladder), 1 Hz engine
     * readings when store1 */
    int store25, store1;
    int lit;                    /* the tube has been seen lit: the phase said so */
    int lit_now, ended;         /* the tube's state now; the program streamed to its end */
    double t_lit0, t_lit_last, last_phase;
    double tail_s;              /* the dark tail the run waits for after the last discharge */
    const char *tail_what;      /* what the tail is for, in the phase */
    int press_open;             /* the press prompt is up: the button is lit */
    double t_led0;              /* when it lit */
    long *hv, *tp;
    int n25;
    double *t1, *down, *up;
    int n1, hold;
    int tick;
} gen_t;

static void gen_free(gen_t *g)
{
    free(g->hv);
    free(g->tp);
    free(g->t1);
    free(g->down);
    free(g->up);
    memset(g, 0, sizeof(*g));
}

/* The frame every program starts with: units, absolute, the origin
 * from the placement, the lens at the working Z. */
static int gen_init(gen_t *g, const live_t *L, double z, const char *body, int store25, int store1)
{
    memset(g, 0, sizeof(*g));
    double x, y;
    sheet_pos(L, &x, &y);
    snprintf(g->head[g->nhead++], sizeof(g->head[0]), "$X");
    snprintf(g->head[g->nhead++], sizeof(g->head[0]), "G21");
    snprintf(g->head[g->nhead++], sizeof(g->head[0]), "G90");
    /* The lens is referenced on the hall edge at the controller's start,
     * so the Z envelope is already open: set the sheet's origin. */
    snprintf(g->head[g->nhead++], sizeof(g->head[0]), "G92 X%.3f Y%.3f Z%.2f", x, y, edge_z());
    snprintf(g->head[g->nhead++], sizeof(g->head[0]), "G0 Z%.2f", z);
    g->body = body;
    g->store25 = store25;
    g->store1 = store1;
    if (store25) {
        g->hv = calloc(S25_MAX, sizeof(long));
        g->tp = calloc(S25_MAX, sizeof(long));
        if (!g->hv || !g->tp)
            return -1;
    }
    if (store1) {
        g->t1 = calloc(S1_MAX, sizeof(double));
        g->down = calloc(S1_MAX, sizeof(double));
        g->up = calloc(S1_MAX, sizeof(double));
        if (!g->t1 || !g->down || !g->up)
            return -1;
    }
    return 0;
}

/* The loop is quiet: past the minimum, the engine not holding, and the
 * downstream reading's slope over the last 30 s under the line; or the
 * cap reached. */
static int settled(const gen_t *g)
{
    double since = g->now - g->settle_t0;
    if (since >= SETTLE_MAX_S)
        return 1;
    if (since < SETTLE_MIN_S || g->hold || g->n1 < 30)
        return 0;
    int n = 30, a = g->n1 - n;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = a; i < g->n1; i++) {
        sx += g->t1[i];
        sy += g->down[i];
        sxx += g->t1[i] * g->t1[i];
        sxy += g->t1[i] * g->down[i];
    }
    double den = n * sxx - sx * sx;
    if (den <= 0)
        return 0;
    double slope = (n * sxy - sx * sy) / den;
    return fabs(slope) < SETTLE_SLOPE;
}

static int gen_next(void *ctx, char *buf, size_t len)
{
    gen_t *g = ctx;
    if (g->ihead < g->nhead) {
        snprintf(buf, len, "%s", g->head[g->ihead++]);
        return 1;
    }
    if (g->settling) {
        if (settled(g)) {
            g->settling = 0;
            wiz_log("the loop is quiet after %.0f s", g->now - g->settle_t0);
        } else {
            if (g->now - g->last_dwell < 5.0)
                return 2;
            g->last_dwell = g->now;
            snprintf(buf, len, "G4 P5");
            return 1;
        }
    }
    for (;;) {
        const char *p = g->body ? g->body + g->off : "";
        while (*p == '\n' || *p == '\r')
            p++;
        if (!*p) {
            /* The tail: the lens back at its reference, the head back at
             * the datum, then the program end, which the controller
             * acknowledges once the stream is produced. */
            static const char *const tail[] = { "G0 Z" "10.6", "G0 X0 Y0", "M2" };
            if (g->tail >= 3) {
                g->ended = 1;
                return 0;
            }
            snprintf(buf, len, "%s", tail[g->tail++]);
            return 1;
        }
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        g->off = (size_t)(p - g->body) + n;
        if (n >= len)
            n = len - 1;
        memcpy(buf, p, n);
        buf[n] = '\0';
        if (buf[0] != ';')
            return 1;
        /* The keys change mid-job, after the card's text burned under
         * the machine's own: the override saves the originals (restored
         * at the card's end, or by the daemon's next start) and M102
         * reloads them in the controller, in order. */
        if (!strcmp(buf, ";keys=floor-off")) {
            if (curverec_override_begin("0", "off", NULL) != 0) {
                snprintf(buf, len, "the laser keys are held by another job");
                return -1;
            }
            wiz_log("the floor and the dose curve are off from here");
            snprintf(buf, len, "M102");
            return 1;
        }
        if (!strncmp(buf, ";gamma=", 7)) {
            int rc = curverec_override_active() ? settings_set("laser_corner_gamma", buf + 7)
                                                : curverec_override_begin(NULL, NULL, buf + 7);
            if (rc != 0) {
                snprintf(buf, len, "cannot write the corner gamma");
                return -1;
            }
            wiz_log("corner gamma %s: the next pattern", buf + 7);
            snprintf(buf, len, "M102");
            return 1;
        }
        if (!strcmp(buf, ";record")) {
            g->n25 = 0;
            if (g->store25)
                g->store25 = 1;
            continue;
        }
        if (!strcmp(buf, ";settle")) {
            g->settling = 1;
            g->settle_t0 = g->now;
            g->last_dwell = g->now - 5.0;
            wiz_phase("letting the coolant loop settle");
            return 2;
        }
        /* an unknown directive: skipped */
    }
}

static void gen_sample(void *ctx, const jobstream_sample_t *s)
{
    gen_t *g = ctx;
    g->now = s->t;
    /* The phase follows the tube: waiting, burning (with the lit
     * seconds), then the dark tail counted down. */
    int lit_now = s->hv > JOBSTREAM_HV_ON;
    if (lit_now) {
        if (!g->lit_now)
            g->t_lit0 = s->t;
        g->t_lit_last = s->t;
        g->lit = 1;
    }
    g->lit_now = lit_now;
    /* The press: the controller lights the button white for its arm
     * wait and puts it out at the press, so a lit button before the
     * first discharge is the press this job waits for. The page shows
     * it as a prompt with the seconds; the discharge takes it down. */
    if (g->lit && g->press_open) {
        wiz_wait_close();
        g->press_open = 0;
    }
    if (s->t - g->last_phase >= 1.0) {
        g->last_phase = s->t;
        if (!g->lit && !g->press_open && led_lit() == 1) {
            g->press_open = 1;
            g->t_led0 = s->t;
            wiz_wait_open("press", "The button is lit white. Press it now: the laser fires after "
                                   "your press.", (int)WAIT_TIMEOUT_S);
        }
        if (lit_now)
            wiz_phase("burning: %.0f s lit", s->t - g->t_lit0);
        else if (g->lit && g->ended && g->tail_s > 0)
            wiz_phase("the tube is dark: %s, %.0f s more",
                      g->tail_what ? g->tail_what : "the samples run on",
                      g->tail_s - (s->t - g->t_lit_last) < 0 ? 0 : g->tail_s - (s->t - g->t_lit_last));
        else if (g->lit && g->ended)
            wiz_phase("the tube is dark: the pulse engine plays the rest");
        else if (g->lit)
            wiz_phase("burning: the tube is between passes");
        else if (g->press_open)
            wiz_phase("the button is lit: press it now (%.0f s; the job waits %.0f at most)",
                      s->t - g->t_led0, WAIT_TIMEOUT_S - g->t_led0);
        else
            wiz_phase("the program goes to the controller, the button lights at the arm: %.0f s",
                      s->t);
    }
    if (g->store25 == 1 && g->n25 < S25_MAX) {
        g->hv[g->n25] = s->hv;
        g->tp[g->n25] = s->tp;
        g->n25++;
    }
    if (g->store1 && (g->tick++ % 25) == 0 && g->n1 < S1_MAX) {
        char st[COOL_STATUS_JSON_MAX];
        if (cool_status_json(st, sizeof(st)) == 0) {
            const char *d = strstr(st, "\"down_c\":"), *u = strstr(st, "\"up_c\":");
            g->hold = strstr(st, "\"hold\":true") != NULL;
            if (d && u) {
                g->t1[g->n1] = s->t;
                g->down[g->n1] = atof(d + 9);
                g->up[g->n1] = atof(u + 7);
                g->n1++;
            }
        }
    }
}

/* Stream the program; the run's outcome in `run`. */
static int stream(gen_t *g, jobstream_run_t *run, double end_dark_s, char *err, size_t elen)
{
    if (jobstream_sender_connected() == 1) {
        snprintf(err, elen, "a sender is connected to the machine");
        return -1;
    }
    jobstream_cfg_t cfg = {
        .gen = gen_next, .sample = gen_sample, .ctx = g,
        .abort_flag = wiz_abort_flag(), .wait_timeout_s = WAIT_TIMEOUT_S,
        .run_timeout_s = RUN_TIMEOUT_S, .end_dark_s = end_dark_s,
    };
    return jobstream_run(&cfg, run, err, elen);
}

/* The emission witnesses into the result: all three must have seen
 * the beam for the burn to count. */
static int witnessed(const jobstream_run_t *run, json_t *r)
{
    long tp_delta = run->tp_base >= 0 ? run->tp_max - run->tp_base : 0;
    json_t *e = json_object();
    json_object_set_new(e, "hv_max", json_integer(run->hv_max));
    json_object_set_new(e, "laser_on_samples", json_integer(run->lon_max));
    json_object_set_new(e, "thermopile_delta", json_integer(tp_delta));
    json_object_set_new(e, "lit_s", json_real(run->lit ? run->t_last_lit - run->t_first_lit : 0));
    json_object_set_new(r, "emission", e);
    return run->lit && run->lon_max > 0 && tp_delta >= TP_DELTA_MIN;
}

static void not_witnessed(const jobstream_run_t *run)
{
    long tp_delta = run->tp_base >= 0 ? run->tp_max - run->tp_base : 0;
    if (!run->lit)
        wiz_finish_err("no discharge: the tube current never rose");
    else if (run->lon_max <= 0)
        wiz_finish_err("the kernel never sampled LASER_ON while the tube current rose");
    else
        wiz_finish_err("the head thermopile did not see the beam (rose %ld, needs %d)",
                       tp_delta, TP_DELTA_MIN);
}

/* -------------------------------------------------------- the builds */

/* The nominal parameters of a card's build, from the record. The
 * wizards fill the same struct with what they measured. */
typedef struct {
    int mark_s, mark_feed, text_s;
    double z_text;
    double focus_z[SHEET_FOCUS_N];
    int focus_k[SHEET_FOCUS_N];     /* the lines' half-steps from the hall edge */
    double floor_d[SHEET_FLOOR_N];
    double gamma[SHEET_CORNER_N];
    sheet_header_t header;
    header_bufs_t hb;
} build_t;

static void build_defaults(build_t *b)
{
    memset(b, 0, sizeof(*b));
    b->mark_s = mark_s();
    b->mark_feed = mark_feed();
    b->text_s = b->mark_s;
    b->z_text = edge_z();
    /* The ladder: twelve lines spread over the window, the top line at
     * its top and the bottom line at its bottom, on whole half-steps. */
    int down, up;
    lens_window(&down, &up);
    for (int i = 0; i < SHEET_FOCUS_N; i++) {
        b->focus_k[i] = (int)lround(up - (double)(up + down) * i / (SHEET_FOCUS_N - 1));
        b->focus_z[i] = z_of_k(b->focus_k[i]);
    }
    for (int i = 0; i < SHEET_FLOOR_N; i++)
        b->floor_d[i] = FLOOR_STEP * (i + 1);
    static const double g[SHEET_CORNER_N] = { 1.0, 1.25, 1.5, 1.75, 2.0 };
    memcpy(b->gamma, g, sizeof(g));
    header_facts(&b->header, &b->hb);
}

/* The preview and the program of a wizard with the build's parameters
 * (the record's, or the running wizard's). alone offsets a card. */
static int build_card(const char *id, build_t *b, int alone, sheet_paths_t *pv, sheet_text_t *pg)
{
    const sheet_card_t *c = sheet_card(id);
    if (alone && c) {
        float dx, dy;
        sheet_alone_offset(c, &dx, &dy);
        sheet_offset(pv, dx, dy);
    }
    if (!strcmp(id, "sheet.frame") || !strcmp(id, "sheet.place")) {
        sheet_build_frame(pv, pg, &b->header, b->text_s, b->mark_feed);
        return 0;
    }
    if (!c)
        return -1;
    if (!strcmp(id, "laser.focus"))
        sheet_build_focus(pv, pg, c, b->focus_z, SHEET_FOCUS_N, b->z_text, b->text_s, b->mark_feed);
    else if (!strcmp(id, "laser.floor"))
        sheet_build_floor(pv, pg, c, b->floor_d, SHEET_FLOOR_N, b->text_s, b->mark_feed);
    else if (!strcmp(id, "laser.dose-curve"))
        sheet_build_dose(pv, pg, c, b->text_s, b->mark_feed);
    else if (!strcmp(id, "laser.corner"))
        sheet_build_corner(pv, pg, c, b->gamma, SHEET_CORNER_N, CORNER_S, CORNER_FEED, b->text_s,
                           b->mark_feed);
    else if (!strcmp(id, "cooling.flow-load"))
        sheet_build_flowload(pv, pg, c, PATCH_S, PATCH_FEED, b->text_s, b->mark_feed);
    else
        return -1;
    return 0;
}

int wizlive_preview(const char *id, sheet_text_t *out)
{
    if (!wizlive_known(id))
        return -1;
    build_t *b = calloc(1, sizeof(*b));
    if (!b)
        return -1;
    build_defaults(b);
    place_t p;
    place_read(&p);
    const sheet_card_t *c = sheet_card(id);
    int alone = p.alone && c != NULL;
    sheet_paths_t pv;
    sheet_text_t pg;
    sheet_paths_init(&pv);
    sheet_text_init(&pg);
    float w = SHEET_W, h = SHEET_H;
    if (alone)
        sheet_alone_size(c, &w, &h);
    else if (c)
        sheet_rect(&pv, SHEET_FRAME_X, SHEET_FRAME_Y, SHEET_FRAME_W, SHEET_FRAME_H);
    int rc = build_card(id, b, alone, &pv, &pg);
    /* The placement shows the whole sheet: the frame and the header,
     * then every card as it will burn, from the record's facts. */
    if (rc == 0 && !strcmp(id, "sheet.place"))
        for (size_t i = 0; rc == 0 && i < sheet_ncards(); i++)
            rc = build_card(sheet_card_at(i)->id, b, 0, &pv, &pg);
    if (rc == 0)
        rc = sheet_svg(&pv, w, h, out);
    sheet_paths_free(&pv);
    sheet_text_free(&pg);
    free(b);
    return rc;
}

int wizlive_program(const char *id, sheet_text_t *out)
{
    if (!wizlive_known(id))
        return -1;
    build_t *b = calloc(1, sizeof(*b));
    if (!b)
        return -1;
    build_defaults(b);
    place_t p;
    place_read(&p);
    sheet_paths_t pv;
    sheet_text_t pg;
    sheet_paths_init(&pv);
    sheet_text_init(&pg);
    sheet_text_line(out, "; ForgeFIRM commissioning: %s, the body the wizard streams after", id);
    sheet_text_line(out, "; $X G21 G90 and G92 (the sheet origin) and G0 Z%.2f; M2 closes it.", focus_z(&p));
    int rc = build_card(id, b, p.alone && sheet_card(id) != NULL, &pv, &pg);
    if (rc == 0 && pg.buf)
        sheet_text_append(out, pg.buf, pg.len);
    sheet_paths_free(&pv);
    sheet_text_free(&pg);
    free(b);
    return rc;
}

/* ------------------------------------------------------- the wizards */

static void run_place(void)
{
    json_t *r = json_object();
    live_t L = { 0 };
    int fd = -1;
    if (live_begin(&L, NULL, 0, 2, r) != 0)
        goto out;
    json_object_set_new(r, "z_referenced", json_true());
    wiz_progress(30);
    fd = wiz_grbl_connect();
    if (fd < 0) {
        wiz_finish_err("cannot reach the controller on the Grbl socket");
        goto out;
    }
    char line[160], a[24];
    wiz_grbl_expect(fd, "Grbl", line, sizeof(line), 5);
    wiz_grbl_send(fd, "$X");
    wiz_grbl_expect(fd, "ok", line, sizeof(line), 5);
    static const char *const jogs[] = { "X-10", "X+10", "Y-10", "Y+10", "X-1", "X+1", "Y-1", "Y+1",
                                        "Set origin" };
    wiz_phase("placing the sheet");
    for (;;) {
        int rc = wiz_ask("jog", "place",
                         "The head should be at the back-left, where the machine normally homes; "
                         "the arrows move it (lid closed). Then open the lid, push the sheet as far "
                         "left as it goes with its top edge on the top of the cut area, close the "
                         "lid, and set the origin.", jogs, 9, a, sizeof(a));
        if (rc != 0)
            goto out;
        if (!strcmp(a, "Set origin"))
            break;
        double d = atof(a + 1);
        if ((a[0] != 'X' && a[0] != 'Y') || fabs(d) > 10.0 || d == 0)
            continue;
        if (!machine_lid_closed()) {
            wiz_log("the lid is open: close it to jog");
            continue;
        }
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "$J=G91 G21 %c%g F3000", a[0], d);
        if (wiz_grbl_send(fd, cmd) != 0 || wiz_grbl_expect(fd, "ok", line, sizeof(line), 5) != 0) {
            wiz_log("the controller refused the jog: %s", line);
            continue;
        }
        wiz_grbl_wait_idle(fd, 15);
        wiz_kernel_wait_idle(3.0);
    }
    wiz_progress(70);
    static const char *const kinds[] = { "Full sheet", "One card" };
    char size[32], kind_q[200];
    snprintf(kind_q, sizeof(kind_q), "Is this the full commissioning sheet (%s or more), or a "
             "smaller piece for one card?", sheet_size_text(size, sizeof(size)));
    if (wiz_ask("choice", "sheet-kind", kind_q, kinds, 2, a, sizeof(a)) != 0)
        goto out;
    int alone = !strcmp(a, "One card");
    static const char *const th[] = { "Set", "Skip" };
    double thickness = 0;
    char text[120];
    snprintf(text, sizeof(text), "The thickness of the sheet in %s.",
             wiz_imperial() ? "inches" : "millimeters");
    int rc = wiz_ask("number", "thickness", text, th, 2, a, sizeof(a));
    if (rc < 0)
        goto out;
    if (rc == 0 && strcasecmp(a, "skip"))
        thickness = wiz_len_mm(a);
    if (thickness < 0 || thickness > 30)
        thickness = 0;
    long x, y, z;
    if (read_counters(&x, &y, &z) != 0) {
        wiz_finish_err("cannot read the position counters");
        goto out;
    }
    json_object_set_new(r, "origin_x", json_integer(x));
    json_object_set_new(r, "origin_y", json_integer(y));
    json_object_set_new(r, "origin_z", json_integer(z));
    json_object_set_new(r, "alone", json_boolean(alone));
    json_object_set_new(r, "thickness_mm", json_real(thickness));
    json_object_set_new(r, "steps_per_mm", json_pack("{s:f,s:f,s:f}", "x", L.spmx, "y", L.spmy,
                                                     "z", L.spmz));
    char u1[32], sum[240];
    wiz_log("origin at counters %ld, %ld (%s, %s)", x, y, alone ? "one card" : "full sheet",
            wiz_len(thickness, u1, sizeof(u1)));
    if (thickness > 0)
        snprintf(u1, sizeof(u1), "%s thick", wiz_len(thickness, sum, sizeof(sum)));
    else
        snprintf(u1, sizeof(u1), "thickness not entered");
    snprintf(sum, sizeof(sum), "The origin is set at the head's position. %s, %s. Every card is "
             "drawn from this point: do not move the sheet until the last card is done.",
             alone ? "One card on its own piece" : "The full sheet", u1);
    json_object_set_new(r, "summary", json_string(sum));
    wiz_progress(100);
    wiz_finish_ok(1, r, NULL);
out:
    if (fd >= 0)
        close(fd);
    live_end(&L);
    json_decref(r);
}

/* One armed burn of a build: the program streamed, the witnesses read
 * into r. 0 witnessed, -1 (the error is set). */
static int burn(live_t *L, build_t *b, const char *id, int alone, double z, int store25, int store1,
                double end_dark_s, const char *tail_what, gen_t *g, jobstream_run_t *run, json_t *r)
{
    sheet_paths_t pv;
    sheet_text_t pg;
    sheet_paths_init(&pv);
    sheet_text_init(&pg);
    int rc = build_card(id, b, alone, &pv, &pg);
    sheet_paths_free(&pv);
    if (rc != 0 || pg.oom || !pg.buf) {
        sheet_text_free(&pg);
        wiz_finish_err("the program could not be built");
        return -1;
    }
    if (gen_init(g, L, z, pg.buf, store25, store1) != 0) {
        sheet_text_free(&pg);
        wiz_finish_err("out of memory");
        return -1;
    }
    g->tail_what = tail_what;
    wiz_phase("the program goes to the controller, the button lights at the arm");
    char err[160];
    /* The controller acknowledges the program end when the stream is
     * produced, one queue depth before the pulse engine has played it:
     * the run follows the tube until it has been dark for a while, and
     * then the kernel is waited for before anything else touches the
     * controller. */
    g->tail_s = end_dark_s < 2.0 ? 2.0 : end_dark_s;
    rc = stream(g, run, g->tail_s, err, sizeof(err));
    sheet_text_free(&pg);
    g->body = NULL;
    if (g->press_open) {
        wiz_wait_close();
        g->press_open = 0;
    }
    if (rc != 0) {
        wiz_finish_err("%s", err);
        return -1;
    }
    wiz_phase("the tube is dark: the pulse engine plays the rest");
    if (wiz_kernel_wait_idle(600.0) != 0) {
        wiz_finish_err("the pulse engine did not finish the program");
        return -1;
    }
    wiz_log("streamed %d lines; tube current peak %ld, LASER_ON samples %ld, thermopile +%ld",
            run->sent, run->hv_max, run->lon_max,
            run->tp_base >= 0 ? run->tp_max - run->tp_base : 0);
    if (!witnessed(run, r)) {
        not_witnessed(run);
        return -1;
    }
    return 0;
}

static void run_frame(void)
{
    json_t *r = json_object();
    live_t L = { 0 };
    build_t *b = calloc(1, sizeof(*b));
    gen_t g = { 0 };
    jobstream_run_t run;
    if (!b) {
        wiz_finish_err("out of memory");
        goto out;
    }
    if (live_begin(&L, NULL, 1, 2, r) != 0)
        goto out;
    build_defaults(b);
    wiz_progress(20);
    char size[32], arm_q[240];
    snprintf(arm_q, sizeof(arm_q), "The frame and the header burn now: %s at the mark dose. "
             "Eye protection on, exhaust on, lid closed. Continue, then press the machine's "
             "button when it lights white; this page says when.", frame_size_text(size, sizeof(size)));
    for (int attempt = 0; attempt < 4; attempt++) {
        if (wiz_ask_continue("frame-arm", arm_q) != 0)
            goto out;
        b->text_s = b->mark_s;
        if (burn(&L, b, "sheet.frame", 0, focus_z(&L.place), 0, 0, 0, NULL, &g, &run, r) != 0)
            goto out;
        gen_free(&g);
        wiz_progress(60 + attempt * 10);
        static const char *const opts[] = { "Yes", "Again, lighter", "Again, darker" };
        char a[24];
        if (wiz_ask("choice", "frame-ok",
                    "Is the frame on the sheet, square, and visible? " STILL, opts, 3,
                    a, sizeof(a)) != 0)
            goto out;
        if (!strcmp(a, "Yes"))
            break;
        int s = strstr(a, "lighter") ? (int)(b->mark_s * 0.9) : (int)(b->mark_s * 1.1);
        b->mark_s = s < 50 ? 50 : s > 1000 ? 1000 : s;
        wiz_log("mark dose S%d for the next try", b->mark_s);
        if (attempt == 3) {
            wiz_finish_err("the frame was not accepted after four burns");
            goto out;
        }
    }
    json_object_set_new(r, "mark_s", json_integer(b->mark_s));
    json_object_set_new(r, "mark_feed", json_integer(b->mark_feed));
    json_object_set_new(r, "started", json_string(b->hb.started));
    char sum[240];
    snprintf(sum, sizeof(sum), "The frame and the header are on the sheet. The mark dose is S%d at "
             "%d mm/min; every card's box and labels burn at it. All three witnesses saw the "
             "beam: the tube current, the kernel's LASER_ON samples, and the head thermopile.",
             b->mark_s, b->mark_feed);
    json_object_set_new(r, "summary", json_string(sum));
    wiz_progress(100);
    wiz_finish_ok(1, r, NULL);
out:
    gen_free(&g);
    free(b);
    live_end(&L);
    json_decref(r);
}

static void run_focus(void)
{
    json_t *r = json_object(), *applied = json_object();
    live_t L = { 0 };
    build_t *b = calloc(1, sizeof(*b));
    gen_t g = { 0 };
    jobstream_run_t run;
    if (!b) {
        wiz_finish_err("out of memory");
        goto out;
    }
    L.find_stops = 1;
    if (live_begin(&L, sheet_card("laser.focus"), 1, 2, r) != 0)
        goto out;
    build_defaults(b);
    /* The ladder: twelve lines over the window the finder just measured
     * (else the fallback), build_defaults placed them. Nothing here can
     * reach a stop. */
    int down, up;
    lens_window(&down, &up);
    char text[640], u1[32], u2[32], u3[32], u4[32], stops[320];
    if (L.stops.found)
        snprintf(stops, sizeof(stops), "Its stops were found by the head accelerometer, %d half-steps "
                 "below the reference and %d above, without a slip.", down, up);
    else
        snprintf(stops, sizeof(stops), "Its stops could not be found by the head accelerometer (%s), "
                 "so the ladder keeps a reduced travel.", L.stops.why);
    snprintf(text, sizeof(text),
             "The lens is on its hall reference. %s Twelve %s lines burn heavy, the lens stepped "
             "from %s above the reference to %s below it. "
             "Lid closed. Continue, then press the button when it lights white; this page "
             "says when.", stops,
             wiz_len(sheet_ladder_len(sheet_card("laser.focus")), u1, sizeof(u1)),
             wiz_len(up / z_spm, u2, sizeof(u2)),
             wiz_len(down / z_spm, u3, sizeof(u3)));
    if (wiz_ask_continue("focus-arm", text) != 0)
        goto out;
    if (burn(&L, b, "laser.focus", L.place.alone, edge_z(), 0, 0, 0,
             NULL, &g, &run, r) != 0)
        goto out;
    wiz_progress(70);
    const char *opts[SHEET_FOCUS_N];
    char names[SHEET_FOCUS_N][8], a[24];
    for (int i = 0; i < SHEET_FOCUS_N; i++) {
        snprintf(names[i], sizeof(names[i]), "%d", i + 1);
        opts[i] = names[i];
    }
    char picks[40];
    if (wiz_ask_t("multichoice", "focus-pick",
                  "Every line is dark. Which is the narrowest dark line, with the crispest edges? "
                  "Count from the top, 1 to 12. Several adjacent lines can look the same, since "
                  "the depth of focus spans a few of them: choose every one that does, and the "
                  "middle of the run is taken. Then Done. " STILL,
                  opts, SHEET_FOCUS_N, PICK_TIMEOUT_S, picks, sizeof(picks)) != 0)
        goto out;
    /* The picks: one line, or a run of adjacent lines, its middle the
     * pick (the depth of focus spans a few lines; the middle of the run
     * that looks alike is the focus). */
    double pick = 0;
    int npick = 0, lo = 0, hi = 0;
    for (char *tok = strtok(picks, ", "); tok; tok = strtok(NULL, ", ")) {
        int v = atoi(tok);
        if (v < 1 || v > SHEET_FOCUS_N) {
            wiz_finish_err("the pick must be line numbers from 1 to %d", SHEET_FOCUS_N);
            goto out;
        }
        lo = npick == 0 || v < lo ? v : lo;
        hi = npick == 0 || v > hi ? v : hi;
        npick++;
    }
    if (npick == 0 || hi - lo + 1 != npick) {
        wiz_finish_err("choose one line, or a run of adjacent lines that look alike");
        goto out;
    }
    pick = (lo + hi) / 2.0;
    json_object_set_new(r, "pick", json_real(pick));
    json_object_set_new(r, "picked_lines", json_pack("[i,i]", lo, hi));
    if (lo == 1 || hi == SHEET_FOCUS_N) {
        wiz_finish_err("the sharpest line is at the end of the ladder, so the focus lies outside "
                       "it: use a sheet nearer 1/8 in (3 mm) thick and run the card again");
        goto out;
    }
    static const char *const th[] = { "Set", "Keep" };
    snprintf(text, sizeof(text), "The sheet thickness (%s from the placement).",
             wiz_len(L.place.thickness, u1, sizeof(u1)));
    double thickness = L.place.thickness;
    int rc = wiz_ask("number", "thickness", text, th, 2, a, sizeof(a));
    if (rc < 0)
        goto out;
    if (rc == 0 && strcasecmp(a, "keep") && wiz_len_mm(a) > 0 && wiz_len_mm(a) <= 30)
        thickness = wiz_len_mm(a);
    /* The lens moves with its focal point, so the picked line's half-steps
     * from the hall edge, over the screw's scale, is how far the focus at
     * the edge sits below the material top: the one number this head
     * needs. The window around the edge is the focal range. */
    double k = (b->focus_k[lo - 1] + b->focus_k[hi - 1]) / 2.0;
    double edge = thickness - k / z_spm;
    double z_lo = edge - down / z_spm, z_hi = edge + up / z_spm;
    char v[16], vd[8], vu[8];
    snprintf(v, sizeof(v), "%.2f", edge);
    snprintf(vd, sizeof(vd), "%d", down);
    snprintf(vu, sizeof(vu), "%d", up);
    const char *keys[] = { "lens_hall_edge_z_mm", "lens_stop_below_steps", "lens_stop_above_steps" };
    const char *vals[] = { v, vd, vu };
    char err[96];
    if (wiz_write_settings(keys, vals, 3, applied, err, sizeof(err)) != 0) {
        wiz_finish_err("%s", err);
        goto out;
    }
    json_object_set_new(r, "thickness_mm", json_real(thickness));
    json_object_set_new(r, "pick_half_steps", json_real(k));
    json_object_set_new(r, "edge_z_mm", json_real(edge));
    json_object_set_new(r, "steps_per_mm", json_real(z_spm));
    json_object_set_new(r, "max_height_mm", json_real(z_hi));
    json_object_set_new(r, "focus_range_mm", json_pack("{s:f,s:f}", "min", z_lo, "max", z_hi));
    wiz_log("line %g on %s: %+g half-steps from the hall edge, so the focus at the edge is %s "
            "above the tray; the lens reaches %s to %s", pick,
            wiz_len(thickness, u1, sizeof(u1)), k, wiz_len(edge, u2, sizeof(u2)),
            wiz_len(z_lo, u3, sizeof(u3)), wiz_len(z_hi, u4, sizeof(u4)));
    char sum[960];
    if (L.stops.found)
        snprintf(sum, sizeof(sum), "Line %g on %s: with the lens on its hall reference the focus is "
                 "%s above the tray. A home puts the lens there, sets Z to that height, and parks "
                 "the focus at the park height; Z counts the focus height above the bed. The stops "
                 "were found %d half-steps below the reference and %d above, so the lens reaches "
                 "%s to %s. Every later card runs at the focus for this sheet.",
                 pick, u1, u2, down, up, u3, u4);
    else
        snprintf(sum, sizeof(sum), "Line %g on %s: with the lens on its hall reference the focus is "
                 "%s above the tray. A home puts the lens there, sets Z to that height, and parks "
                 "the focus at the park height; Z counts the focus height above the bed. The stops "
                 "could not be found by the head accelerometer on this machine (%s), so the lens "
                 "keeps a reduced travel, %s to %s. Please open an issue at "
                 "github.com/openglow-org/forgefirm or post on community.openglow.org so this head "
                 "can be looked at. Every later card runs at the focus for this sheet.",
                 pick, u1, u2, L.stops.why, u3, u4);
    json_object_set_new(r, "summary", json_string(sum));
    wiz_progress(100);
    wiz_finish_ok(1, r, applied);
out:
    gen_free(&g);
    free(b);
    live_end(&L);
    json_decref(r);
    json_decref(applied);
}

static void run_floor(void)
{
    json_t *r = json_object(), *applied = json_object();
    live_t L = { 0 };
    build_t *b = calloc(1, sizeof(*b));
    gen_t g = { 0 };
    jobstream_run_t run;
    if (!b) {
        wiz_finish_err("out of memory");
        goto out;
    }
    if (live_begin(&L, sheet_card("laser.floor"), 1, 2, r) != 0)
        goto out;
    build_defaults(b);
    char text[320], u1[32];
    snprintf(text, sizeof(text),
             "Twelve %s rungs from 2 to 24 percent density, the floor and the dose curve off for "
             "the rungs. Lid closed. Continue, then press the button when it lights white; this "
             "page says when.",
             wiz_len(sheet_ladder_len(sheet_card("laser.floor")), u1, sizeof(u1)));
    if (wiz_ask_continue("floor-arm", text) != 0)
        goto out;
    if (burn(&L, b, "laser.floor", L.place.alone, focus_z(&L.place), 0, 0, 0, NULL, &g, &run, r) != 0)
        goto out;
    curverec_override_end();
    wiz_progress(70);
    const char *opts[SHEET_FLOOR_N + 1];
    char names[SHEET_FLOOR_N][8], a[24];
    for (int i = 0; i < SHEET_FLOOR_N; i++) {
        snprintf(names[i], sizeof(names[i]), "%.0f", b->floor_d[i]);
        opts[i] = names[i];
    }
    opts[SHEET_FLOOR_N] = "None";
    if (wiz_ask_t("choice", "floor-pick",
                  "Which is the faintest rung that shows a continuous line? The labels are the "
                  "percent density; the top rung is 2. " STILL, opts, SHEET_FLOOR_N + 1,
                  PICK_TIMEOUT_S, a, sizeof(a)) != 0)
        goto out;
    if (!strcmp(a, "None")) {
        wiz_finish_err("no rung marked a continuous line up to 24 percent: the tube or the "
                       "supply needs attention before the floor can be set");
        goto out;
    }
    double faint = atof(a), floor = faint + FLOOR_STEP;
    char v[16];
    snprintf(v, sizeof(v), "%.0f", floor);
    const char *keys[] = { "laser_floor_density" };
    const char *vals[] = { v };
    char err[96];
    if (wiz_write_settings(keys, vals, 1, applied, err, sizeof(err)) != 0) {
        wiz_finish_err("%s", err);
        goto out;
    }
    json_object_set_new(r, "faintest_density", json_real(faint));
    json_object_set_new(r, "floor_density", json_real(floor));
    wiz_log("the faintest continuous rung is %.0f percent: the floor is %.0f", faint, floor);
    snprintf(text, sizeof(text), "The faintest rung with a continuous line is %.0f percent "
             "density. The laser floor is set at %.0f percent, two above it: no job asks the "
             "tube for less, so the lightest engraving still marks this wood.", faint, floor);
    json_object_set_new(r, "summary", json_string(text));
    wiz_progress(100);
    wiz_finish_ok(1, r, applied);
out:
    curverec_override_end();
    gen_free(&g);
    free(b);
    live_end(&L);
    json_decref(r);
    json_decref(applied);
}

static void run_dose(void)
{
    json_t *r = json_object(), *applied = json_object();
    live_t L = { 0 };
    build_t *b = calloc(1, sizeof(*b));
    gen_t g = { 0 };
    jobstream_run_t run;
    if (!b) {
        wiz_finish_err("out of memory");
        goto out;
    }
    if (live_begin(&L, sheet_card("laser.dose-curve"), 1, 2, r) != 0)
        goto out;
    build_defaults(b);
    if (wiz_ask_continue("dose-arm",
                         "Seven 100 mm rungs from 10 to 100 percent density, the thermopile "
                         "reading each. The curve fits itself. Lid closed. Continue, then press "
                         "the button when it lights white; this page says when.") != 0)
        goto out;
    if (burn(&L, b, "laser.dose-curve", L.place.alone, focus_z(&L.place), 2, 0, DOSE_DARK_S,
             "the fit needs the dark tail", &g, &run, r) != 0)
        goto out;
    curverec_override_end();
    wiz_progress(80);
    curverec_pt pts[8];
    char err[96];
    int n = curverec_fit(g.hv, g.tp, g.n25, JOBSTREAM_HZ, pts, 8, err, sizeof(err));
    if (n < 0) {
        wiz_finish_err("the ladder did not fit: %s", err);
        goto out;
    }
    char text[256];
    curverec_curve_text(pts, n, text, sizeof(text));
    json_t *arr = json_array();
    for (int i = 0; i < n; i++)
        json_array_append_new(arr, json_pack("{s:f,s:f}", "density", pts[i].density, "light",
                                             pts[i].light));
    json_object_set_new(r, "points", arr);
    json_object_set_new(r, "curve", json_string(text));
    char sum[320];
    snprintf(sum, sizeof(sum), "The curve rose rung by rung and is written. It maps the density "
             "a job asks for to the light the tube gives: %.0f percent density gives %.1f "
             "percent of the light, %.0f gives %.0f, and %.0f gives %.0f. The chart shows the fit.",
             pts[0].density, pts[0].light, pts[n / 2].density, pts[n / 2].light,
             pts[n - 1].density, pts[n - 1].light);
    json_object_set_new(r, "summary", json_string(sum));
    const char *keys[] = { "laser_dose_curve" };
    const char *vals[] = { text };
    if (wiz_write_settings(keys, vals, 1, applied, err, sizeof(err)) != 0) {
        wiz_finish_err("%s", err);
        goto out;
    }
    wiz_log("dose curve %s", text);
    wiz_progress(100);
    wiz_finish_ok(1, r, applied);
out:
    curverec_override_end();
    gen_free(&g);
    free(b);
    live_end(&L);
    json_decref(r);
    json_decref(applied);
}

static void run_corner(void)
{
    json_t *r = json_object(), *applied = json_object();
    live_t L = { 0 };
    build_t *b = calloc(1, sizeof(*b));
    gen_t g = { 0 };
    jobstream_run_t run;
    if (!b) {
        wiz_finish_err("out of memory");
        goto out;
    }
    if (live_begin(&L, sheet_card("laser.corner"), 1, 2, r) != 0)
        goto out;
    build_defaults(b);
    char curve[192] = "";
    settings_get("laser_dose_curve", curve, sizeof(curve));
    if (!strcmp(curve, "off"))
        wiz_log("the dose curve is off: the rolloff has no effect until it is on");
    /* The first pattern's directive saves the original gamma to the
     * marker; each pattern writes its own and the end restores it before
     * the winner is written. */
    if (wiz_ask_continue("corner-arm",
                         "The same corner-heavy pattern five times at rolloff exponents 1.0 to "
                         "2.0 under velocity-scaled power at 30 percent. Lid closed. Continue, "
                         "then press the button when it lights white; this page says when.") != 0)
        goto out;
    if (burn(&L, b, "laser.corner", L.place.alone, focus_z(&L.place), 0, 0, 0, NULL, &g, &run, r) != 0)
        goto out;
    curverec_override_end();
    wiz_progress(70);
    const char *opts[SHEET_CORNER_N];
    char names[SHEET_CORNER_N][8], a[24];
    for (int i = 0; i < SHEET_CORNER_N; i++) {
        snprintf(names[i], sizeof(names[i]), "%.2f", b->gamma[i]);
        opts[i] = names[i];
    }
    if (wiz_ask_t("choice", "corner-pick",
                  "Which pattern has the most even corners, neither burned dark nor faded, through "
                  "the teeth, the square, and the root sign at the end? The top pattern is 1.00. "
                  STILL, opts, SHEET_CORNER_N, PICK_TIMEOUT_S, a, sizeof(a)) != 0)
        goto out;
    const char *keys[] = { "laser_corner_gamma" };
    const char *vals[] = { a };
    char err[96];
    if (wiz_write_settings(keys, vals, 1, applied, err, sizeof(err)) != 0) {
        wiz_finish_err("%s", err);
        goto out;
    }
    json_object_set_new(r, "gamma", json_real(atof(a)));
    wiz_log("corner rolloff gamma %s", a);
    char sum[320];
    snprintf(sum, sizeof(sum), "Pattern %s has the most even corners: the corner rolloff is set "
             "to %s. Under velocity-scaled power the machine eases the power into every corner "
             "by that exponent, so corners burn like the straights.", a, a);
    json_object_set_new(r, "summary", json_string(sum));
    wiz_progress(100);
    wiz_finish_ok(1, r, applied);
out:
    curverec_override_end();
    gen_free(&g);
    free(b);
    live_end(&L);
    json_decref(r);
    json_decref(applied);
}

/* The mean of a 1 Hz series over [t0, t1); NAN when empty. */
static double tmean(const double *t, const double *v, int n, double t0, double t1)
{
    double s = 0;
    int k = 0;
    for (int i = 0; i < n; i++)
        if (t[i] >= t0 && t[i] < t1) {
            s += v[i];
            k++;
        }
    return k ? s / k : NAN;
}

/* The coolant loop quiet before the press: the engine's readings at
 * 1 Hz until the downstream slope over 30 s is under SETTLE_SLOPE (60 s
 * at least, SETTLE_MAX_S at most), the phase counting. 0, -1 aborted. */
static int settle_before(void)
{
    enum { SETTLE_N = (int)SETTLE_MAX_S + 1 };
    double t[SETTLE_N], d[SETTLE_N];
    int n = 0;
    for (int i = 0; i <= (int)SETTLE_MAX_S; i++) {
        char st[COOL_STATUS_JSON_MAX];
        double down = NAN;
        int hold = 0;
        if (cool_status_json(st, sizeof(st)) == 0) {
            const char *p = strstr(st, "\"down_c\":");
            if (p)
                down = atof(p + 9);
            hold = strstr(st, "\"hold\":true") != NULL;
        }
        if (!isnan(down) && n < SETTLE_N) {
            t[n] = i;
            d[n] = down;
            n++;
        }
        double slope = NAN;
        if (n >= 30) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int k = n - 30; k < n; k++) {
                sx += t[k];
                sy += d[k];
                sxx += t[k] * t[k];
                sxy += t[k] * d[k];
            }
            double den = 30 * sxx - sx * sx;
            if (den > 0)
                slope = (30 * sxy - sx * sy) / den;
        }
        if (i >= (int)SETTLE_MIN_S && !hold && !isnan(slope) && fabs(slope) < SETTLE_SLOPE) {
            wiz_log("the coolant loop is quiet after %d s (downstream %.2f C)", i, down);
            return 0;
        }
        if (isnan(slope))
            wiz_phase("the coolant loop settles before the press: %d s (%.0f at least, %.0f at "
                      "most), reading the sensors", i, SETTLE_MIN_S, SETTLE_MAX_S);
        else
            wiz_phase("the coolant loop settles before the press: %d s (%.0f at least, %.0f at "
                      "most), drifting %.3f C per s", i, SETTLE_MIN_S, SETTLE_MAX_S, slope);
        wiz_progress(20 + i * 20 / (int)SETTLE_MAX_S);
        for (int k = 0; k < 10; k++) {
            wiz_msleep(100);
            if (wiz_aborted())
                return -1;
        }
    }
    wiz_log("the coolant loop did not settle within %.0f s: going on", SETTLE_MAX_S);
    return 0;
}

static void run_flowload(void)
{
    json_t *r = json_object(), *applied = json_object();
    live_t L = { 0 };
    build_t *b = calloc(1, sizeof(*b));
    gen_t g = { 0 };
    jobstream_run_t run;
    int held = 0;
    if (!b) {
        wiz_finish_err("out of memory");
        goto out;
    }
    if (live_begin(&L, sheet_card("cooling.flow-load"), 1, 2, r) != 0)
        goto out;
    build_defaults(b);
    /* The engine's flow check heats the loop by ten degrees, a hundred
     * times the tube's share: the check is held for this card, the loop
     * is settled before the press, and the text and the patch burn in
     * one armed window. */
    if (settle_before() != 0)
        goto out;
    cool_flow_check_hold(1);
    held = 1;
    if (wiz_ask_continue("load-arm",
                         "The loop is quiet. Now the box, then a 60 x 15 mm patch at 60 percent "
                         "that keeps the tube lit for about a minute, both coolant sensors read "
                         "for 100 s after. About four minutes in all. Lid closed. Continue, then "
                         "press the button when it lights white; this page says when.") != 0)
        goto out;
    if (burn(&L, b, "cooling.flow-load", L.place.alone, focus_z(&L.place), 1, 1, LOAD_TAIL_S,
             "the coolant sensors follow the heat", &g, &run, r) != 0)
        goto out;
    cool_flow_check_hold(0);
    held = 0;
    wiz_progress(80);
    /* The tube's signature (the bench drill's method): the current's
     * integral over the lit span, the downstream baseline over the 20 s
     * before the fire, the peak of the 5 s bins after it, and k as the
     * peak over the dose. The patch is the last lit span of the run. */
    double t_fire0 = -1, t_fire1 = -1;
    for (int i = g.n25 - 1; i >= 0; i--) {
        double t = i / JOBSTREAM_HZ;
        if (g.hv[i] > JOBSTREAM_HV_ON) {
            if (t_fire1 < 0)
                t_fire1 = t;
            t_fire0 = t;
        } else if (t_fire1 >= 0 && t_fire1 - t > 5.0)
            break;
    }
    if (t_fire0 < 0 || g.n1 < 40) {
        wiz_finish_err("no lit span or too few coolant readings to judge");
        goto out;
    }
    double dose = 0, hv_sum = 0;
    int lit_n = 0;
    for (int i = (int)(t_fire0 * JOBSTREAM_HZ); i + 1 < g.n25 && i / JOBSTREAM_HZ <= t_fire1 + 1.0; i++) {
        dose += 0.5 * (g.hv[i] + g.hv[i + 1]) / JOBSTREAM_HZ;
        if (g.hv[i] > JOBSTREAM_HV_ON) {
            hv_sum += g.hv[i];
            lit_n++;
        }
    }
    double base = tmean(g.t1, g.down, g.n1, t_fire0 - 20.0, t_fire0 - 0.5);
    double up_base = tmean(g.t1, g.up, g.n1, t_fire0 - 20.0, t_fire0 - 0.5);
    if (isnan(base)) {
        wiz_finish_err("no coolant baseline before the fire");
        goto out;
    }
    double peak = -1e9, t_peak = 0, lag = -1;
    for (double t = t_fire0; t + 5.0 <= g.t1[g.n1 - 1]; t += 5.0) {
        double m = tmean(g.t1, g.down, g.n1, t, t + 5.0);
        if (isnan(m))
            continue;
        if (m > peak) {
            peak = m;
            t_peak = t;
        }
        if (lag < 0 && m > base + 0.3)
            lag = t - t_fire0;
    }
    double rise = peak - base;
    if (dose <= 0 || rise <= 0) {
        wiz_finish_err("the coolant did not warm under the patch (rise %.2f C over %.0f raw-s)", rise,
                       dose);
        goto out;
    }
    double k = rise / dose;
    if (k > 2e-4) {
        wiz_finish_err("the coefficient came out at %.3g C per raw-second, ten times the tube's "
                       "(the flow-check heater ran during the patch?)", k);
        goto out;
    }
    json_object_set_new(r, "lit_s", json_real(t_fire1 - t_fire0));
    json_object_set_new(r, "dose_raw_s", json_real(dose));
    json_object_set_new(r, "hv_mean", json_real(lit_n ? hv_sum / lit_n : 0));
    json_object_set_new(r, "base_c", json_real(base));
    json_object_set_new(r, "up_base_c", json_real(up_base));
    json_object_set_new(r, "peak_c", json_real(rise));
    json_object_set_new(r, "t_peak_s", json_real(t_peak - t_fire0));
    json_object_set_new(r, "lag_s", json_real(lag));
    json_object_set_new(r, "k_density", json_real(k));
    json_object_set_new(r, "k_cw", json_real(k / 0.77));
    wiz_log("lit %.0f s, dose %.0f raw-s, rise %.2f C at %.0f s: k %.3g C per raw-s",
            t_fire1 - t_fire0, dose, rise, t_peak - t_fire0, k);
    char sum[320];
    snprintf(sum, sizeof(sum), "The tube was lit for %.0f s and warmed the coolant by %.2f C, "
             "peaking %.0f s after the fire began. That is the tube's own share of a flow "
             "check's rise on this machine. It is written, and the flow check allows for it "
             "from now on.", t_fire1 - t_fire0, rise, t_peak - t_fire0);
    json_object_set_new(r, "summary", json_string(sum));
    char vd[24], vc[24], err[96];
    snprintf(vd, sizeof(vd), "%.4g", k);
    snprintf(vc, sizeof(vc), "%.4g", k / 0.77);
    const char *keys[] = { "cool_laser_heat_density", "cool_laser_heat_cw" };
    const char *vals[] = { vd, vc };
    if (wiz_write_settings(keys, vals, 2, applied, err, sizeof(err)) != 0) {
        wiz_finish_err("%s", err);
        goto out;
    }
    wiz_progress(100);
    wiz_finish_ok(1, r, applied);
out:
    if (held)
        cool_flow_check_hold(0);
    gen_free(&g);
    free(b);
    live_end(&L);
    json_decref(r);
    json_decref(applied);
}

/* --------------------------------------------------------- the table */

const wiz_entry_t wizlive_table[] = {
    { "sheet.place",       run_place,    1 },
    { "sheet.frame",       run_frame,    1 },
    { "laser.focus",       run_focus,    1 },
    { "laser.floor",       run_floor,    1 },
    { "laser.dose-curve",  run_dose,     1 },
    { "laser.corner",      run_corner,   1 },
    { "cooling.flow-load", run_flowload, 1 },
};
const size_t wizlive_n = sizeof(wizlive_table) / sizeof(*wizlive_table);

int wizlive_known(const char *id)
{
    for (size_t i = 0; id && i < wizlive_n; i++)
        if (!strcmp(wizlive_table[i].id, id))
            return 1;
    return 0;
}
