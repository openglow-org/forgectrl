/*
 * wizlive.c - the setup burns: the head's focus and the laser floor
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The two steps of the setup that fire the laser: Focus (laser.focus)
 * and the optional Laser floor (laser.floor). Each is self-contained
 * and runs on its own scrap of wood: it asks that the gantry stops are
 * installed, references the lens on its hall sensor, takes the
 * controller in loopback posture, asks that the head is at its home
 * against the gantry stops (or frees the motors so it can be pushed
 * there by hand), asks for the scrap in the back-left corner and its
 * thickness, and burns its card at a fixed place from home: every
 * program declares X0 Y0 at the head and its tail brings the head back.
 * Nothing carries a position from one step to the next. The program
 * streams through jobstream with the emission witnesses sampled, and
 * what the step measured goes through the validated settings path. Both
 * run on the runner's worker (wizdark.c) with the same prompts and
 * record. The laser latch opens only through the controller's arm gate
 * with the operator's press; every cooling gate stands; a lid opened
 * mid-burn cancels the program, as any job.
 */
#define _GNU_SOURCE
#include "wizrun.h"
#include "setup.h"
#include "cool.h"
#include "curverec.h"
#include "fflog.h"
#include "jobrun.h"
#include "led.h"
#include "lens.h"
#include "paths.h"
#include "settings.h"
#include "sheet.h"
#include "status.h"
#include "super.h"
#include "wizlive.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* The lens. Z is the focal point's height above the tray: Z 0 focuses
 * on the bed, Z 3 on the top of 3 mm material. The lens is a 2 in lens
 * under a collimated beam, so the focal point moves 1:1 with the lens
 * and +Z is lens up. The head's one reference is the hall sensor's
 * rising edge (the first position reading home going up), which every
 * burn takes before its controller starts; the wizard counts the lens in
 * half-steps from that edge, up positive. The edge's focal height, the
 * free half-steps each way, and the reach they give are the lens frame
 * (lens.h), read from the same settings the controller opens its Z soft
 * limit from, so a Z sent from here is one the controller takes. The
 * free half-steps are the Lens travel check's (wizdark.c, lensfind.c),
 * or the fallback window that clears the stops on any head. */
static double edge_z(void);
static double z_of_k(double k);
#define FLOOR_STEP    2.0
/* Every look at the card after its burn carries this. */
#define STILL "Look, but do not move the scrap until the step is done."

#define WAIT_TIMEOUT_S 600.0    /* the arm and the press */
#define PICK_TIMEOUT_S 1800     /* a look at the card after a burn: the burn is not repeated */
#define RUN_TIMEOUT_S 1500.0
#define TP_SEEN_MIN   100       /* a beam sensor rise this size reads as the beam seen */

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

/* --------------------------------------------------- the head's home */

/* The X and Y motors are released: the driver's marker stands
 * (glowforge_release.c). It is the release's own record, and it carries
 * a release across a controller restart. */
static int motors_released(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/motors.released", ff_run_dir());
    return access(path, F_OK) == 0;
}

/* $MD or $ME to the running controller, judged by the marker: the
 * release raises its alarm line ahead of its ok. 0 when the motors are
 * as wanted. */
static int release_send(const char *cmd, int want_released)
{
    int fd = wiz_grbl_connect();
    if (fd < 0)
        return -1;
    char line[160];
    wiz_grbl_expect(fd, "Grbl", line, sizeof(line), 5);
    wiz_grbl_send(fd, cmd);
    wiz_grbl_expect(fd, "ok", line, sizeof(line), 5);
    close(fd);
    for (int i = 0; i < 20 && motors_released() != want_released; i++)
        wiz_msleep(100);
    return motors_released() == want_released ? 0 : -1;
}

/* Every step starts with the head at its home, the back-left against the
 * gantry stops, and the program declares X0 Y0 there: the operator says
 * the head is home, or releases the motors and pushes it there by hand.
 * The motors are energized again only when the operator continues, with
 * the lid closed; a step aborted while they are free leaves them free, and
 * the next step finds them so and asks for the push again. 0 home, or
 * -1 (aborted, or the error set). */
static int head_home(void)
{
    static const char *const opts[] = { "The head is home", "Release the motors" };
    char a[32];
    while (!motors_released()) {
        if (wiz_ask("choice", "home",
                    "Every card is drawn from the head's home: the back-left corner, against the "
                    "gantry stops. Is the head there?",
                    opts, 2, a, sizeof(a)) != 0)
            return -1;
        if (!strcmp(a, opts[0])) {
            wiz_log("the head is home, the operator says");
            return 0;
        }
        if (!machine_lid_closed()) {
            wiz_log("the lid is open: close it, then release the motors");
            continue;
        }
        if (release_send("$MD", 1) != 0) {
            wiz_finish_err("the X and Y motors could not be released");
            return -1;
        }
        wiz_log("the X and Y motors are released");
    }
    for (;;) {
        wiz_phase("the motors are free: the head goes home by hand");
        if (wiz_ask_continue("home-push",
                             "The X and Y motors are free. Open the lid and push the head gently "
                             "to the back-left corner, against the gantry stops, keeping the "
                             "gantry square to the machine as it goes back. Close the lid, then "
                             "continue: the motors hold the head there again.") != 0)
            return -1;
        if (!machine_lid_closed()) {
            wiz_log("the lid is open: close it, then continue");
            continue;
        }
        if (release_send("$ME", 0) != 0) {
            wiz_finish_err("the X and Y motors could not be energized");
            return -1;
        }
        wiz_log("the head is home by hand; the motors hold it");
        return 0;
    }
}

/* ----------------------------------------------------------- the steps */

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

/* The controller's Z steps per mm ($102), read when a session is live. */
static double z_spm = LENS_STEPS_PER_MM;

/* The focal height when the lens is on the hall edge: the focus burn's
 * number for this head, else the bench reference machine's. */
static double edge_z(void)
{
    return lens_edge_z();
}

/* Lens half-steps from the hall edge (up positive) into Z. */
static double z_of_k(double k)
{
    return lens_z_of_k(edge_z(), z_spm, k);
}

/* The window the settings hold, and whether it was measured on this
 * head's stops: the Lens travel check found them. A check that could not
 * keeps the reduced window, and a head change clears the window. */
static int window_measured(int *down, int *up)
{
    if (!lens_window(down, up))
        return 0;
    json_t *r = setup_wizard_result("lens.travel");
    int found = !json_is_false(json_object_get(json_object_get(r, "window"), "found"));
    json_decref(r);
    return found;
}

/* ------------------------------------------------------- the session */

typedef struct {
    int stopped, local;
    double spmx, spmy, spmz;
    double thickness;           /* the scrap's, as the operator gave it */
    int z_taken;                /* the lens unlocked for the session */
    int g92;                    /* the program's head handed out its G92 origin */
    int clean;                  /* the program played to its tail: the lens is back on its
                                   reference and the G92 origin cleared */
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
 * controller back in loopback posture with its settings published, the
 * head at its home, and the cooling engine ready for its arm. */
static int live_begin(live_t *L, json_t *r)
{
    memset(L, 0, sizeof(*L));
    wiz_phase("stopping the motion controller");
    if (wiz_controller_stop() != 0) {
        wiz_finish_err("could not stop the motion controller");
        return -1;
    }
    L->stopped = 1;
    wiz_phase("the lens finds its reference on the hall sensor");
    int rc = wiz_z_reference(r, 2);
    if (rc != 0) {
        wiz_finish_err(rc == -2 ? "the lens never reached the hall sensor in 200 steps"
                       : rc == -3 ? "the lens reference passes disagree"
                                  : "the lens reference could not run");
        return -1;
    }
    /* The controller takes its Z frame from the reference marker: write
     * it here, so the controller below holds the reference just taken
     * whatever the broker's own hunt did (it skips the hunt while the
     * motors are released). */
    if (wiz_lens_rehome() != 0) {
        wiz_finish_err("the lens reference could not be handed to the controller");
        return -1;
    }
    super_set_local(1);
    L->local = 1;
    wiz_controller_start();
    L->stopped = 0;
    if (wait_named("starting the motion controller", wiz_grbl_up, NULL, 60) != 0) {
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
    if (head_home() != 0)
        return -1;
    rc = wait_named("the cooling engine takes the controller's report", engine_ready, NULL, 20);
    if (rc == -1)
        return -1;
    if (rc != 0) {
        wiz_finish_err("the cooling engine did not take the controller's report, or holds fire");
        return -1;
    }
    wiz_msleep(1200);               /* the driver's next read of the verdict */
    /* The driver starts with the lens locked out of the pulse path (the
     * factory's posture: a sender's Z never moves the lens) and the lens
     * motor at its hold current. The burns move the lens from the
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

/* G92.1 to the running controller after an unlock: a program stopped
 * before its tail leaves the origin it declared, which the controller
 * stores and would restore at its next start. 0 when it was taken. */
static int clear_g92(void)
{
    int fd = wiz_grbl_connect();
    if (fd < 0)
        return -1;
    char line[160];
    wiz_grbl_expect(fd, "Grbl", line, sizeof(line), 5);
    wiz_grbl_send(fd, "$X");
    wiz_grbl_expect(fd, "ok", line, sizeof(line), 5);
    wiz_grbl_send(fd, "G92.1");
    int rc = wiz_grbl_expect(fd, "ok", line, sizeof(line), 5);
    close(fd);
    return rc;
}

static void live_end(live_t *L)
{
    /* Whatever the exit, the pulse engine finishes before the controller
     * is stopped (a stop at Idle would discard the tail) and before the
     * lens is stepped. */
    wiz_phase("the machine goes back to its normal posture");
    wiz_kernel_settle(600.0);
    if (L->local && L->g92 && !L->clean && clear_g92() != 0)
        wiz_log("the burn's G92 origin could not be cleared: send G92.1 before a job");
    if (L->local)
        wiz_controller_stop();
    /* A session that did not play to its tail may leave the lens off its
     * reference, and the next controller takes its Z frame from the
     * reference marker: the lens goes back on its hall edge, or, when it
     * cannot, the marker goes and the controller holds Z unreferenced. */
    if (!L->clean && (L->local || L->stopped) && wiz_lens_rehome() != 0)
        wiz_log("the lens could not find its reference: Z stays unreferenced until it does");
    if (L->z_taken) {
        wiz_wr_attr("cnc/motor_lock", "8");
        wiz_wr_attr("head/z_current", "1");
    }
    if (L->local)
        super_set_local(0);
    if (L->local || L->stopped)
        wiz_controller_start();
}

/* The tail every program ends with: the lens back on the reference the
 * G92 declared (edge, the height the program's head declared it at), the
 * head back home, the G92 origin cleared (the controller stores a G92
 * offset and restores it at its next start, where it would shift a
 * sender's own work offsets), then the program end, which the
 * controller acknowledges once the stream is produced. 0 past the last
 * line. */
static int tail_line(int i, double edge, char *buf, size_t len)
{
    switch (i) {
    case 0:
        snprintf(buf, len, "G0 Z%.2f", edge);
        return 1;
    case 1:
        snprintf(buf, len, "G0 X0 Y0");
        return 1;
    case 2:
        snprintf(buf, len, "G92.1");
        return 1;
    case 3:
        snprintf(buf, len, "M2");
        return 1;
    default:
        return 0;
    }
}

/* The Z a burn runs at: the focus card burns its text on the reference
 * and steps the ladder from there; the floor runs at the focal point on
 * the scrap's surface, which is Z = the thickness, kept inside the window
 * the lens reaches from its reference. */
static double card_z(const char *id, double thickness)
{
    if (!strcmp(id, "laser.focus"))
        return edge_z();
    int down, up;
    lens_window(&down, &up);
    double lo = z_of_k(-down), hi = z_of_k(up);
    return thickness < lo ? lo : thickness > hi ? hi : thickness;
}

/* What every burn asks before its arm: the gantry stops installed (the
 * home is against them; the factory's corner is springy, and its X end
 * is a cable, so nothing is pushed against it), the machine taken with
 * the head at home, then the scrap in the back-left corner and its
 * thickness. 0, or -1 (aborted, or the error set). */
static int burn_setup(live_t *L, const char *id, json_t *r)
{
    int stops = wiz_ask_confirm("gantry-stops",
                                "This burn is drawn from the head's home against the gantry stops "
                                "(Install, Gantry stops). Are the gantry stops installed?");
    if (stops < 0)
        return -1;
    if (stops == 0) {
        wiz_finish_err("the burns need the gantry stops: install them (Install, Gantry stops), "
                       "then run this step again");
        return -1;
    }
    if (live_begin(L, r) != 0)
        return -1;
    float w, h;
    char u1[32], u2[32], text[320];
    sheet_scrap_size(sheet_card(id), &w, &h);
    snprintf(text, sizeof(text), "Open the lid and put a flat scrap of wood, at least %s by %s, in "
             "the back-left corner of the cut area: as far left as it goes, its top edge at the top "
             "of the cut area. Close the lid.", wiz_len(w, u1, sizeof(u1)), wiz_len(h, u2, sizeof(u2)));
    if (wiz_ask_continue("scrap", text) != 0)
        return -1;
    static const char *const th[] = { "Set" };
    char a[24];
    snprintf(text, sizeof(text), "The scrap's thickness in %s.", wiz_imperial() ? "inches" : "millimeters");
    for (;;) {
        if (wiz_ask("number", "thickness", text, th, 1, a, sizeof(a)) != 0)
            return -1;
        double t = wiz_len_mm(a);
        if (!(t > 0 && t <= 30)) {
            wiz_log("the thickness is a decimal number, more than 0 and at most %s",
                    wiz_len(30, u1, sizeof(u1)));
            continue;
        }
        /* The floor burns at the focus on the scrap's surface: a surface
         * the lens cannot focus on would measure a defocused ladder and
         * write a floor that is too high. */
        if (!strcmp(id, "laser.floor")) {
            int down, up;
            lens_window(&down, &up);
            double lo = z_of_k(-down), hi = z_of_k(up);
            if (t < lo || t > hi) {
                wiz_log("the lens focuses %s to %s above the tray: use a scrap in that range",
                        wiz_len(lo, u1, sizeof(u1)), wiz_len(hi, u2, sizeof(u2)));
                continue;
            }
        }
        L->thickness = t;
        break;
    }
    json_object_set_new(r, "thickness_mm", json_real(L->thickness));
    return 0;
}

/* ------------------------------------------------ the program generator */

#define G92_LINE 3              /* gen_init: $X, G21, G90, then the G92 line */

typedef struct {
    char head[6][80];
    int nhead, ihead;
    const char *body;
    size_t off;
    int tail;               /* the lines after the body: Z up, home, G92.1, M2 */
    double edge;            /* the reference height the head declared */
    int lit;                    /* the tube has been seen lit: the phase said so */
    int lit_now, ended;         /* the tube's state now; the program streamed to its end */
    double t_lit0, t_lit_last, last_phase;
    double tail_s;              /* the dark tail the run waits for after the last discharge */
    const char *tail_what;      /* what the tail is for, in the phase */
    int press_open;             /* the press prompt is up: the button is lit */
    double t_led0;              /* when it lit */
} gen_t;

/* The frame every program starts with: units, absolute, the origin at
 * the head (at its home: the session saw to it), the lens at the working
 * Z. */
static void gen_init(gen_t *g, double z, const char *body)
{
    memset(g, 0, sizeof(*g));
    g->edge = edge_z();
    snprintf(g->head[g->nhead++], sizeof(g->head[0]), "$X");
    snprintf(g->head[g->nhead++], sizeof(g->head[0]), "G21");
    snprintf(g->head[g->nhead++], sizeof(g->head[0]), "G90");
    /* The lens is referenced on the hall edge at the controller's start,
     * so the Z envelope is already open: set the origin. */
    snprintf(g->head[g->nhead++], sizeof(g->head[0]), "G92 X0 Y0 Z%.2f", g->edge);
    snprintf(g->head[g->nhead++], sizeof(g->head[0]), "G0 Z%.2f", z);
    g->body = body;
}

static int gen_next(void *ctx, char *buf, size_t len)
{
    gen_t *g = ctx;
    if (g->ihead < g->nhead) {
        snprintf(buf, len, "%s", g->head[g->ihead++]);
        return 1;
    }
    for (;;) {
        const char *p = g->body ? g->body + g->off : "";
        while (*p == '\n' || *p == '\r')
            p++;
        if (!*p) {
            if (!tail_line(g->tail, g->edge, buf, len)) {
                g->ended = 1;
                return 0;
            }
            g->tail++;
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
         * at the burn's end, or by the daemon's next start) and M102
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
        /* an unknown directive: skipped */
    }
}

static void gen_sample(void *ctx, const jobstream_sample_t *s)
{
    gen_t *g = ctx;
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
     * it as a prompt with the seconds from the moment the button lights,
     * and takes it down the moment the button goes out (the press taken)
     * or the tube lights: the button is read at every sample, so the
     * prompt never stands over a press already made, which an operator
     * would answer with another press, and a press during the job pauses
     * it. */
    int led = g->lit ? 0 : led_lit();
    if (g->press_open && (g->lit || led == 0)) {
        wiz_wait_close();
        g->press_open = 0;
    }
    if (!g->lit && !g->press_open && led == 1) {
        g->press_open = 1;
        g->t_led0 = s->t;
        wiz_wait_open("press", "The button is lit white. Press it now: the laser fires after "
                               "your press.", (int)WAIT_TIMEOUT_S);
    }
    if (s->t - g->last_phase >= 1.0) {
        g->last_phase = s->t;
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
}

/* Stream the program as a job of the runner's, inside the wizard's own
 * hold of the machine (so a connected Grbl client refuses it, in the
 * lease's words); the run's outcome in `run`. */
static int stream(gen_t *g, jobstream_run_t *run, double end_dark_s, char *err, size_t elen)
{
    const char *wizard = wiz_lease_owner();
    const char *id = strchr(wizard, ':');
    char owner[48];
    snprintf(owner, sizeof(owner), "job:%s", id ? id + 1 : wizard);
    jobrun_cfg_t cfg = {
        .owner = owner, .under = wizard,
        .stream = { .gen = gen_next, .sample = gen_sample, .ctx = g,
                    .abort_flag = wiz_abort_flag(), .wait_timeout_s = WAIT_TIMEOUT_S,
                    .run_timeout_s = RUN_TIMEOUT_S, .end_dark_s = end_dark_s },
    };
    return jobrun_sync(&cfg, run, err, elen);
}

/* The emission witnesses into the result. The tube current and the
 * kernel's LASER_ON samples must both have seen the beam for the burn to
 * count. The head's beam sensor is recorded and never required: a head
 * may have none, or one that reads differently from the bench
 * reference's. */
static int witnessed(const jobstream_run_t *run, json_t *r)
{
    long tp_delta = run->tp_base >= 0 ? run->tp_max - run->tp_base : 0;
    json_t *e = json_object();
    json_object_set_new(e, "hv_max", json_integer(run->hv_max));
    json_object_set_new(e, "laser_on_samples", json_integer(run->lon_max));
    json_object_set_new(e, "thermopile_delta", json_integer(tp_delta));
    json_object_set_new(e, "beam_sensor_seen", json_boolean(tp_delta >= TP_SEEN_MIN));
    json_object_set_new(e, "lit_s", json_real(run->lit ? run->t_last_lit - run->t_first_lit : 0));
    json_object_set_new(r, "emission", e);
    if (run->lit && tp_delta < TP_SEEN_MIN)
        wiz_log("the beam sensor gave no output (rose %ld); the tube current and LASER_ON count",
                tp_delta);
    return run->lit && run->lon_max > 0;
}

static void not_witnessed(const jobstream_run_t *run)
{
    if (!run->lit)
        wiz_finish_err("no discharge: the tube current never rose");
    else
        wiz_finish_err("the kernel never sampled LASER_ON while the tube current rose");
}

/* -------------------------------------------------------- the builds */

/* The parameters of a card's build: the mark dose, and the ladders'
 * lines from the lens window and the floor's steps. */
typedef struct {
    int mark_s, mark_feed, text_s;
    double z_text;
    double focus_z[SHEET_FOCUS_N];
    int focus_k[SHEET_FOCUS_N];     /* the lines' half-steps from the hall edge */
    double floor_d[SHEET_FLOOR_N];
} build_t;

static void build_defaults(build_t *b)
{
    memset(b, 0, sizeof(*b));
    b->mark_s = SHEET_MARK_S;
    b->mark_feed = SHEET_MARK_FEED;
    b->text_s = b->mark_s;
    b->z_text = edge_z();
    /* The ladder: twelve lines spread over the window, the top line at
     * its top and the bottom line at its bottom, on whole half-steps. */
    int down, up;
    lens_window(&down, &up);
    lens_ladder_k(down, up, SHEET_FOCUS_N, b->focus_k);
    for (int i = 0; i < SHEET_FOCUS_N; i++)
        b->focus_z[i] = z_of_k(b->focus_k[i]);
    for (int i = 0; i < SHEET_FLOOR_N; i++)
        b->floor_d[i] = FLOOR_STEP * (i + 1);
}

/* The preview and the program of a burn with the build's parameters. */
static int build_card(const char *id, build_t *b, sheet_paths_t *pv, sheet_text_t *pg)
{
    const sheet_card_t *c = sheet_card(id);
    if (!c)
        return -1;
    if (!strcmp(id, "laser.focus"))
        sheet_build_focus(pv, pg, c, b->focus_z, SHEET_FOCUS_N, b->z_text, b->text_s, b->mark_feed);
    else if (!strcmp(id, "laser.floor"))
        sheet_build_floor(pv, pg, c, b->floor_d, SHEET_FLOOR_N, b->text_s, b->mark_feed);
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
    sheet_paths_t pv;
    sheet_text_t pg;
    sheet_paths_init(&pv);
    sheet_text_init(&pg);
    float w, h;
    sheet_scrap_size(sheet_card(id), &w, &h);
    int rc = build_card(id, b, &pv, &pg);
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
    sheet_paths_t pv;
    sheet_text_t pg;
    sheet_paths_init(&pv);
    sheet_text_init(&pg);
    sheet_text_line(out, "; ForgeFIRM setup: %s, the body the wizard streams after", id);
    if (!strcmp(id, "laser.focus"))
        sheet_text_line(out, "; the head the wizard sends first: $X G21 G90, G92 X0 Y0 at the"
                             " head's home, G0 Z%.2f", edge_z());
    else
        sheet_text_line(out, "; the head the wizard sends first: $X G21 G90, G92 X0 Y0 at the"
                             " head's home, G0 Z at the scrap's thickness");
    int rc = build_card(id, b, &pv, &pg);
    if (rc == 0 && pg.buf)
        sheet_text_append(out, pg.buf, pg.len);
    /* The tail the wizard streams after the body, as it streams it. */
    for (int i = 0; rc == 0; i++) {
        char line[80];
        if (!tail_line(i, edge_z(), line, sizeof(line)))
            break;
        sheet_text_line(out, "%s", line);
    }
    sheet_paths_free(&pv);
    sheet_text_free(&pg);
    free(b);
    return rc;
}

/* ------------------------------------------------------- the wizards */

/* One line against the reach; 1 with `why` when it asks a Z outside it. */
static int z_off_reach(const char *line, int n, double lo, double hi, char *why, size_t wlen)
{
    double z;
    if (!lens_line_z(line, &z) || (z >= lo - 0.01 && z <= hi + 0.01))
        return 0;
    snprintf(why, wlen, "line %d asks Z %.2f, outside the lens reach %.2f to %.2f (%s)", n, z, lo, hi,
             line);
    return 1;
}

/* Every Z the program will command, head to tail, against the reach the
 * lens has now. A Z outside the reach is one the controller refuses with
 * its soft-limit alarm, and it refuses it at parse time, while the body
 * is still burning, so the program is judged whole before its first line
 * goes out and a card that cannot run burns nothing. 0 fits; -1 with the
 * offending line in `why`. */
static int program_fits(const gen_t *g, const char *body, char *why, size_t wlen)
{
    int down, up, n = 0;
    lens_window(&down, &up);
    double lo, hi;
    lens_reach(g->edge, z_spm, down, up, &lo, &hi);
    for (int i = 0; i < g->nhead; i++)
        if (z_off_reach(g->head[i], ++n, lo, hi, why, wlen))
            return -1;
    for (const char *p = body; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[96];
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        if (z_off_reach(line, ++n, lo, hi, why, wlen))
            return -1;
        p = nl ? nl + 1 : p + len;
    }
    for (int i = 0;; i++) {
        char line[80];
        if (!tail_line(i, g->edge, line, sizeof(line)))
            break;
        if (z_off_reach(line, ++n, lo, hi, why, wlen))
            return -1;
    }
    return 0;
}

/* One armed burn of a build: the program streamed, the witnesses read
 * into r. 0 witnessed, -1 (the error is set). */
static int burn(live_t *L, build_t *b, const char *id, gen_t *g, jobstream_run_t *run, json_t *r)
{
    sheet_paths_t pv;
    sheet_text_t pg;
    sheet_paths_init(&pv);
    sheet_text_init(&pg);
    int rc = build_card(id, b, &pv, &pg);
    sheet_paths_free(&pv);
    if (rc != 0 || pg.oom || !pg.buf) {
        sheet_text_free(&pg);
        wiz_finish_err("the program could not be built");
        return -1;
    }
    gen_init(g, card_z(id, L->thickness), pg.buf);
    char why[200];
    if (program_fits(g, pg.buf, why, sizeof(why)) != 0) {
        sheet_text_free(&pg);
        g->body = NULL;
        wiz_finish_err("the program does not fit the lens: %s", why);
        return -1;
    }
    wiz_phase("the program goes to the controller, the button lights at the arm");
    char err[160];
    /* The controller acknowledges the program end when the stream is
     * produced, one queue depth before the pulse engine has played it:
     * the run follows the tube until it has been dark for a while, and
     * then the kernel is waited for before anything else touches the
     * controller. */
    g->tail_s = 2.0;
    rc = stream(g, run, g->tail_s, err, sizeof(err));
    L->g92 = g->ihead > G92_LINE;
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
    /* The tail played: the lens is on its reference, the head home, and
     * the G92 origin cleared. */
    L->clean = g->ended;
    wiz_log("streamed %d lines; tube current peak %ld, LASER_ON samples %ld, thermopile +%ld",
            run->sent, run->hv_max, run->lon_max,
            run->tp_base >= 0 ? run->tp_max - run->tp_base : 0);
    if (!witnessed(run, r)) {
        not_witnessed(run);
        return -1;
    }
    return 0;
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
    if (burn_setup(&L, "laser.focus", r) != 0)
        goto out;
    build_defaults(b);
    /* The ladder: twelve lines over the window the settings hold (the
     * Lens travel check's, else the fallback); build_defaults placed
     * them. Nothing here can reach a stop. */
    int down, up;
    int found = window_measured(&down, &up);
    char text[640], u1[32], u2[32], u3[32], u4[32];
    snprintf(text, sizeof(text),
             "The lens is on its hall reference. Twelve %s lines burn heavy, the lens stepped "
             "from %s above the reference to %s below it%s. "
             "Lid closed. Continue, then press the button when it lights white; this page "
             "says when.",
             wiz_len(sheet_ladder_len(sheet_card("laser.focus")), u1, sizeof(u1)),
             wiz_len(up / z_spm, u2, sizeof(u2)),
             wiz_len(down / z_spm, u3, sizeof(u3)),
             found ? "" : " (a reduced travel: run the Lens travel check for this head's own)");
    if (wiz_ask_continue("focus-arm", text) != 0)
        goto out;
    /* The ladder as it burns, for the record: the twelve heights and the
     * window they span. */
    json_t *ladder = json_array();
    for (int i = 0; i < SHEET_FOCUS_N; i++)
        json_array_append_new(ladder, json_real(b->focus_z[i]));
    json_object_set_new(r, "ladder_z", ladder);
    json_object_set_new(r, "window", json_pack("{s:i,s:i,s:f,s:f}", "below", down, "above", up,
                                               "reach_min", z_of_k(-down), "reach_max", z_of_k(up)));
    if (burn(&L, b, "laser.focus", &g, &run, r) != 0)
        goto out;
    wiz_progress(70);
    const char *opts[SHEET_FOCUS_N];
    char names[SHEET_FOCUS_N][8];
    for (int i = 0; i < SHEET_FOCUS_N; i++) {
        snprintf(names[i], sizeof(names[i]), "%d", i + 1);
        opts[i] = names[i];
    }
    char picks[40];
    if (wiz_ask_t("multichoice", "focus-pick",
                  "Which dark line is the narrowest, with the crispest edges? Count from the "
                  "top, 1 to 12. Neighbors can look the same, since the depth of focus spans a "
                  "few lines: choose every one that does, and the middle is taken. Then Done. "
                  STILL,
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
                       "it: use a scrap nearer 1/8 in (3 mm) thick and run the step again");
        goto out;
    }
    double thickness = L.thickness;
    /* The lens moves with its focal point, so the picked line's half-steps
     * from the hall edge, over the screw's scale, is how far the focus at
     * the edge sits below the material top: the one number this head
     * needs. The window around the edge is the focal range. */
    double k = (b->focus_k[lo - 1] + b->focus_k[hi - 1]) / 2.0;
    double edge = thickness - k / z_spm;
    double z_lo = edge - down / z_spm, z_hi = edge + up / z_spm;
    /* The window is the Lens travel check's (or the fallback); the edge
     * is this step's number. */
    char v[16];
    snprintf(v, sizeof(v), "%.2f", edge);
    const char *keys[] = { "lens_hall_edge_z_mm" };
    const char *vals[] = { v };
    char err[96];
    if (wiz_write_settings(keys, vals, 1, applied, err, sizeof(err)) != 0) {
        wiz_finish_err("%s", err);
        goto out;
    }
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
    snprintf(sum, sizeof(sum), "Line %g on %s: with the lens on its hall reference the focus is "
             "%s above the tray. A home puts the lens there, sets Z to that height, and parks "
             "the focus at the park height; Z counts the focus height above the bed. The lens "
             "reaches %s to %s%s.", pick, u1, u2, u3, u4,
             found ? "" : ", a reduced travel until the Lens travel check runs");
    json_object_set_new(r, "summary", json_string(sum));
    wiz_progress(100);
    wiz_finish_ok(1, r, applied);
out:
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
    /* The floor burns at the focus Focus measured on this head: a Focus
     * flagged again (a head change) measured another one. */
    if (setup_wizard_version("laser.focus") < 1 || setup_flagged("laser.focus")) {
        wiz_finish_err("run Focus first: the floor burns at the focus it measures");
        goto out;
    }
    if (burn_setup(&L, "laser.floor", r) != 0)
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
    if (burn(&L, b, "laser.floor", &g, &run, r) != 0)
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
    int offered = 0;
    for (int i = 0; i < SHEET_FLOOR_N; i++)
        offered |= !strcmp(a, names[i]);
    if (!offered) {
        wiz_finish_err("the answer is not one of the rungs");
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
    free(b);
    live_end(&L);
    json_decref(r);
    json_decref(applied);
}

/* --------------------------------------------------------- the table */

const wiz_entry_t wizlive_table[] = {
    { "laser.focus", run_focus, 1 },
    { "laser.floor", run_floor, 1 },
};
const size_t wizlive_n = sizeof(wizlive_table) / sizeof(*wizlive_table);

int wizlive_known(const char *id)
{
    for (size_t i = 0; id && i < wizlive_n; i++)
        if (!strcmp(wizlive_table[i].id, id))
            return 1;
    return 0;
}
