/*
 * curverec.c - the owner-run dose-curve recorder
 *
 * The tube's light output is convex in pulse density, so the controller
 * maps a commanded power through a measured curve (laser_dose_curve).
 * That curve is per tube and supply and drifts with tube age; this
 * module lets an owner measure their own from one Record press: the
 * recorder streams the ladder job itself over the local Grbl socket
 * through jobstream (absolute coordinates from X0 Y0, one 100 mm line
 * per rung, ok-per-line flow control), and the machine treats it as any
 * job: the arm gates stand and the operator's button press starts the
 * fire. Because a new Grbl connection displaces the sender (last
 * connection wins), the recorder refuses to start while the
 * controller's published state file shows a sender connected. While
 * the ladder plays the streamer samples the tube current
 * (pic/hv_current) and the head thermopile (head/beam_detect_analog, a
 * scatter detector in the beam path, so it reads the beam and not the
 * material) at 25 Hz; when the ladder has played, the fitter segments
 * the trace on the dark gaps, reads each rung's thermopile delta over
 * its local baseline, normalizes to the full-power rung, and offers the
 * result as a ready laser_dose_curve value the panel can apply.
 * GET /curve/ladder.gcode still serves the exact job the recorder
 * streams, for inspection.
 *
 * For the measurement to be the raw dose response, the floor and the
 * curve in force must not bend the ladder: the run overrides
 * laser_floor_density to 0 and laser_dose_curve to "off", and every end
 * path restores them. The override keeps the originals in a marker
 * file so a daemon that dies mid-run restores them at its next start;
 * the sheet wizards use the same override for their ladders and for
 * the corner rolloff exponent. The controller re-reads the keys at
 * each arm, so the override applies to the one job and to nothing
 * after it.
 *
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "curverec.h"
#include "fflog.h"
#include "jobstream.h"
#include "settings.h"
#include "wizdark.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SAMPLE_HZ      JOBSTREAM_HZ
#define MAX_SAMPLES    30000            /* 20 minutes at 25 Hz */
#define HV_ON          JOBSTREAM_HV_ON  /* discharge present above this */
#define GAP_MERGE_S    1.0             /* dark shorter than this stays in a rung */
#define MIN_SEG_S      2.0             /* a rung is at least this long lit */
#define DARK_END_S     20.0            /* recording ends after this much dark */
#define WAIT_TIMEOUT_S 600.0           /* budget for the arm + the press */
#define RUN_TIMEOUT_S  900.0
#define LADDER_MAX_LINES 40

/* The ladder's rungs: the S each maps to with the floor at 0 and the
 * curve off is the density itself, so the fitter knows each segment's
 * density by position. Matched to curverec_ladder_gcode(). */
static const double LADDER_D[] = { 10, 20, 30, 45, 60, 80, 100 };
#define LADDER_N ((int)(sizeof(LADDER_D) / sizeof(LADDER_D[0])))

enum { CR_IDLE, CR_WAITING, CR_RECORDING, CR_DONE, CR_FAILED };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int cr_state = CR_IDLE;
static char cr_reason[128];
static time_t cr_started;
static int cr_samples;
static long *buf_hv, *buf_tp;
static pthread_t thread;
static int thread_live;
static volatile int stop_requested;

/* ------------------------------------------------ the laser override */

/* The marker that carries the saved keys across a daemon exit mid-job:
 * three lines, floor, curve, gamma; an empty line for a value that was
 * unset, a line of one '-' for a key the override did not touch. */
#define SAVED_MARKER "/data/forgefirm/curverec.saved"
#define UNTOUCHED "-"

static pthread_mutex_t ov_mu = PTHREAD_MUTEX_INITIALIZER;
static int ov_active;
static char saved_floor[32], saved_curve[192], saved_gamma[32];

/* One line of the marker; a missing line reads as untouched. */
static void saved_line(FILE *f, char *buf, size_t len)
{
    buf[0] = '\0';
    if (fgets(buf, (int)len, f))
        buf[strcspn(buf, "\r\n")] = '\0';
    else
        snprintf(buf, len, "%s", UNTOUCHED);
}

static void saved_restore_locked(void)
{
    if (strcmp(saved_floor, UNTOUCHED))
        settings_set("laser_floor_density", saved_floor);
    if (strcmp(saved_curve, UNTOUCHED))
        settings_set("laser_dose_curve", saved_curve);
    if (strcmp(saved_gamma, UNTOUCHED))
        settings_set("laser_corner_gamma", saved_gamma);
    unlink(SAVED_MARKER);
    ov_active = 0;
}

int curverec_override_begin(const char *floor, const char *curve, const char *gamma)
{
    pthread_mutex_lock(&ov_mu);
    if (ov_active) {
        pthread_mutex_unlock(&ov_mu);
        return -1;
    }
    snprintf(saved_floor, sizeof(saved_floor), "%s", UNTOUCHED);
    snprintf(saved_curve, sizeof(saved_curve), "%s", UNTOUCHED);
    snprintf(saved_gamma, sizeof(saved_gamma), "%s", UNTOUCHED);
    if (floor && settings_get("laser_floor_density", saved_floor, sizeof(saved_floor)) != 0)
        saved_floor[0] = '\0';
    if (curve && settings_get("laser_dose_curve", saved_curve, sizeof(saved_curve)) != 0)
        saved_curve[0] = '\0';
    if (gamma && settings_get("laser_corner_gamma", saved_gamma, sizeof(saved_gamma)) != 0)
        saved_gamma[0] = '\0';
    /* The originals go to the marker before the keys change, so a
     * daemon that dies or is restarted mid-job restores them at its
     * next start. */
    FILE *f = fopen(SAVED_MARKER, "w");
    if (!f) {
        fflog(LOG_ERR, "curverec: cannot write %s: %s", SAVED_MARKER, strerror(errno));
        pthread_mutex_unlock(&ov_mu);
        return -1;
    }
    fprintf(f, "%s\n%s\n%s\n", saved_floor, saved_curve, saved_gamma);
    fclose(f);
    if (floor)
        settings_set("laser_floor_density", floor);
    if (curve)
        settings_set("laser_dose_curve", curve);
    if (gamma)
        settings_set("laser_corner_gamma", gamma);
    ov_active = 1;
    pthread_mutex_unlock(&ov_mu);
    return 0;
}

void curverec_override_end(void)
{
    pthread_mutex_lock(&ov_mu);
    if (ov_active)
        saved_restore_locked();
    pthread_mutex_unlock(&ov_mu);
}

int curverec_override_active(void)
{
    pthread_mutex_lock(&ov_mu);
    int a = ov_active;
    pthread_mutex_unlock(&ov_mu);
    return a;
}

void curverec_saved(char *floor, size_t fl, char *curve, size_t cl)
{
    pthread_mutex_lock(&ov_mu);
    if (floor)
        snprintf(floor, fl, "%s", strcmp(saved_floor, UNTOUCHED) ? saved_floor : "");
    if (curve)
        snprintf(curve, cl, "%s", strcmp(saved_curve, UNTOUCHED) ? saved_curve : "");
    pthread_mutex_unlock(&ov_mu);
}

void curverec_init(void)
{
    FILE *f = fopen(SAVED_MARKER, "r");
    if (!f)
        return;
    pthread_mutex_lock(&ov_mu);
    saved_line(f, saved_floor, sizeof(saved_floor));
    saved_line(f, saved_curve, sizeof(saved_curve));
    saved_line(f, saved_gamma, sizeof(saved_gamma));
    fclose(f);
    ov_active = 1;
    saved_restore_locked();
    pthread_mutex_unlock(&ov_mu);
    fflog(LOG_WARNING, "curverec: a job was cut short by a daemon exit; the laser keys "
          "it overrode are restored");
}

/* ------------------------------------------------------- the ladder */

static char ladder_lines[LADDER_MAX_LINES][40];
static int ladder_n;
static char result_curve[256];
static curverec_pt result_pts[LADDER_N];
static int result_n;

/* -------------------------------------------------- the pure fitter */

static double win_mean(const long *v, int a, int b)
{
    if (b <= a)
        return NAN;
    double s = 0;
    for (int i = a; i < b; i++)
        s += (double)v[i];
    return s / (b - a);
}

int curverec_fit(const long *hv, const long *tp, int n, double hz,
                 curverec_pt *pts, int max_pts, char *err, size_t elen)
{
    int gap_merge = (int)(GAP_MERGE_S * hz);
    int min_seg = (int)(MIN_SEG_S * hz);
    int seg_a[LADDER_N + 4], seg_b[LADDER_N + 4];
    int nseg = 0, start = -1, last_on = -1;

    for (int i = 0; i <= n; i++) {
        int on = i < n && hv[i] > HV_ON;
        if (on) {
            if (start < 0)
                start = i;
            last_on = i;
        } else if (start >= 0 && (i == n || i - last_on > gap_merge)) {
            if (last_on - start >= min_seg) {
                if (nseg < LADDER_N + 4) {
                    seg_a[nseg] = start;
                    seg_b[nseg] = last_on + 1;
                }
                nseg++;
            }
            start = -1;
        }
    }
    if (nseg != LADDER_N) {
        snprintf(err, elen, "%d discharge segments, the ladder has %d rungs",
                 nseg, LADDER_N);
        return -1;
    }

    double delta[LADDER_N];
    for (int s = 0; s < LADDER_N; s++) {
        int b0 = seg_a[s] - (int)(1.5 * hz);
        int b1 = seg_a[s] - (int)(0.5 * hz);
        if (b0 < 0)
            b0 = 0;
        double base = win_mean(tp, b0, b1);
        double lit = win_mean(tp, seg_a[s] + (int)(0.5 * hz),
                              seg_b[s] - (int)(0.3 * hz));
        if (isnan(base) || isnan(lit)) {
            snprintf(err, elen, "rung %d too short to read", s + 1);
            return -1;
        }
        delta[s] = lit - base;
    }
    if (delta[LADDER_N - 1] <= 0) {
        snprintf(err, elen, "the full-power rung shows no thermopile rise");
        return -1;
    }
    for (int s = 1; s < LADDER_N; s++) {
        if (delta[s] <= delta[s - 1]) {
            snprintf(err, elen, "rung %d does not rise over rung %d: not a "
                     "clean ladder", s + 1, s);
            return -1;
        }
    }
    int out = LADDER_N < max_pts ? LADDER_N : max_pts;
    for (int s = 0; s < out; s++) {
        pts[s].density = LADDER_D[s];
        pts[s].light = delta[s] / delta[LADDER_N - 1] * 100.0;
        if (pts[s].light < 0.01)
            pts[s].light = 0.01;        /* keep the pair strictly increasing */
    }
    return out;
}

int curverec_curve_text(const curverec_pt *pts, int n, char *buf, size_t len)
{
    size_t off = 0;
    buf[0] = '\0';
    for (int i = 0; i < n; i++)
        off += (size_t)snprintf(buf + off, len - off, "%s%g:%.2f", i ? "," : "",
                                pts[i].density, pts[i].light);
    return off < len ? 0 : -1;
}

static void ladder_build(void);

/* -------------------------------------------------- lifecycle */

static void finish_locked(int state, const char *reason)
{
    cr_state = state;
    snprintf(cr_reason, sizeof(cr_reason), "%s", reason ? reason : "");
    curverec_override_end();
    fflog(LOG_INFO, "curverec: %s%s%s",
          state == CR_DONE ? "done" : "failed",
          reason && *reason ? " - " : "", reason ? reason : "");
}

static void fit_locked(void)
{
    char err[96] = "";
    int n = curverec_fit(buf_hv, buf_tp, cr_samples, SAMPLE_HZ,
                         result_pts, LADDER_N, err, sizeof(err));
    if (n < 0) {
        finish_locked(CR_FAILED, err);
        return;
    }
    result_n = n;
    curverec_curve_text(result_pts, n, result_curve, sizeof(result_curve));
    finish_locked(CR_DONE, "");
}

/* The streamer's sample: into the trace, and the state to recording
 * at the first discharge. */
static void on_sample(void *ctx, const jobstream_sample_t *s)
{
    (void)ctx;
    pthread_mutex_lock(&mu);
    if (cr_samples < MAX_SAMPLES) {
        buf_hv[cr_samples] = s->hv;
        buf_tp[cr_samples] = s->tp;
        cr_samples++;
    }
    if (s->hv > HV_ON && cr_state == CR_WAITING) {
        cr_state = CR_RECORDING;
        fflog(LOG_INFO, "curverec: the ladder is firing");
    }
    pthread_mutex_unlock(&mu);
}

static void *record_thread(void *arg)
{
    (void)arg;
    char program[LADDER_MAX_LINES * 40], err[160];
    size_t off = 0;
    ladder_build();
    for (int i = 0; i < ladder_n; i++)
        off += (size_t)snprintf(program + off, sizeof(program) - off, "%s\n", ladder_lines[i]);
    jobstream_lines_t lines = { program, 0 };
    jobstream_cfg_t cfg = {
        .gen = jobstream_lines_gen, .sample = on_sample, .ctx = &lines,
        .abort_flag = &stop_requested, .wait_timeout_s = WAIT_TIMEOUT_S,
        .run_timeout_s = RUN_TIMEOUT_S, .end_dark_s = DARK_END_S,
    };
    jobstream_run_t run;
    int rc = jobstream_run(&cfg, &run, err, sizeof(err));
    pthread_mutex_lock(&mu);
    if (rc == 0 || (stop_requested && run.lit)) {
        if (cr_samples >= MAX_SAMPLES)
            finish_locked(CR_FAILED, "the record ran out of room");
        else
            fit_locked();
    } else if (stop_requested)
        finish_locked(CR_FAILED, "stopped before the ladder fired");
    else
        finish_locked(CR_FAILED, err);
    pthread_mutex_unlock(&mu);
    return NULL;
}

int curverec_start(char *err, size_t elen)
{
    pthread_mutex_lock(&mu);
    if (cr_state == CR_WAITING || cr_state == CR_RECORDING) {
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "a recording is already running");
        return -1;
    }
    if (wizdark_running()) {
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "a commissioning wizard holds the machine");
        return -1;
    }
    if (thread_live) {
        pthread_join(thread, NULL);
        thread_live = 0;
    }
    if (!buf_hv)
        buf_hv = calloc(MAX_SAMPLES, sizeof(long));
    if (!buf_tp)
        buf_tp = calloc(MAX_SAMPLES, sizeof(long));
    if (!buf_hv || !buf_tp) {
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "out of memory");
        return -1;
    }
    if (jobstream_sender_connected() == 1) {
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "a sender is connected to the machine - close it "
                 "first (the recorder streams the ladder itself)");
        return -1;
    }
    if (curverec_override_begin("0", "off", NULL) != 0) {
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "the laser keys are held by another job");
        return -1;
    }
    cr_state = CR_WAITING;
    cr_reason[0] = '\0';
    cr_samples = 0;
    result_n = 0;
    result_curve[0] = '\0';
    stop_requested = 0;
    cr_started = time(NULL);
    if (pthread_create(&thread, NULL, record_thread, NULL) != 0) {
        finish_locked(CR_FAILED, "cannot start the sampler");
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "cannot start the sampler");
        return -1;
    }
    thread_live = 1;
    pthread_mutex_unlock(&mu);
    fflog(LOG_INFO, "curverec: streaming the ladder (floor and curve "
          "overridden for the run) - waiting for the button");
    return 0;
}

void curverec_stop(void)
{
    pthread_mutex_lock(&mu);
    int live = cr_state == CR_WAITING || cr_state == CR_RECORDING;
    if (live)
        stop_requested = 1;
    pthread_mutex_unlock(&mu);
    if (live && thread_live) {
        pthread_join(thread, NULL);
        thread_live = 0;
    }
}

int curverec_status_json(char *buf, size_t len)
{
    static const char *names[] = { "idle", "waiting", "recording", "done", "failed" };
    pthread_mutex_lock(&mu);
    size_t off = 0;
    off += (size_t)snprintf(buf + off, len - off,
        "{\"state\":\"%s\",\"reason\":\"%s\",\"elapsed_s\":%ld,"
        "\"samples\":%d,\"curve\":\"%s\",\"points\":[",
        names[cr_state], cr_reason,
        cr_state == CR_WAITING || cr_state == CR_RECORDING
            ? (long)difftime(time(NULL), cr_started) : 0,
        cr_samples, result_curve);
    for (int i = 0; i < result_n && off < len - 48; i++)
        off += (size_t)snprintf(buf + off, len - off,
            "%s{\"density\":%g,\"light\":%.2f}", i ? "," : "",
            result_pts[i].density, result_pts[i].light);
    snprintf(buf + off, len - off, "]}");
    pthread_mutex_unlock(&mu);
    return 0;
}

/* The ladder job, absolute from the origin: rung i cuts X0..X100 at
 * Y = i mm. The same lines the recorder streams. */
static void ladder_build(void)
{
    ladder_n = 0;
    snprintf(ladder_lines[ladder_n++], 40, "G21");
    snprintf(ladder_lines[ladder_n++], 40, "G90");
    snprintf(ladder_lines[ladder_n++], 40, "G0 X0 Y0");
    snprintf(ladder_lines[ladder_n++], 40, "M3");
    for (int i = 0; i < LADDER_N; i++) {
        snprintf(ladder_lines[ladder_n++], 40, "G0 X0 Y%d", i);
        /* The rapid between rungs is only ~0.5 s and the fitter merges
         * dark gaps up to a second (a low rung's own pulsing must not
         * split it), so each rung is separated by a real dark dwell. */
        snprintf(ladder_lines[ladder_n++], 40, "G4 P2");
        snprintf(ladder_lines[ladder_n++], 40, "S%d", (int)(LADDER_D[i] * 10.0));
        snprintf(ladder_lines[ladder_n++], 40, "G1 X100 F600");
    }
    snprintf(ladder_lines[ladder_n++], 40, "M5");
    snprintf(ladder_lines[ladder_n++], 40, "M2");
}

int curverec_ladder_gcode(char *buf, size_t len)
{
    ladder_build();
    size_t off = 0;
    off += (size_t)snprintf(buf + off, len - off,
        "; ForgeFIRM dose-curve ladder - the job the panel's recorder streams.\n"
        "; Absolute from X0 Y0: 100 mm of +X travel, %d mm of +Y, scrap under\n"
        "; the whole area. One line per rung, low to full power.\n", LADDER_N);
    for (int i = 0; i < ladder_n; i++)
        off += (size_t)snprintf(buf + off, len - off, "%s\n", ladder_lines[i]);
    return off < len ? 0 : -1;
}
