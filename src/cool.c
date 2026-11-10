/*
 * cool.c - forgectrl: the cooling engine
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Single owner of the thermal hardware (fans, pump, TEC, loop heater)
 * for every controller mode, per the cooling-engine contract
 * (https://docs.forgefirm.org/technical/forgefirm/cooling-engine/). Job state arrives
 * as level-triggered reports (POST /cool/state, ~1 Hz); the engine
 * publishes its verdict to /run/forgefirm/cooling.state (atomic
 * replace, ~1 Hz). Enforcement - the laser fire gate and feed-hold
 * issuance - stays in-process with the active controller; a reader
 * that finds the verdict file missing or stale treats it as
 * fire-blocked. The hardware AND-gate remains the safety boundary;
 * everything here is equipment protection.
 *
 * Policy, built from the factory's own numbers (captured pulse-file
 * headers + settings map): fan duties AAid=204/AArd=1023, EFrd=65535,
 * IFrd=43278; coolant windows CM* in millidegrees - run ceiling 33 C
 * (CMrx), start/resume gate 31 C (CMwx). All thresholds user-tunable:
 * cool_* keys in the shared machine settings (re-read at every run
 * start), GFCOOL_* env vars as bench overrides that win for the
 * process lifetime.
 *
 * - IDLE: pump on, TEC off, purge on, fans at factory idle, and this
 *   engine's heater off (the warm-up's 40 % would warm the loop ~1.5 C
 *   in minutes for no benefit while nothing can fire). The flow watch,
 *   when an owner turns it on, holds the heater at a few percent: about
 *   0.5 C over 90 min on the bench reference.
 * - RUN (reported mode "run", or armed whatever the mode says):
 *   cut-profile fans - per-job duties from the report when given.
 * - COOLDOWN (run ends): a smoke-clear phase at run duty, then a
 *   thermal phase at reduced duty (fan airflow measurably cools the
 *   loop) until the upstream temp is back under the resume gate or a
 *   timeout expires; heater off throughout (it would fight the
 *   cooling). A session that never had the armed window open (a
 *   homing motion, a hunt, a dark job) has no smoke to clear: its
 *   smoke phase is zero length, the thermal gate still applies.
 * - TEC: a Pro's chiller, driven only when cool_tec_present is set
 *   (the line has no readback, so presence is the operator's word; a
 *   Basic or a Plus has no part on it). Hysteresis on the upstream
 *   reading, on above cool_tec_on_c and off below cool_tec_off_c, and
 *   only while the fans run (run, smoke clear, thermal, or a forced
 *   cooldown): the cooler's heat sink sits in the airflow path, so no
 *   airflow means no TEC. Off at idle, off in the warm-up hold (it
 *   would fight the heater), off within a degree of the coolant floor
 *   (the settings cross-check keeps the pair above the floor). Turning
 *   off is immediate; turning on again waits a short dwell so sensor
 *   noise cannot chatter the part.
 * - COLD / WARM-UP: the coolant floor (cool_temp_min) is a fire gate
 *   under the ceiling with a degree of hysteresis; the warm-up gate
 *   (cool_temp_start) holds a session that opens under it with the
 *   loop heater on and the fans idle, releases once the bulk reading
 *   (a one-minute rolling minimum upstream, blind to the heater's
 *   slugs passing the sensor) reaches the gate, then
 *   brings the run fans up. Either at its off end (0) is no gate.
 *   A warm-up that stops making progress (the heater's plateau in a
 *   cold room) is named once and keeps holding: no release under the
 *   gate.
 * - OVER-TEMP: if the upstream temp exceeds the run ceiling the
 *   verdict goes OVERTEMP with hold=true and cooling airflow forced;
 *   it recovers (resume_ok) once the temp is back under the resume
 *   gate. The upstream sensor gates because it reads the coolant
 *   entering the tube.
 * - GATE SETTINGS (gates.c): the ceiling, its resume gate and the
 *   other limits are plain settings with a wide legal range, a
 *   recommended band and an off end; a ceiling at its top turns that
 *   gate off by value. An off gate is skipped, still measured, logged
 *   at every run start, and published as gates_off.
 * - AIRFLOW GATES (airflow.c): while the run profile is applied, each
 *   fan is held to a floor (the exhaust, intakes and air assist by
 *   tachometer, the purge fan by current) after a spin-up grace;
 *   three consecutive ticks under the floor trip a fault for the
 *   rest of the run session (verdict AIRFLOW: hold, fire blocked, no
 *   resume), the fans held at run duty. A header's tach window can
 *   raise a floor for a job, never lower it. A machine built with one
 *   intake fan (cool_intake_fans 1, which the setup's airflow check
 *   writes) has nothing on the second intake tach, and that tach is
 *   not judged.
 * - Physical-evidence witnesses (1 Hz, alongside the loop): the
 *   kernel's sampled LASER_ON count is the ground truth of emission
 *   (the gated output of the hardware AND-gate, not a commanded
 *   state). Emission sensed with no armed window in the recent past
 *   stops motion and locks the latch; laser power-good degradation
 *   during an armed window is warned. The four lid IR channels are
 *   polled every tick: their run-start baseline and session peaks are
 *   logged every job (the setup dataset), and through the run,
 *   smoke and thermal phases the four readings sorted ascending (the
 *   quartiles, the factory's statistic) are judged against two tiers
 *   per quartile, the factory's own shape and defaults. A first or
 *   second quartile over its alert threshold for two ticks is the
 *   pause tier (verdict FLAME, hold, fire blocked; released once the
 *   reading is back under for five ticks) - at least three of the four
 *   channels lit well past the lamp. Over its critical threshold is
 *   the fail tier: motion stopped, latch locked, the controller ended
 *   through the supervisor (cool_fail_tier_stop), verdict FIRE with
 *   hold through the session's tail (it ends when the engine is back
 *   at idle or a run session opens, and the operator's acknowledgment
 *   carries it from there), smoke airflow held. Zero is a
 *   tier off; the thresholds sit above a fully lit lid lamp by
 *   construction, so the lamp never trips them - and a candle-sized
 *   flame stays under them too: this catches a developed fire.
 * - Controller silence: if the active controller stops reporting past
 *   REPORT_TIMEOUT_S, fire_ok goes false immediately and the engine
 *   stands down through the normal cooldown path (smoke clear is the
 *   right physical behavior for a job that died mid-cut). Silence with
 *   the armed window open - or with the kernel still playing the ring
 *   (cloud mode preloads whole jobs, so the ring can run for minutes
 *   with no live feeder) - additionally stops motion and locks the
 *   laser latch right here: the supervisor safes controller *deaths*,
 *   this covers controller *hangs*. And while cnc/state still reads
 *   running, exhaust/intake never drop below cooldown duty.
 * - Diagnostics (diag.c) own the hardware while they run: the engine
 *   suspends its writes and publishes fire-blocked until they finish.
 * - The coolant flow watch (flowrun.c, flowwatch.h; opt-in, log only)
 *   borrows the loop heater for its hold and never touches the pump: it
 *   gets this engine's context every tick, and every engine path that
 *   writes the heater (the warm-up, an unreadable sensor, diagnostics, a
 *   quiet hold, shutdown) takes it back first.
 */
#define _GNU_SOURCE
#include "accel.h"
#include "airflow.h"
#include "cool.h"
#include "setup.h"
#include "coolfmt.h"
#include "fflog.h"
#include "flowrun.h"
#include "gates.h"
#include "holds.h"
#include "led.h"
#include "senslog.h"
#include "settings.h"
#include "status.h"
#include "super.h"
#include "testhook.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef GF_SYSFS                /* the host test points these at a fake tree */
#define GF_SYSFS     "/sys/glowforge/"
#endif
/* The test hook GF_SYSFS_ROOT moves the tree in a host-test run
 * (testhook.h), as status.c reads it (it ends with '/'); else GF_SYSFS
 * stands. */
static const char *sysfs_root(void)
{
    const char *r = ff_test_hook("GF_SYSFS_ROOT");
    return r ? r : GF_SYSFS;
}
#ifndef VERDICT_DIR
#define VERDICT_DIR  "/run/forgefirm"
#endif
#define VERDICT_FILE VERDICT_DIR "/cooling.state"
#define VERDICT_TMP  VERDICT_DIR "/.cooling.state.tmp"

/* A controller reports at ~1 Hz; past this silence it is treated as
 * gone (fire blocked, stand-down through cooldown). */
#define REPORT_TIMEOUT_S 5.0

/* Factory fan values (pulse-header ground truth; run duties shared via
 * cool.h). */
#define AIR_ASSIST_IDLE 204
#define AIR_ASSIST_RUN  COOL_AIR_ASSIST_RUN
#define EXHAUST_IDLE    0
#define EXHAUST_RUN     COOL_EXHAUST_RUN
#define INTAKE_IDLE     0
#define INTAKE_RUN      COOL_INTAKE_RUN
/* Thermal-cooldown duty: half the run airflow - enough to keep pulling
 * heat out of the loop without the full-run roar. */
#define EXHAUST_COOL    32768
#define INTAKE_COOL     21639

/* Defaults, all tunable (see the table below). Temperature values are
 * the factory coolant-monitor windows (job-header CM*, millidegrees):
 * run ceiling CMrx=33 C, start/resume gate CMwx=31 C. */
#define COOLDOWN_SMOKE_S       15
#define COOLDOWN_MAX_S         300
#define TEMP_MAX_C_DEFAULT     33.0f
#define TEMP_RESUME_C_DEFAULT  31.0f
#define TEMP_CRITICAL_C_DEFAULT 38.0f
#define TEMP_MIN_C_DEFAULT      5.0f    /* the coolant floor, a fire gate */
#define TEMP_START_C_DEFAULT   16.0f    /* the warm-up gate */
#define TEMP_MIN_HYST_C         1.0f    /* the floor clears this far above itself */
#define WARMUP_STALL_S        300       /* no progress this long: named once */
#define WARMUP_STALL_C          0.2f    /* what counts as progress */
#define WARMUP_WIN            60        /* 1 Hz samples; the bulk, not a slug */
#define WARMUP_HEATER_PCT     40        /* the loop heater's duty while held */
#define WARMUP_HEAT_MAX_S   1800        /* the heater's budget in one warm-up; the hold stays after */
#define WARMUP_DOWN_OFF_C     45.0f     /* downstream at this: the warm-up heater off */
#define WARMUP_DOWN_ON_C      40.0f     /* ...and back on under this */
/* A coolant sensor read that fails (a sysfs error, not a rail value)
 * leaves every coolant gate blind. One failed tick is tolerated: the
 * gates keep their state and the sample is skipped. This many in a row
 * is the SENSOR verdict. */
#define SENSOR_FAIL_TICKS      2
#define TEC_ON_C_DEFAULT       20.0f    /* chosen, not the factory's (unknown on a Pro) */
#define TEC_OFF_C_DEFAULT      18.0f
#define TEC_DWELL_S            30       /* minimum off-to-on spacing */
/* Airflow floors (gates.c carries the rationale and the ranges). */
#define TACH_EXHAUST_MIN_RPM     6400.0f
#define TACH_INTAKE_MIN_RPM      2290.0f
#define TACH_AIR_ASSIST_MIN_RPM  6000.0f
#define PURGE_MIN_CURRENT        300.0f
#define FAN_GRACE_S              15.0f

/* The air-assist fan's return current shares a ground with the coolant
 * thermistors' reference, so both sensors read low by a voltage offset,
 * constant in ADC counts, while the fan runs: measured on the bench
 * about 20 counts (1.2 C near 22 C) at the run duty 1023, nothing below
 * the fan's start (256), and in between in proportion to the fan's
 * current. More counts read colder in this conversion, so the lift reads
 * as a drop. The engine commands that fan, so it knows the duty at every
 * tick and takes the lift off both raw readings before conversion; the
 * same correction goes into /status through
 * cool_coolant_offset_counts(). The value is the machine's own
 * (cool_aa_offset_counts, measured by the aa-offset-calibrate
 * diagnostic); zero, the default, is the factory behavior, which never
 * corrected it. The over-temperature gates are what the correction is
 * for. Bounded so no setting can move the ceiling far. */
#define AA_OFFSET_START      256    /* the fan does not turn below this duty */
#define AA_OFFSET_FULL       1023   /* the duty the setting is measured at */
#define AA_OFFSET_MAX_COUNTS 60.0f

/* Lid IR fire watch: ADC counts of rise above the run-start baseline
 * that read as a fire signal, sustained for FIRE_IR_TICKS consecutive
 * ticks. The shipped defaults arm both tiers (the factory's own
 * quartile thresholds); a tier set to 0 is off and only logs.
 * GFCOOL_FIRE_* override. */
#define FIRE_IR_TICKS      2        /* sustained breach, alert or critical */
#define FIRE_CLEAR_TICKS   5        /* a cleared alert releases after this */

typedef enum {
    Cool_Idle = 0,
    Cool_Run,
    Cool_Smoke,      /* post-job smoke clear at run duty */
    Cool_Thermal,    /* reduced-duty airflow until temp recovers */
    Cool_Warmup,     /* session held under the warm-up gate, heater on */
} cool_state_t;

/* ------------------------------------------------------------- state */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t engine_th;
static int engine_run = 0;

/* Controller report (HTTP thread writes, engine thread reads). */
static int rep_mode = 0;               /* 0 idle, 1 run, 2 cooldown */
static int rep_armed = 0;
static long rep_duty[3] = {-1, -1, -1}; /* air_assist, exhaust, intake */
static cool_limits_t rep_lim = {-1, -1, -1, -1, -1}; /* <= 0 = absent */
static double rep_at = -1.0;           /* CLOCK_MONOTONIC; <0 = never */

/* Published snapshot for cool_status_json. */
static char pub_verdict[COOL_VERDICT_MAX] = "OK";
static char pub_reason[COOL_REASON_MAX];
static int pub_fire_ok, pub_hold;
static int pub_armed_ack;           /* the acknowledgment as published */
static double pub_down = -273.15, pub_up = -273.15;
static const char *pub_phase = "idle";
static char pub_inforce[1024];          /* every tunable as resolved at the last reload */
static double tick_max_ms = NAN;        /* the slowest tick since cool_tick_max_ms read it */

/* Engine-thread internals (no locking needed). */
static cool_state_t cool_state = Cool_Idle;
static int start_cool;              /* the busy-start cooldown airflow still stands */
static uint32_t smoke_s = COOLDOWN_SMOKE_S;
static uint32_t cooldown_max_s = COOLDOWN_MAX_S;
static float temp_offset_c = 0.0f;     /* the per-machine correction (a settings-file value) */
static float temp_max_c = TEMP_MAX_C_DEFAULT;
static float temp_resume_c = TEMP_RESUME_C_DEFAULT;
static float temp_critical_c = TEMP_CRITICAL_C_DEFAULT;
static float temp_min_c = TEMP_MIN_C_DEFAULT;
static float temp_start_c = TEMP_START_C_DEFAULT;
static uint32_t tec_present = 0;
static float tec_on_c = TEC_ON_C_DEFAULT;
static float tec_off_c = TEC_OFF_C_DEFAULT;
static int tec_on_state = 0;        /* what the engine wants driven */
static int tec_written = -1;        /* last value written; -1 = write next */
static double tec_switched_at = 0;
static float tach_exhaust_min_rpm = TACH_EXHAUST_MIN_RPM;
static float tach_intake_min_rpm = TACH_INTAKE_MIN_RPM;
/* The intake fans fitted: 2, or 1 on a machine built with one. Only 1
 * takes the second intake out of the judging; any other value keeps it. */
static uint32_t intake_fans = 2;
static float tach_air_assist_min_rpm = TACH_AIR_ASSIST_MIN_RPM;
static float purge_min_current = PURGE_MIN_CURRENT;
static float fan_grace_s = FAN_GRACE_S;
static int quiet_held = 0;              /* cool_quiet_hold: every fan off for a listening */
static int quiet_pump_held = 0;         /* ...and the pump and the TEC too (the bench tools) */
static double quiet_since = 0;          /* when the quiet hold was taken */
static float aa_offset_counts = 0.0f;   /* cool_aa_offset_counts, at duty 1023 */
static long aa_cmd = 204;               /* the air-assist duty last commanded */
static double phase_until;          /* smoke end / thermal timeout */
static int over_temp_gate = 0;      /* hysteresis: >max sets, <=resume clears */
static int cold_gate = 0;           /* hysteresis: <floor sets, >=floor+hyst clears */
static float warmup_gate_c = 0;     /* the start gate the session holds against */
static float warmup_last_up = 0;    /* progress tracking for the stall warning */
static double warmup_progress_at = 0;
static int warmup_stall_warned = 0;
static double warmup_started_at = 0;    /* the warm-up's start, for the heater's budget */
static int warmup_spent = 0;            /* the budget ran out: heater off, the hold stays */
static int warmup_hot = 0;              /* downstream at the cutoff: heater off until it cools */
/* The release judges the mixed bulk, not the heater's slug passing the
 * sensor: the minimum of the upstream reading over the last WARMUP_WIN
 * seconds (between slugs the reading falls back to the bulk). */
static float warmup_win[WARMUP_WIN];
static uint32_t warmup_win_n = 0;
static int critical_alarm = 0;      /* latched coolant fault, this run session */
static uint32_t sensor_fail_ticks = 0;  /* consecutive ticks with a coolant read failed */
static int sensor_alarm = 0;        /* SENSOR: blind on the coolant, fire blocked, hold */
static long throttle_state = 0;     /* the kernel's CPU cooling state last seen */
static int critical_would_warned = 0;   /* off gate, would have tripped: once */
static int forced_cool = 0;         /* over-temp overrode the phase fans */
static int flood_on = 0;            /* effective run window */
static int silent_warned = 0;
static int silent_safed = 0;        /* hang dead-man fired this episode */

void (*cool_fail_tier_stop)(const char *why) = NULL;

/* Physical-evidence witnesses. */
static float fire_q1_alert = 275.0f;    /* the factory's header defaults */
static float fire_q1_critical = 688.0f;
static float fire_q2_alert = 374.0f;
static float fire_q2_critical = 1022.0f;
static int flame_alert = 0;         /* the pause tier's hold */
static int flame_clear_ticks = 0;
static double last_armed_at = -1.0; /* last fresh armed report seen */
static int emission_warned = 0;
static int pgood_warned = 0;        /* once per run session */
static long last_faults = 0;
static int ir_over_ticks = 0;
static int fire_alarm = 0;          /* latched lid-IR fire signal */
static const char *pub_fire_watch = "watch";

/* The head-accelerometer crash watch (accel.c): the LIS2HH12's own
 * interrupt generators, armed over i2c-dev only while the laser is
 * armed - liveness and gfhome read the accel through st_accel in
 * unarmed sessions, and the watch owns the ODR and the +/-4 g full
 * scale only inside the armed window. Thresholds in IG register
 * units at +/-4 g (LSB ~15.6 mg), the factory's own cut-header
 * values; zero is that tier off, Z is never armed (gravity). */
static float accel_x_alert = 132.0f;
static float accel_y_alert = 112.0f;
static float accel_abort = 133.0f;
static crash_watch_t crash_w;
static int crash_hw_state = 0;      /* 0 unprobed, 1 open, -1 absent */
static int crash_hw_armed = 0;
static int crash_poll_errs = 0;     /* consecutive; 3 stands the watch down */
static const char *pub_accel_watch = "watch";
static long run_duty[3] = {-1, -1, -1};
static long cmd_duty[3] = {-1, -1, -1};   /* what fans_run last wrote */
static int eff_armed = 0;                 /* the armed window, as reported */
static int session_armed = 0;             /* this run session has been armed */
static unsigned long verdict_seq = 0;

/* Gate settings at their off end (gates.c): the coolant ceiling at its
 * top, the critical line at its top, the floor and the warm-up gate at
 * zero. An off gate is skipped, not evaluated, and still measured: the
 * first reading in a run session that would have tripped the shipped
 * default is logged once. */
static int gate_coolant_off = 0;
static int gate_critical_off = 0;
static int gate_floor_off = 0;
static int gate_warmup_off = 0;
static int off_would_warned = 0;
static char pub_gates_off[COOL_GATES_OFF_JSON_MAX] = "[]";

/* Effective limits: the local tunable, tightened by the job's header
 * where the report carries a stricter value (gate_effective). The
 * coolant ceiling is the one with a gate behind it today; the floors
 * are carried, logged and published so the gates that follow find
 * them in place. eff_resume follows a tightened ceiling down by the
 * configured gap. Logged whenever the effective set changes. */
static float eff_temp_max_c = TEMP_MAX_C_DEFAULT;
static float eff_temp_resume_c = TEMP_RESUME_C_DEFAULT;
static float eff_temp_min_c = TEMP_MIN_C_DEFAULT;
static int eff_floor_from_header = 0;
static int eff_from_header = 0;          /* the ceiling is the header's */
static cool_limits_t eff_lim = {-1, -1, -1, -1, -1};
static cool_limits_t last_logged_lim = {-2, -2, -2, -2, -2};
static float last_logged_max = -1.0f;
static double looser_warned_c = -1.0;    /* header ceiling already named */
static char pub_limits[COOL_LIMITS_JSON_MAX] = "{}";

/* The airflow gates (airflow.c): one per fan, evaluated while the run
 * profile is applied, after a grace window from the moment it was
 * written. A trip is a fault for the rest of the run session. */
enum { Fan_Exhaust, Fan_Intake1, Fan_Intake2, Fan_AirAssist, Fan_Purge, Fan_N };
static const char *fan_name[Fan_N] = {"exhaust", "intake_1", "intake_2", "air_assist", "purge"};
static airflow_fan_t fan_gate[Fan_N];
static double fan_reading[Fan_N];
static double fan_floor[Fan_N];
static double fan_grace_until = 0.0;
/* Every gated fan at or above its floor, this tick: the armed window is
 * acknowledged only then, so the first fire waits for the airflow. */
static int fans_up = 0;
static double fans_up_at = -1.0;     /* when they first were, this session */
static int airflow_alarm = 0;        /* latched fan fault, this run session */
static int fan_would_warned = 0;     /* off gate, would have tripped: once */
static char pub_fan_gates[COOL_FAN_GATES_JSON_MAX] = "{}";

static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------- hardware io */

static int wr_attr(const char *attr, const char *val)
{
    char path[128];
    snprintf(path, sizeof(path), "%s%s", sysfs_root(), attr);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int ret = write(fd, val, strlen(val)) < 0 ? -1 : 0;
    close(fd);
    return ret;
}

/* The engine's own safing (stop motion, lock the laser): a write that
 * fails is named at the highest severity, and the callers repeat it
 * while their alarm stands. */
static void safing_write(const char *attr, const char *val)
{
    if (wr_attr(attr, val) != 0)
        fflog(LOG_CRIT, "cool: safing write %s=%s failed: %s", attr, val,
              strerror(errno));
}

static int wr_attr_long(const char *attr, long val)
{
    char v[24];
    snprintf(v, sizeof(v), "%ld", val);
    return wr_attr(attr, v);
}

static long rd_long(const char *attr)
{
    char path[128], buf[24];
    snprintf(path, sizeof(path), "%s%s", sysfs_root(), attr);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return strtol(buf, NULL, 10);
}

static int read_temp(const char *attr, float *c)
{
    long raw = rd_long(attr);
    if (raw < 0)
        return 0;
    /* The air-assist ground shift lifts the raw counts (more counts read
     * colder): take them off before the conversion. */
    long raw_c = raw - cool_coolant_offset_counts();
    /* The per-machine correction from the settings file. */
    *c = (float)coolant_degc(raw_c > 0 ? raw_c : raw) + temp_offset_c;
    return 1;
}

/* cnc/state == "running" is the one state in which the machine can be
 * depositing energy (the ring is being clocked). A read failure counts
 * as not-running here: this feeds extra protective actions, and the
 * fire gate itself fails closed elsewhere. */
static int cnc_is_running(void)
{
    char path[128], buf[16];
    snprintf(path, sizeof(path), "%scnc/state", sysfs_root());
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return strncmp(buf, "running", 7) == 0;
}

static uint32_t heater_pct = 0;     /* the duty last commanded */

static void heater_set_pct(uint32_t pct)
{
    heater_pct = pct;
    wr_attr_long("thermal/heater_pwm", (long)pct * 65535 / 100);
}

/* The three fans the engine drives, every write to them read back. The
 * head's air-assist register and the two thermal PWMs report the duty in
 * force, so a write the bus dropped, or a value the device did not take,
 * is caught here, tried again at once, and named when the tries run out.
 * Without this a lost write left the engine believing its command while
 * the fan sat at the idle duty it had, and the fan was then judged
 * against its floor and held the job as a slow fan. fan_cmd is the duty
 * last commanded on each, what the tick's readback is held to. */
enum { Fx_AirAssist = 0, Fx_Exhaust = 1, Fx_Intake = 2 };
static const char *const fan_attr[3] = {
    "head/air_assist_pwm", "thermal/exhaust_pwm", "thermal/intake_pwm" };
static long fan_cmd[3] = {-1, -1, -1};
#define FAN_WRITE_TRIES 3

static int fan_write(int ix, long duty)
{
    fan_cmd[ix] = duty;
    if (ix == Fx_AirAssist)
        aa_cmd = duty;                  /* the coolant readings' correction
                                         * follows the duty commanded */
    long back = -1;
    for (int t = 1; t <= FAN_WRITE_TRIES; t++) {
        int wr = wr_attr_long(fan_attr[ix], duty);
        back = rd_long(fan_attr[ix]);
        if (wr == 0 && back == duty) {
            if (t > 1)
                fflog(LOG_INFO, "cool: %s took %ld on try %d",
                      fan_attr[ix], duty, t);
            return 0;
        }
    }
    fflog(LOG_WARNING, "cool: %s did not take %ld: it reads %ld after %d "
                       "tries", fan_attr[ix], duty, back, FAN_WRITE_TRIES);
    return -1;
}

static void aa_write(long duty)
{
    fan_write(Fx_AirAssist, duty);
}

/* The duties in force, read back once a tick: a value a device lost
 * since it was written (a reset, a dropped transaction) is put back and
 * named, so a fan is never judged against a duty it was not given. A
 * fan whose duty was raised back gets its spin-up grace again, up to a
 * few times a session: a device that keeps losing its value is judged
 * at the tach like any other, not excused forever. fan_back is what each
 * read this tick, for the fault line. */
#define FAN_LOST_GRACE_MAX 3
static long fan_back[3] = {-1, -1, -1};
static int  fan_lost[3];                /* rewrites this session */

static void fans_verify(double now)
{
    for (int i = 0; i < 3; i++) {
        long back = rd_long(fan_attr[i]);
        fan_back[i] = back;
        if (fan_cmd[i] < 0 || back < 0 || back == fan_cmd[i])
            continue;
        long cmd = fan_cmd[i];
        int ok = fan_write(i, cmd) == 0;
        fan_lost[i]++;
        fflog(LOG_WARNING, "cool: %s read %ld with %ld commanded: %s",
              fan_attr[i], back, cmd,
              ok ? "put back" : "the rewrite did not take");
        if (ok && cmd > back && cool_state == Cool_Run &&
            fan_lost[i] <= FAN_LOST_GRACE_MAX &&
            fan_grace_until < now + (double)fan_grace_s)
            fan_grace_until = now + (double)fan_grace_s;
    }
}

long cool_coolant_offset_counts(void)
{
    float k = aa_offset_counts;
    if (k <= 0.0f || aa_cmd <= AA_OFFSET_START)
        return 0;
    if (k > AA_OFFSET_MAX_COUNTS)
        k = AA_OFFSET_MAX_COUNTS;
    float frac = (float)(aa_cmd - AA_OFFSET_START) / (float)(AA_OFFSET_FULL - AA_OFFSET_START);
    if (frac > 1.0f)
        frac = 1.0f;
    return (long)(k * frac + 0.5f);
}

/* Every fan off: the lens stop finder listens to the head accelerometer
 * and a running fan is in the reading. */
static void fans_quiet(void)
{
    aa_write(0);
    fan_write(Fx_Exhaust, 0);
    fan_write(Fx_Intake, 0);
    wr_attr("head/purge_air", "0");
}

/* The pump and the TEC off as well, so the machine is silent: the bench's
 * accelerometer tools ask for it, and the operator listens too. Idle
 * only, the laser latched, so the pump-off is dry; the tick leaves the
 * TEC alone while this stands. The finder keeps the pump running: it was
 * proven that way. */
static void pump_quiet(void)
{
    wr_attr("thermal/water_pump_on", "0");
    wr_attr("thermal/tec_on", "0");
}

static void fans_idle(void)
{
    if (quiet_held) {
        fans_quiet();
        start_cool = 0;
        return;
    }
    aa_write(AIR_ASSIST_IDLE);
    fan_write(Fx_Exhaust, EXHAUST_IDLE);
    fan_write(Fx_Intake, INTAKE_IDLE);
    start_cool = 0;
}

/* Run duties: the per-job report profile when given, factory values
 * otherwise. */
/* The run profile: the configured duties, or the job's own where it
 * gave one, which may only raise a fan while the laser is armed
 * (airflow_run_duty). */
/* What the fans were told to do, and what decided it: the machine's run
 * duty, the job's own where its report gave one, and the armed window,
 * which puts the machine's back when a job asked for less. A fan judged
 * against its floor while it sits at a duty that cannot reach that floor
 * is what an unexplainable airflow hold looks like from the readings
 * alone, and the readings are all the log kept. Written when the answer
 * changes, not on every pass. */
static void fans_log_duty(void)
{
    static long was[3] = {-2, -2, -2};
    static int was_quiet = -1, was_armed = -1;
    if (cmd_duty[0] == was[0] && cmd_duty[1] == was[1] && cmd_duty[2] == was[2] &&
        quiet_held == was_quiet && eff_armed == was_armed)
        return;
    was[0] = cmd_duty[0];
    was[1] = cmd_duty[1];
    was[2] = cmd_duty[2];
    was_quiet = quiet_held;
    was_armed = eff_armed;
    fflog(LOG_INFO, "cool: fan duty: air assist %ld (machine %d, job %ld), exhaust %ld "
          "(machine %d, job %ld), intake %ld (machine %d, job %ld); armed %d%s",
          cmd_duty[0], AIR_ASSIST_RUN, run_duty[0],
          cmd_duty[1], EXHAUST_RUN, run_duty[1],
          cmd_duty[2], INTAKE_RUN, run_duty[2], eff_armed,
          quiet_held ? "; the quiet hold stands, the fans are held off instead" : "");
}

static void fans_run(void)
{
    cmd_duty[0] = airflow_run_duty(AIR_ASSIST_RUN, run_duty[0], eff_armed);
    cmd_duty[1] = airflow_run_duty(EXHAUST_RUN, run_duty[1], eff_armed);
    cmd_duty[2] = airflow_run_duty(INTAKE_RUN, run_duty[2], eff_armed);
    fans_log_duty();
    if (quiet_held) {
        fans_quiet();
        return;
    }
    aa_write(cmd_duty[0]);
    fan_write(Fx_Exhaust, cmd_duty[1]);
    fan_write(Fx_Intake, cmd_duty[2]);
}

static void fans_cool(void)
{
    if (quiet_held) {
        fans_quiet();
        return;
    }
    aa_write(AIR_ASSIST_IDLE);
    fan_write(Fx_Exhaust, EXHAUST_COOL);
    fan_write(Fx_Intake, INTAKE_COOL);
}

/* Reapply the fan profile the current phase calls for (used when an
 * over-temp intervention forced cooling airflow and then stood down). */
static void fans_apply_phase(void)
{
    switch (cool_state) {
    case Cool_Run:
    case Cool_Smoke:
        fans_run();
        break;
    case Cool_Thermal:
        fans_cool();
        break;
    default:
        fans_idle();
        break;
    }
}

/* ----------------------------------------------------------- logging */

static void warn(const char *msg)
{
    pthread_mutex_lock(&mu);
    snprintf(pub_reason, sizeof(pub_reason), "%s", msg);
    pthread_mutex_unlock(&mu);
    fflog(LOG_WARNING, "cool: %s", msg);
}

static float last_up_c = 0;         /* the upstream reading the hold was taken on */

/* The ceiling's hold, named: logged on the rising edge, and published
 * again (no new log line, it is the same hold) when a critical fault
 * that had overwritten the reason clears. */
static void overtemp_warn(int log)
{
    char msg[96];
    snprintf(msg, sizeof(msg),
             "coolant %.1f C over %.0f C limit%s - hold until %.0f C",
             last_up_c, eff_temp_max_c, eff_from_header ? " (the job's)" : "",
             eff_temp_resume_c);
    if (log) {
        warn(msg);
        return;
    }
    pthread_mutex_lock(&mu);
    snprintf(pub_reason, sizeof(pub_reason), "%s", msg);
    pthread_mutex_unlock(&mu);
}

static void info(const char *msg)
{
    fflog(LOG_INFO, "cool: %s", msg);
}

/* ------------------------------------------------- an extension's hold */

/* A package's hold withholds fire and never permits it: it joins the pause
 * tier below every verdict of the engine's own, so it can neither hide one
 * nor outrank one. The extension host keeps the hold files (holds.h). The
 * engine looks at them only while extensions are on and the machine is not
 * in safe mode: turning extensions off, or the safe-mode file, ends every
 * hold at the next tick, whatever has become of the host. */
#define EXT_SAFE_FILE   "/run/forgefirm/ext-safe"

static const char *holds_dir = HOLDS_DIR_DEFAULT;
static const char *holds_required_dir = HOLDS_REQUIRED_DIR;
static const char *ext_safe_file = EXT_SAFE_FILE;
static int ext_held;                    /* a hold stood at the last tick */
static int ext_dropped;                 /* stale advisory holds were dropped at the last tick */
static char ext_reason[COOL_REASON_MAX];

static void ext_holds_read(double now, holds_t *h)
{
    char v[8];
    memset(h, 0, sizeof(*h));
    if (access(ext_safe_file, F_OK) == 0 || settings_get("ext_enabled", v, sizeof(v)) != 0 || strcmp(v, "1") != 0)
        return;
    holds_read(holds_dir, holds_required_dir, now, h);
}

/* Named on its edges, and when the hold that stands changes. */
static void ext_holds_log(const holds_t *h)
{
    char msg[COOL_REASON_MAX + 48];
    if (h->standing && (!ext_held || strcmp(h->reason, ext_reason) != 0)) {
        snprintf(msg, sizeof(msg), "extension hold: %s", h->reason);
        fflog(LOG_WARNING, "cool: %s", msg);
    } else if (!h->standing && ext_held) {
        info("extension hold cleared");
        pthread_mutex_lock(&mu);
        if (!strcmp(pub_reason, ext_reason))
            pub_reason[0] = '\0';          /* the hold's words go with the hold */
        pthread_mutex_unlock(&mu);
    }
    if (h->stale_advisory && !ext_dropped)
        fflog(LOG_WARNING, "cool: %d advisory extension hold(s) dropped: the extension host is not answering",
              h->stale_advisory);
    ext_held = h->standing;
    ext_dropped = h->stale_advisory > 0;
    snprintf(ext_reason, sizeof(ext_reason), "%s", h->standing ? h->reason : "");
}

/* ---------------------------------------------------------- tunables */

/* Tunables resolve env > settings > compiled default. The GFCOOL_* env
 * vars are the bench-debug path and win for the process lifetime; the
 * cool_* keys in the shared machine settings are the user-facing store
 * (Machine tab) and are re-read at every run start, so GUI changes
 * apply from the next job with no restart. */
static struct {
    const char *env, *key;
    float def;
    float *f;                       /* exactly one of f/u is set */
    uint32_t *u;
    int env_set;
} tunables[] = {
    { "GFCOOL_TEMP_OFFSET_C",   "cool_temp_offset_c",
      0.0f,                   &temp_offset_c,   NULL, 0 },
    { "GFCOOL_COOLDOWN_S",      "cool_cooldown_s",
      COOLDOWN_SMOKE_S,       NULL,             &smoke_s, 0 },
    { "GFCOOL_COOLDOWN_MAX_S",  "cool_cooldown_max_s",
      COOLDOWN_MAX_S,         NULL,             &cooldown_max_s, 0 },
    { "GFCOOL_TEMP_MAX",        "cool_temp_max",
      TEMP_MAX_C_DEFAULT,     &temp_max_c,      NULL, 0 },
    { "GFCOOL_TEMP_RESUME",     "cool_temp_resume",
      TEMP_RESUME_C_DEFAULT,  &temp_resume_c,   NULL, 0 },
    { "GFCOOL_TEMP_CRITICAL",   "cool_temp_critical_c",
      TEMP_CRITICAL_C_DEFAULT, &temp_critical_c, NULL, 0 },
    { "GFCOOL_TEMP_MIN",        "cool_temp_min",
      TEMP_MIN_C_DEFAULT,     &temp_min_c,      NULL, 0 },
    { "GFCOOL_TEMP_START",      "cool_temp_start",
      TEMP_START_C_DEFAULT,   &temp_start_c,    NULL, 0 },
    { "GFCOOL_TEC_PRESENT",     "cool_tec_present",
      0.0f,                   NULL,             &tec_present, 0 },
    { "GFCOOL_TEC_ON",          "cool_tec_on_c",
      TEC_ON_C_DEFAULT,       &tec_on_c,        NULL, 0 },
    { "GFCOOL_TEC_OFF",         "cool_tec_off_c",
      TEC_OFF_C_DEFAULT,      &tec_off_c,       NULL, 0 },
    { "GFCOOL_AA_OFFSET_COUNTS", "cool_aa_offset_counts",
      0.0f,                   &aa_offset_counts, NULL, 0 },
    { "GFCOOL_FIRE_Q1_ALERT",   "cool_fire_q1_alert",
      275.0f,                 &fire_q1_alert,   NULL, 0 },
    { "GFCOOL_FIRE_Q1_CRIT",    "cool_fire_q1_critical",
      688.0f,                 &fire_q1_critical, NULL, 0 },
    { "GFCOOL_FIRE_Q2_ALERT",   "cool_fire_q2_alert",
      374.0f,                 &fire_q2_alert,   NULL, 0 },
    { "GFCOOL_FIRE_Q2_CRIT",    "cool_fire_q2_critical",
      1022.0f,                &fire_q2_critical, NULL, 0 },
    { "GFCOOL_ACCEL_X_ALERT",   "cool_accel_x_alert",
      132.0f,                 &accel_x_alert,   NULL, 0 },
    { "GFCOOL_ACCEL_Y_ALERT",   "cool_accel_y_alert",
      112.0f,                 &accel_y_alert,   NULL, 0 },
    { "GFCOOL_ACCEL_ABORT",     "cool_accel_abort",
      133.0f,                 &accel_abort,     NULL, 0 },
    { "GFCOOL_TACH_EXHAUST_MIN", "cool_tach_exhaust_min_rpm",
      TACH_EXHAUST_MIN_RPM,   &tach_exhaust_min_rpm, NULL, 0 },
    { "GFCOOL_TACH_INTAKE_MIN",  "cool_tach_intake_min_rpm",
      TACH_INTAKE_MIN_RPM,    &tach_intake_min_rpm, NULL, 0 },
    { "GFCOOL_INTAKE_FANS",      "cool_intake_fans",
      2.0f,                   NULL,             &intake_fans, 0 },
    { "GFCOOL_TACH_AIR_MIN",    "cool_tach_air_assist_min_rpm",
      TACH_AIR_ASSIST_MIN_RPM, &tach_air_assist_min_rpm, NULL, 0 },
    { "GFCOOL_PURGE_MIN",        "cool_purge_min_current",
      PURGE_MIN_CURRENT,      &purge_min_current, NULL, 0 },
    { "GFCOOL_FAN_GRACE_S",      "cool_fan_grace_s",
      FAN_GRACE_S,            &fan_grace_s,     NULL, 0 },
};

/* The engine's resolved value for a gate setting (env > settings >
 * default), for the gates table. */
static double engine_gate_value(const gate_setting_t *g, void *ctx)
{
    (void)ctx;
    if (!strcmp(g->key, "cool_temp_max"))
        return temp_max_c;
    if (!strcmp(g->key, "cool_temp_resume"))
        return temp_resume_c;
    if (!strcmp(g->key, "cool_temp_critical_c"))
        return temp_critical_c;
    if (!strcmp(g->key, "cool_temp_min"))
        return temp_min_c;
    if (!strcmp(g->key, "cool_temp_start"))
        return temp_start_c;
    if (!strcmp(g->key, "cool_tec_on_c"))
        return tec_on_c;
    if (!strcmp(g->key, "cool_tec_off_c"))
        return tec_off_c;
    if (!strcmp(g->key, "cool_fire_q1_alert"))
        return fire_q1_alert;
    if (!strcmp(g->key, "cool_fire_q1_critical"))
        return fire_q1_critical;
    if (!strcmp(g->key, "cool_fire_q2_alert"))
        return fire_q2_alert;
    if (!strcmp(g->key, "cool_fire_q2_critical"))
        return fire_q2_critical;
    if (!strcmp(g->key, "cool_accel_x_alert"))
        return accel_x_alert;
    if (!strcmp(g->key, "cool_accel_y_alert"))
        return accel_y_alert;
    if (!strcmp(g->key, "cool_accel_abort"))
        return accel_abort;
    if (!strcmp(g->key, "cool_tach_exhaust_min_rpm"))
        return tach_exhaust_min_rpm;
    if (!strcmp(g->key, "cool_tach_intake_min_rpm"))
        return tach_intake_min_rpm;
    if (!strcmp(g->key, "cool_tach_air_assist_min_rpm"))
        return tach_air_assist_min_rpm;
    if (!strcmp(g->key, "cool_purge_min_current"))
        return purge_min_current;
    if (!strcmp(g->key, "cool_fan_grace_s"))
        return fan_grace_s;
    return g->def;
}

/* Classify every gate setting from the resolved tunables, publish the
 * gates that are off, and say so in the log: one line per setting,
 * at warning level when it is off or outside its band, so a machine
 * running without a gate says so at every run start. */
static void gates_apply(void)
{
    size_t n;
    const gate_setting_t *t = gate_settings(&n);
    /* The values in force go out as one line, not one datagram per key:
     * a burst of twenty overran the log socket's queue at every arm and
     * took the lines around it with it. */
    char inforce[768];
    int off = 0;
    for (size_t i = 0; i < n; i++) {
        double v = engine_gate_value(&t[i], NULL);
        gate_state_t st = gate_state(&t[i], v);
        if (t[i].gate && !strcmp(t[i].gate, "coolant_max"))
            gate_coolant_off = st == Gate_Off;
        if (t[i].gate && !strcmp(t[i].gate, "coolant_critical"))
            gate_critical_off = st == Gate_Off;
        if (t[i].gate && !strcmp(t[i].gate, "coolant_min"))
            gate_floor_off = st == Gate_Off;
        if (t[i].gate && !strcmp(t[i].gate, "warm_up"))
            gate_warmup_off = st == Gate_Off;
        if (st == Gate_Off)
            fflog(LOG_WARNING, "cool: gate %s OFF: %s = %g (the %s end of "
                  "%g to %g; recommended %g to %g, default %g)",
                  t[i].gate, t[i].key, v,
                  t[i].off_end < 0 ? "low" : "high", t[i].lo, t[i].hi,
                  t[i].band_lo, t[i].band_hi, t[i].def);
        else if (st == Gate_Warn)
            fflog(LOG_WARNING, "cool: %s = %g is outside the recommended "
                  "%g to %g (default %g)", t[i].key, v,
                  t[i].band_lo, t[i].band_hi, t[i].def);
        else if (off >= 0 && off < (int)sizeof(inforce))
            off += snprintf(inforce + off, sizeof(inforce) - (size_t)off,
                            "%s%s=%g", off ? " " : "", t[i].key, v);
    }
    if (off > 0)
        fflog(LOG_INFO, "cool: settings in force: %s", inforce);
    char off_json[sizeof(pub_gates_off)];
    if (gates_off_json(off_json, sizeof(off_json), engine_gate_value, NULL) < 0)
        snprintf(off_json, sizeof(off_json), "[]");
    pthread_mutex_lock(&mu);
    snprintf(pub_gates_off, sizeof(pub_gates_off), "%s", off_json);
    pthread_mutex_unlock(&mu);
}

/* Resolve the effective limits from the local tunables and the
 * latest fresh report's header limits (absent when the report carried
 * none, or is stale). The coolant ceiling is the one consumer; a
 * header ceiling applies only if the local gate is on (an operator
 * who turned the gate off at its far end is not overruled by a job)
 * and stricter than the local value, and the resume gate follows it
 * down by the configured gap. The floors are resolved the same way
 * and published for the gates that follow. Logged on every change of
 * the effective set, and a looser header value is named once per run
 * session. */
static void limits_apply(const cool_limits_t *hdr, int fresh)
{
    cool_limits_t none = {-1, -1, -1, -1, -1};
    const cool_limits_t *h = fresh && hdr ? hdr : &none;
    int from = 0;
    float max_c = temp_max_c, resume_c = temp_resume_c;
    if (!gate_coolant_off) {
        max_c = (float)gate_effective(temp_max_c, h->coolant_max_c, 0, &from);
        if (from)
            resume_c = max_c - (temp_max_c - temp_resume_c);
    }
    cool_limits_t eff;
    eff.coolant_max_c = max_c;
    /* The floor: the job's header floor can only raise the local one;
     * a local floor at its off end is no floor and no header raises it. */
    int from_floor = 0;
    eff.coolant_min_c = gate_floor_off ? 0.0
        : gate_effective(temp_min_c, h->coolant_min_c, 1, &from_floor);
    /* A floor the operator set to zero is off and stays off; otherwise
     * a header floor can only raise it. */
    eff.exhaust_min_rpm = tach_exhaust_min_rpm <= 0.0f ? 0.0
        : gate_effective(tach_exhaust_min_rpm, h->exhaust_min_rpm, 1, NULL);
    eff.intake_min_rpm = tach_intake_min_rpm <= 0.0f ? 0.0
        : gate_effective(tach_intake_min_rpm, h->intake_min_rpm, 1, NULL);
    eff.air_assist_min_rpm = tach_air_assist_min_rpm <= 0.0f ? 0.0
        : gate_effective(tach_air_assist_min_rpm, h->air_assist_min_rpm, 1, NULL);

    int changed = max_c != last_logged_max ||
                  memcmp(&eff, &last_logged_lim, sizeof(eff)) != 0 ||
                  eff_from_header != from || eff_floor_from_header != from_floor;
    eff_temp_max_c = max_c;
    eff_temp_resume_c = resume_c;
    eff_from_header = from;
    eff_temp_min_c = (float)eff.coolant_min_c;
    eff_floor_from_header = from_floor;
    eff_lim = eff;
    if (changed) {
        last_logged_max = max_c;
        last_logged_lim = eff;
        fflog(LOG_INFO, "cool: effective limits: coolant ceiling %.1f C "
              "(local %.1f, header %s%.1f) resume %.1f C; floors coolant "
              "%.1f C %s, exhaust %.0f rpm, intake %.0f rpm, air "
              "assist %.0f rpm",
              max_c, temp_max_c, h->coolant_max_c > 0 ? "" : "none ",
              h->coolant_max_c > 0 ? h->coolant_max_c : 0.0, resume_c,
              eff.coolant_min_c,
              gate_floor_off ? "(off)" : from_floor ? "(the job's)" : "(local)",
              eff.exhaust_min_rpm, eff.intake_min_rpm,
              eff.air_assist_min_rpm);
        char lim[sizeof(pub_limits)];
        if (coolfmt_limits(lim, sizeof(lim), max_c, resume_c, temp_critical_c,
                           from, &eff) < 0)
            fflog(LOG_ERR, "cool: the effective limits do not fit their "
                  "status fragment; publishing none");
        pthread_mutex_lock(&mu);
        snprintf(pub_limits, sizeof(pub_limits), "%s", lim);
        pthread_mutex_unlock(&mu);
    }
    /* A header ceiling that does not tighten is named once per value:
     * the limits arrive with the job load, before the run session, and
     * leave with the job, so the value itself is the session. */
    if (h->coolant_max_c <= 0)
        looser_warned_c = -1.0;
    if (!gate_coolant_off && h->coolant_max_c > 0 && !from &&
        h->coolant_max_c != looser_warned_c && h->coolant_max_c >= temp_max_c) {
        looser_warned_c = h->coolant_max_c;
        fflog(LOG_INFO, "cool: header coolant ceiling %.1f C is not stricter "
              "than the local %.1f C; the local one stands",
              h->coolant_max_c, temp_max_c);
    }
}

static void conf_reload(void)
{
    for (size_t i = 0; i < sizeof(tunables) / sizeof(tunables[0]); i++) {
        if (tunables[i].env_set)
            continue;
        char v[32];
        char *end;
        float f = tunables[i].def;
        if (settings_get(tunables[i].key, v, sizeof(v)) == 0) {
            float parsed = strtof(v, &end);
            /* "nan" and "inf" parse as numbers. Every comparison against a
             * NaN is false, so a limit set to one never trips and never
             * reads as off: the default stands instead, and the line says
             * so at every run start. */
            if (end != v && *end == '\0' && !isfinite(parsed))
                fflog(LOG_WARNING, "cool: %s = %s is not a finite number; the default %g stands",
                      tunables[i].key, v, (double)tunables[i].def);
            else if (end != v && *end == '\0')
                f = parsed;
        }
        if (tunables[i].f)
            *tunables[i].f = f;
        else
            *tunables[i].u = f < 0.0f ? 0 : (uint32_t)f;
    }
    /* Every tunable as resolved, for the sensors logger's settings record. */
    char inforce[sizeof(pub_inforce)];
    size_t o = 0;
    inforce[0] = '\0';
    for (size_t i = 0; i < sizeof(tunables) / sizeof(tunables[0]); i++) {
        int n = tunables[i].f
              ? snprintf(inforce + o, sizeof(inforce) - o, "%s%s=%g", o ? " " : "",
                         tunables[i].key, (double)*tunables[i].f)
              : snprintf(inforce + o, sizeof(inforce) - o, "%s%s=%u", o ? " " : "",
                         tunables[i].key, (unsigned)*tunables[i].u);
        if (n < 0 || (size_t)n >= sizeof(inforce) - o)
            break;
        o += (size_t)n;
    }
    inforce[o] = '\0';
    pthread_mutex_lock(&mu);
    snprintf(pub_inforce, sizeof(pub_inforce), "%s", inforce);
    pthread_mutex_unlock(&mu);
    gates_apply();
    /* Named at every run start, as an off gate is. */
    if (intake_fans == 1)
        fflog(LOG_INFO, "cool: one intake fan fitted (cool_intake_fans 1): "
              "intake_2 is not judged");
}

/* ---------------------------------------------- diagnostic ownership */

/* The diagnostic tools own the thermal hardware between
 * cool_diag_take and cool_diag_release: the tick publishes phase
 * "diag" with fire blocked and touches nothing, the tools write only
 * through the guarded helpers, and the release reasserts the idle
 * posture in one place. Take succeeds only from an idle engine (the
 * HTTP layer additionally gates on an idle machine), so a takeover
 * never lands inside a run session or its cooldown. */
static int diag_owned = 0;
static int tick_busy = 0;           /* the engine thread is inside a tick (under mu) */

int cool_diag_take(void)
{
    pthread_mutex_lock(&mu);
    if (diag_owned || cool_state != Cool_Idle || strcmp(pub_phase, "idle")) {
        pthread_mutex_unlock(&mu);
        return -1;
    }
    diag_owned = 1;
    /* A tick in flight may still write the fans it decided on before
     * the take: wait it out (bounded) so nothing of it lands after the
     * tool's own writes. */
    for (int i = 0; i < 200 && tick_busy; i++) {
        pthread_mutex_unlock(&mu);
        usleep(10 * 1000);
        pthread_mutex_lock(&mu);
    }
    pthread_mutex_unlock(&mu);
    flowrun_yield("diagnostics");
    warn("diagnostics own the thermal hardware");
    return 0;
}

void cool_diag_release(void)
{
    heater_set_pct(0);
    wr_attr("thermal/water_pump_on", "1");
    fans_idle();
    tec_written = -1;               /* the tick rewrites the TEC's state */
    pthread_mutex_lock(&mu);
    diag_owned = 0;
    pub_reason[0] = '\0';
    pthread_mutex_unlock(&mu);
    fflog(LOG_INFO, "cool: diagnostics handed the thermal hardware back");
}

static int diag_guard(const char *what)
{
    pthread_mutex_lock(&mu);
    int own = diag_owned;
    pthread_mutex_unlock(&mu);
    if (!own)
        fflog(LOG_ERR, "cool: %s refused - diagnostics do not own the "
              "thermal hardware", what);
    return own ? 0 : -1;
}

void cool_quiet_hold_ex(int on, int with_pump)
{
    pthread_mutex_lock(&mu);
    int was = quiet_held, was_pump = quiet_pump_held;
    quiet_held = on ? 1 : 0;
    quiet_pump_held = (on && with_pump) ? 1 : 0;
    if (on)
        quiet_since = wall_s();
    pthread_mutex_unlock(&mu);
    /* After the hold is published: a tick between the two then carries it,
     * so nothing lets the watch take the heater back before the pump stops. */
    if (on)
        flowrun_yield("a quiet hold");
    if (on) {
        if (!was) {
            fans_quiet();
            info("every fan off for a listening");
        }
        if (with_pump && !was_pump) {
            pump_quiet();
            info("the pump and the TEC off too: the machine silent");
        }
    } else if (was) {
        if (was_pump) {
            wr_attr("thermal/water_pump_on", "1");
            tec_written = -1;           /* the tick rewrites the TEC's state */
        }
        wr_attr("head/purge_air", "1");
        fans_apply_phase();
        info(was_pump ? "the fans and the pump back in their posture after the listening"
                      : "the fans back in their posture after the listening");
    }
}

void cool_quiet_hold(int on)
{
    cool_quiet_hold_ex(on, 0);
}

int cool_quiet_held(void)
{
    pthread_mutex_lock(&mu);
    int held = quiet_held;
    pthread_mutex_unlock(&mu);
    return held;
}

/* The pump only from an idle engine: never under a warm-up (its heater
 * would run on a still loop), a run session, a smoke or thermal phase, or
 * a diagnostic. */
int cool_quiet_pump_ok(void)
{
    pthread_mutex_lock(&mu);
    int ok = !diag_owned && !strcmp(pub_phase, "idle");
    pthread_mutex_unlock(&mu);
    return ok;
}

int cool_quiet_pump_held(void)
{
    pthread_mutex_lock(&mu);
    int held = quiet_pump_held;
    pthread_mutex_unlock(&mu);
    return held;
}

void cool_diag_pump(int on)
{
    if (diag_guard("diag pump write") == 0)
        wr_attr("thermal/water_pump_on", on ? "1" : "0");
}

/* The kernel holds a heater duty above zero only for its lease
 * (heater_lease_ms): a diagnostic that wants heat writes it again inside
 * the lease, here. The engine's tick does not write it for the diagnostic,
 * so one that hangs loses its heat with the lease. */
void cool_diag_heater_pct(double pct)
{
    if (diag_guard("diag heater write") == 0)
        heater_set_pct(pct < 0 ? 0 : (uint32_t)pct);
}

/* The whole run posture, not a part of it. Every fan the engine drives at
 * a run, and purge air, which the engine holds on continuously: a
 * diagnostic that measures the fans has to measure them where a job
 * would find them. Air assist was left out, so the airflow check read it
 * at its idle duty for the whole window, called that its steady speed at
 * the run profile, and wrote a floor from it - a floor no job could then
 * meet, because a job does raise the fan. Purge is asserted for the same
 * reason: it is part of the posture, and something that switched it off
 * would otherwise stay off through a check that judges it. */
void cool_diag_fans_run(void)
{
    if (diag_guard("diag fan write") == 0) {
        fan_write(Fx_Exhaust, EXHAUST_RUN);
        fan_write(Fx_Intake, INTAKE_RUN);
        aa_write(AIR_ASSIST_RUN);
        wr_attr("head/purge_air", "1");
    }
}

/* Stand the machine back down to the idle posture a diagnostic borrowed
 * it from. Purge air is part of that posture: the engine holds it on
 * continuously, the factory does the same, and the idle phase never
 * re-applies its own duties, so a diagnostic that switched it off (the
 * airflow check reads its off current) would leave it off until the
 * daemon restarted. The next job then judged it against the floor that
 * same check had just written and was held mid-cut. */
void cool_diag_fans_idle(void)
{
    if (diag_guard("diag fan write") == 0) {
        fan_write(Fx_Exhaust, EXHAUST_IDLE);
        fan_write(Fx_Intake, INTAKE_IDLE);
        aa_write(AIR_ASSIST_IDLE);
        wr_attr("head/purge_air", "1");
    }
}

void cool_diag_aa(long duty)
{
    if (diag_guard("diag air-assist write") == 0)
        aa_write(duty);
}

void cool_diag_purge(int on)
{
    if (diag_guard("diag purge write") == 0)
        wr_attr("head/purge_air", on ? "1" : "0");
}

/* Engine-raised setup flags: a fan judged within 10 percent of its
 * floor at any tick of a run session, past the spin-up grace,
 * recommends the airflow check, once per fan per session. */
static int fan_margin_noted[Fan_N];

/* ------------------------------------------------------ verdict file */

/* The document must always fit its buffer. An oversized one is not
 * published at all, and no verdict reads to every controller as a
 * fault: fire blocked and the job held. Budget: the keys, quotes and
 * separators; the five booleans; the numbers (seq, ts_mono and the two
 * temperatures); and the two bounded strings. */
#define VERDICT_PUNCTUATION 116
#define VERDICT_BOOLEANS     25
#define VERDICT_NUMBERS      64
#define VERDICT_BODY_MAX    384
_Static_assert(VERDICT_PUNCTUATION + VERDICT_BOOLEANS + VERDICT_NUMBERS +
               COOL_VERDICT_MAX + COOL_REASON_MAX <= VERDICT_BODY_MAX,
               "the cooling verdict must always fit its buffer: an "
               "unpublished verdict blocks fire and holds the job");

/* Atomic publish: write-temp + rename, ~1 Hz and on every change (the
 * loop runs at 1 Hz, so every iteration publishes). Readers treat a
 * missing file or ts_mono older than ~2 s as fire_ok=false, hold=true.
 *
 * "armed" is this engine's own view of the armed window, and it is what
 * makes a verdict answer the session it is read against. The controller
 * opens its armed window before this engine has seen the report, so
 * without it the permissive idle verdict - fire_ok=true, nothing wrong
 * at idle - stays fresh across the arm and authorizes the first fire
 * before the run airflow is applied. Published from eff_armed, which
 * the tick sets before flood_apply and before this call, so a verdict
 * carrying armed=true was computed with the run session open, and only
 * once every gated fan reads at or above its floor (fans_up): the
 * controller waits on this acknowledgment, so the first fire waits for
 * the airflow instead of the fans catching up under the beam.
 *
 * "ack_wait" says a failure waits for the operator's acknowledgment
 * (attention_tick), whatever the verdict's name: the name is ACK only
 * where no other verdict stands, and a controller that must not ask for a
 * press it would then refuse reads this instead. It is read from the
 * attention's own state here, so every document carries it. fire_ok is
 * false whenever it is true. */
static int attn_ack_waits(void);

static void verdict_publish(int fire_ok, const char *verdict, int hold,
                            int have_down, float down,
                            int have_up, float up)
{
    char body[VERDICT_BODY_MAX];
    int armed_ack = eff_armed && fans_up;
    pthread_mutex_lock(&mu);
    int ack_wait = attn_ack_waits();
    snprintf(pub_verdict, sizeof(pub_verdict), "%s", verdict);
    pub_fire_ok = fire_ok;
    pub_hold = hold;
    pub_armed_ack = armed_ack;
    pub_down = have_down ? down : -273.15;
    pub_up = have_up ? up : -273.15;
    /* To the millisecond, rounded down: a reader on this clock never
     * sees a stamp from its future. Both readers refuse one, and the
     * cloud client reads with no cache, so a refused read is a lost
     * verdict there. */
    double stamp = floor(wall_s() * 1000.0) / 1000.0;
    int n = snprintf(body, sizeof(body),
        "{\"seq\":%lu,\"ts_mono\":%.3f,\"fire_ok\":%s,"
        "\"verdict\":\"%s\",\"hold\":%s,\"resume_ok\":%s,\"armed\":%s,"
        "\"ack_wait\":%s,"
        "\"reason\":\"%s\",\"down_c\":%.2f,\"up_c\":%.2f}\n",
        ++verdict_seq, stamp, fire_ok ? "true" : "false",
        verdict, hold ? "true" : "false", hold ? "false" : "true",
        armed_ack ? "true" : "false", ack_wait ? "true" : "false",
        pub_reason, pub_down, pub_up);
    pthread_mutex_unlock(&mu);
    /* snprintf reports the untruncated length; never publish more than
     * the buffer holds, and never a document whose closing brace was
     * cut off (the client refuses an incomplete verdict). */
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(body)) {
        fflog(LOG_ERR, "cool: verdict too long (%d bytes), "
                       "not published", n);
        return;
    }

    int fd = open(VERDICT_TMP, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    int ok = write(fd, body, (size_t)n) == n;
    close(fd);
    if (ok)
        rename(VERDICT_TMP, VERDICT_FILE);
}

/* ------------------------------------------------------------ engine */

/* Enter/leave the effective run window: reported mode "run" OR armed -
 * fire must never happen without the cut airflow, which only lives
 * inside a run session. */
/* The operator's attention. Whatever blocks firing and needs the operator
 * flashes the button red, and the panel says why: FIRE, CRASH, FLAME, BUMP,
 * AIRFLOW, CRITICAL, SENSOR, and a failed motion check. Holds that clear on
 * their own (the warm-up, a cold or a hot loop, an extension's hold) do not:
 * they are the machine's own to wait out.
 *
 * Red always means firing is blocked. A failure of the fail tier (FIRE,
 * CRASH, AIRFLOW, CRITICAL), or one that still stands when its session ends,
 * ends with the session as far as its own verdict goes, so it stays up as
 * the verdict ACK, which blocks fire and holds any armed window until the
 * operator has acknowledged it on the panel. Motion is free. A pause tier's
 * cause that goes away under a job still held needs no acknowledgment: the
 * job's resume is the operator's (a press, or the window the operator armed
 * still open). Nothing here survives a restart.
 *
 * The flash stands over any pattern a controller writes: it is checked every
 * tick and put back, never rewritten while it stands, which would restart its
 * period. When nothing wants it any more the engine puts out only its own:
 * a button that no longer reads as the flash was written by another owner
 * since the last tick, and is left as that owner set it. A pattern the
 * flash covered comes back from its owner (led_keep).
 *
 * A fault that clears on its own still gets said: a notice with its type and
 * its times stands on the panel until it is dismissed. It blocks nothing and
 * lights nothing. Repeats of the last one are counted, not stacked. */
static int attn_latched = 0;
static int attn_live = 0;
static int attn_sticky = 0;             /* it takes the acknowledgment to end it */
static int attn_was_armed = 0;
static int attn_was_session = 0;        /* the engine was in a run session or its tail */
static int attn_unwatched = 0;          /* a watch stood down with its pause tier standing */
static char attn_verdict[COOL_VERDICT_MAX];
static char attn_reason[COOL_REASON_MAX];
static double attn_at = 0.0;
static double attn_cleared_at = 0.0;
static int attn_lamp_up = 0;

typedef struct {
    unsigned seq;
    char verdict[COOL_VERDICT_MAX];
    char reason[COOL_REASON_MAX];
    double at, cleared_at;
    int times;
} attn_notice_t;
static attn_notice_t attn_notices[COOL_NOTICES_N];
static int attn_notice_n = 0;
static unsigned attn_notice_seq = 0;
static char pub_notices[COOL_NOTICES_JSON_MAX] = "[]";

/* Under mu. */
static void notices_publish(void)
{
    size_t o = 0;
    int n = snprintf(pub_notices, sizeof(pub_notices), "[");
    o = (size_t)n;
    for (int i = 0; i < attn_notice_n; i++) {
        const attn_notice_t *a = &attn_notices[i];
        n = snprintf(pub_notices + o, sizeof(pub_notices) - o,
                     "%s{\"seq\":%u,\"verdict\":\"%s\",\"reason\":\"%s\",\"at\":%.0f,"
                     "\"cleared_at\":%.0f,\"times\":%d}",
                     i ? "," : "", a->seq, a->verdict, a->reason, a->at, a->cleared_at, a->times);
        if (n < 0 || (size_t)n >= sizeof(pub_notices) - o - 1) {
            snprintf(pub_notices, sizeof(pub_notices), "[]");   /* never cut */
            return;
        }
        o += (size_t)n;
    }
    snprintf(pub_notices + o, sizeof(pub_notices) - o, "]");
}

/* Under mu. */
static void notice_add(const char *verdict, const char *reason, double at, double cleared_at)
{
    if (attn_notice_n > 0 && !strcmp(attn_notices[attn_notice_n - 1].verdict, verdict)) {
        attn_notice_t *a = &attn_notices[attn_notice_n - 1];
        a->cleared_at = cleared_at;
        a->times++;
    } else {
        if (attn_notice_n == COOL_NOTICES_N) {
            memmove(&attn_notices[0], &attn_notices[1],
                    sizeof(attn_notices[0]) * (COOL_NOTICES_N - 1));
            attn_notice_n--;
        }
        attn_notice_t *a = &attn_notices[attn_notice_n++];
        a->seq = ++attn_notice_seq;
        snprintf(a->verdict, sizeof(a->verdict), "%s", verdict);
        snprintf(a->reason, sizeof(a->reason), "%s", reason);
        a->at = at;
        a->cleared_at = cleared_at;
        a->times = 1;
    }
    notices_publish();
    fflog(LOG_NOTICE, "cool: %s cleared on its own after %.0f s", verdict, cleared_at - at);
}

void cool_notices_dismiss(unsigned upto)
{
    pthread_mutex_lock(&mu);
    int k = 0;
    for (int i = 0; i < attn_notice_n; i++)
        if (attn_notices[i].seq > upto)
            attn_notices[k++] = attn_notices[i];
    attn_notice_n = k;
    notices_publish();
    pthread_mutex_unlock(&mu);
}

static int attn_needs_operator(const char *verdict)
{
    static const char *const names[] = { "FIRE", "CRASH", "FLAME", "BUMP", "AIRFLOW",
                                         "CRITICAL", "SENSOR" };
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (!strcmp(verdict, names[i]))
            return 1;
    return 0;
}

static int attn_fail_tier(const char *verdict)
{
    return !strcmp(verdict, "FIRE") || !strcmp(verdict, "CRASH") ||
           !strcmp(verdict, "AIRFLOW") || !strcmp(verdict, "CRITICAL");
}

/* A failure whose cause has gone waits for the acknowledgment. Under mu. */
static int attn_ack_waits(void)
{
    return attn_latched && !attn_live;
}

static void attention_tick(const char *verdict)
{
    int live = attn_needs_operator(verdict);
    int motion = super_motion_fault();
    pthread_mutex_lock(&mu);
    double wall_now = (double)time(NULL);
    /* A new cause takes the record, unless the record waits for its
     * acknowledgment: that one stays what the panel asks about (the new one
     * shows as the verdict). One that another took over from may still stand
     * under it (a verdict names only the highest), so it is not called
     * cleared. */
    if (live && attn_latched && attn_sticky && strcmp(attn_verdict, verdict) && !attn_live)
        fflog(LOG_WARNING, "cool: firing blocked, the operator's attention wanted: %s (the "
              "acknowledgment of %s still waits)", pub_reason[0] ? pub_reason : verdict, attn_verdict);
    if (live && (!attn_latched || (!attn_sticky && strcmp(attn_verdict, verdict)))) {
        snprintf(attn_verdict, sizeof(attn_verdict), "%s", verdict);
        snprintf(attn_reason, sizeof(attn_reason), "%s", pub_reason);
        /* The reason goes into JSON as it stands. */
        for (char *p = attn_reason; *p; p++)
            if (*p == '"' || *p == '\\')
                *p = '\'';
        attn_at = wall_now;
        attn_cleared_at = 0.0;
        fflog(LOG_WARNING, "cool: firing blocked, the operator's attention wanted: %s",
              pub_reason[0] ? pub_reason : verdict);
    }
    /* The window closing, or the engine returning to idle (a watch that
     * stands down at the session's end clears its cause without the cause
     * having cleared): a cause still standing then needs the acknowledgment. */
    int in_session = cool_state != Cool_Idle;
    int session_ended = (attn_was_armed && !eff_armed) || (attn_was_session && !in_session);
    /* A watch that stood down over its own pause tier (the crash watch, when
     * the head stops answering) ended that cause the same way: nothing saw
     * it clear, so it needs the acknowledgment too. */
    if (attn_unwatched) {
        attn_unwatched = 0;
        session_ended = 1;
    }
    if (live) {
        attn_latched = 1;
        if (attn_fail_tier(verdict) || session_ended)
            attn_sticky = 1;
    } else if (attn_latched && !attn_sticky) {
        if (session_ended) {
            attn_sticky = 1;
        } else {
            attn_latched = 0;
            notice_add(attn_verdict, attn_reason, attn_at, wall_now);
        }
    }
    if (attn_live && !live)
        attn_cleared_at = wall_now;
    attn_live = live;
    attn_was_armed = eff_armed;
    attn_was_session = in_session;
    int want = attn_latched || motion;
    pthread_mutex_unlock(&mu);

    if (want) {
        if (led_red_up() != 1)
            led_set(LED_FLASH_RED);
        attn_lamp_up = 1;
    } else if (attn_lamp_up) {
        attn_lamp_up = 0;
        /* Only the flash is the engine's to put out. One that cannot be
         * read is put out too: a red left flashing would say firing is
         * blocked when it is not. */
        if (led_red_up() != 0)
            led_release();
    }
}

int cool_attention_ack(void)
{
    pthread_mutex_lock(&mu);
    int rc = attn_live ? -1 : 0;
    if (rc == 0 && attn_latched) {
        attn_latched = 0;
        attn_sticky = 0;
        /* Only the acknowledged fault's words go: another cause's stand. */
        if (!strcmp(pub_reason, attn_reason))
            pub_reason[0] = '\0';
        fflog(LOG_NOTICE, "cool: the operator acknowledged %s", attn_verdict);
    }
    pthread_mutex_unlock(&mu);
    return rc;
}

/* The crash watch disarmed with its pause tier standing. Nothing polls the
 * part from here, so nothing could release the alert: left standing, BUMP
 * holds for the rest of the session and the Acknowledge is refused. The
 * alert goes with the watch (the fail tier's alarm stays for its session),
 * and since nothing saw the head sit quiet, what it held waits for the
 * operator's acknowledgment: the window closing makes it so, and a watch
 * that stood down inside an open window says so itself (attn_unwatched). */
static void crash_alert_stand_down(void)
{
    if (!crash_w.alert)
        return;
    crash_w.alert = 0;
    crash_w.clear_ticks = 0;
    attn_unwatched = 1;
    info("head bump alert ended with its watch; firing waits for the acknowledgment");
    pthread_mutex_lock(&mu);
    pub_reason[0] = '\0';
    pthread_mutex_unlock(&mu);
}

static void flood_apply(int on, double now)
{
    if (on) {
        conf_reload();              /* GUI changes apply per job */
        off_would_warned = 0;
        for (int i = 0; i < Fan_N; i++)
            airflow_reset(&fan_gate[i]);
        airflow_alarm = 0;
        fans_up = 0;
        fans_up_at = -1.0;
        memset(fan_lost, 0, sizeof(fan_lost));
        fan_would_warned = 0;
        memset(fan_margin_noted, 0, sizeof(fan_margin_noted));
        critical_alarm = 0;
        critical_would_warned = 0;
        fan_grace_until = now + (double)fan_grace_s;
        /* A session that opens under the warm-up gate holds there: the
         * loop heater on, the fans idle (their airflow cools the loop),
         * fire blocked, until the upstream reading reaches the gate. */
        float up_now = 0;
        int have_up_now = read_temp("pic/water_temp_2", &up_now);
        if (!gate_warmup_off && have_up_now && up_now < temp_start_c) {
            cool_state = Cool_Warmup;
            warmup_gate_c = temp_start_c;
            warmup_last_up = up_now;
            warmup_progress_at = now;
            warmup_stall_warned = 0;
            warmup_win_n = 0;
            warmup_started_at = now;
            warmup_spent = 0;
            warmup_hot = 0;
            fans_idle();
            flowrun_yield("warm-up");
            heater_set_pct(WARMUP_HEATER_PCT);
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "WARM-UP: coolant %.1f C under the %.0f C start gate - "
                     "heater on, hold", up_now, temp_start_c);
            warn(msg);
        } else {
            cool_state = Cool_Run;
            fans_run();
        }
        /* New run session: a standing fire alarm clears - a new job
         * means the operator is back at the machine. The pause tier's hold
         * is not the session's to clear: a FLAME standing from the last
         * session's tail stands on into this one, and is released as any
         * other, by FIRE_CLEAR_TICKS clean readings. */
        ir_over_ticks = 0;
        fire_alarm = 0;
        crash_reset(&crash_w);
        crash_poll_errs = 0;
        pgood_warned = 0;
    } else if (cool_state == Cool_Run || cool_state == Cool_Warmup) {
        if (cool_state == Cool_Warmup) {
            heater_set_pct(0);
            info("warm-up ended with the session");
            pthread_mutex_lock(&mu);
            pub_reason[0] = '\0';
            pthread_mutex_unlock(&mu);
        }
        cool_state = Cool_Smoke;
        phase_until = now + (session_armed ? (double)smoke_s : 0.0);
        /* A fan fault is the session's: its verdict and its hold end with
         * it, because the next session re-proves every fan after the grace
         * before anything can fire, and a hold left standing would cancel
         * jogs at idle. What outlasts the session is the operator's
         * acknowledgment (attention_tick): fire blocked, motion free. */
        if (airflow_alarm)
            info("airflow fault cleared with the run session; the next "
                 "session judges every fan afresh");
        airflow_alarm = 0;
        for (int i = 0; i < Fan_N; i++)
            airflow_reset(&fan_gate[i]);
        /* The crash fault is the session's too: motion is stopped and
         * the latch locked already, and the next job means the operator
         * is back at the machine. */
        if (crash_w.alarm)
            info("head crash fault cleared with the run session");
        crash_reset(&crash_w);
        /* The coolant critical fault likewise: the ceiling's pause tier
         * keeps holding while the loop is hot, and the next session
         * judges the critical line afresh. */
        if (critical_alarm) {
            info("coolant critical fault cleared with the run session; "
                 "the ceiling holds until the loop cools");
            critical_alarm = 0;
            /* The reason follows the verdict: the ceiling's hold, if it
             * stands, names itself again; otherwise nothing does. */
            if (over_temp_gate)
                overtemp_warn(0);
            else {
                pthread_mutex_lock(&mu);
                pub_reason[0] = '\0';
                pthread_mutex_unlock(&mu);
            }
        }
        critical_alarm = 0;
        /* What the job's sensors saw is the sensors logger's job record
         * (senslog.h). */
    }
}

/* One engine tick at 1 Hz. Hardware writes happen only on transitions
 * so the engine coexists with a not-yet-migrated in-controller writer
 * during bring-up. */
static void engine_tick(void)
{
    double now = wall_s();

    /* Diagnostics own the hardware between cool_diag_take and
     * cool_diag_release: publish the phase, keep fire blocked, and
     * touch nothing until the release reasserts the idle posture. */
    pthread_mutex_lock(&mu);
    int dia = diag_owned;
    pthread_mutex_unlock(&mu);
    if (dia) {
        pthread_mutex_lock(&mu);
        pub_phase = "diag";
        pthread_mutex_unlock(&mu);
        flowrun_ctx_t fc = { .t = now, .yield = 1, .yield_why = "diagnostics", .session = 0,
                             .fans = FW_FANS_OTHER };
        flowrun_engine(&fc);
        /* Nothing is sensed while a diagnostic owns the hardware, so the
         * operator's attention stands as the last tick left it: a cause
         * that was live is still counted live (no notice says it cleared),
         * the red flash is kept up over whatever the diagnostic's side
         * wrote, and an acknowledgment that waits keeps its name. Fire is
         * blocked and the hold stands either way. */
        char held[COOL_VERDICT_MAX];
        pthread_mutex_lock(&mu);
        snprintf(held, sizeof(held), "%s", attn_live ? attn_verdict : "OK");
        pthread_mutex_unlock(&mu);
        attention_tick(held);
        pthread_mutex_lock(&mu);
        int ack_wait = attn_latched && !attn_live;
        pthread_mutex_unlock(&mu);
        verdict_publish(0, ack_wait ? "ACK" : "OK", 1, 0, 0, 0, 0);
        return;
    }

    /* Snapshot the report. */
    pthread_mutex_lock(&mu);
    int mode = rep_mode, armed = rep_armed;
    double at = rep_at;
    long duty[3] = {rep_duty[0], rep_duty[1], rep_duty[2]};
    cool_limits_t hdr = rep_lim;
    pthread_mutex_unlock(&mu);

    int fresh = at >= 0 && now - at <= REPORT_TIMEOUT_S;
    limits_apply(&hdr, fresh);
    if (!fresh) {
        /* Dead-man for a HUNG controller, not just a dead one (the
         * supervisor covers deaths): a reporter that went silent with
         * the armed window open, or with the kernel still playing the
         * ring, has nobody left to stop the beam - stop motion and
         * lock the laser here. The armed case fires once per silence
         * episode; the running case repeats until the stop takes, so a
         * failed write cannot be a one-shot miss. Gated on at >= 0: a
         * freshly (re)started engine must not shoot down a healthy
         * orphaned controller that has not reported to it yet. */
        if (at >= 0 && ((armed && !silent_safed) || cnc_is_running())) {
            /* The writes first, the words after: a log line waits for
             * nothing, but nothing waits for a log line. */
            safing_write("cnc/stop", "1");
            safing_write("cnc/laser_latch", "1");
            if (!silent_safed) {
                warn("controller silent while armed/running - "
                     "stopping motion, locking laser");
                senslog_event("controller silent while armed/running");
            }
            silent_safed = 1;
        }
        mode = 0;
        armed = 0;
        if (at >= 0 && !silent_warned && (flood_on || cool_state == Cool_Run)) {
            silent_warned = 1;
            warn("controller went silent - standing down");
        }
    } else {
        silent_warned = 0;
        silent_safed = 0;
    }

    int flood = fresh && (mode == 1 || armed);
    int duty_changed = memcmp(run_duty, duty, sizeof(run_duty)) != 0;
    memcpy(run_duty, duty, sizeof(run_duty));
    int armed_rose = armed && !eff_armed;
    eff_armed = armed;
    /* A quiet hold (a listening with the fans off) never meets a run
     * session opening: the opening edge releases it (a session already
     * open when the hold was taken is the finder's own after a burn,
     * and stands), and so does the clock, so a listener that died
     * cannot leave the fans off. */
    pthread_mutex_lock(&mu);
    int quiet = quiet_held;
    double quiet_age = now - quiet_since;
    pthread_mutex_unlock(&mu);
    if (quiet && ((flood && !flood_on) || armed_rose)) {
        cool_quiet_hold(0);
        warn("quiet hold released: a run session opened");
    } else if (quiet && quiet_age > (double)COOL_QUIET_HOLD_MAX_S) {
        cool_quiet_hold(0);
        warn("quiet hold released: it ran out of time");
    }
    /* The session's armed history decides its smoke phase: cleared as
     * a session opens (this tick, before flood_apply), set on any tick
     * the window is open. */
    if (flood && !flood_on)
        session_armed = 0;
    if (flood && armed)
        session_armed = 1;
    /* Reapply for a duty change carried by a run report, or for the
     * armed window opening (armed, a job's profile may no longer hold a
     * fan below run duty). Only those: a run-ending report drops the
     * per-job profile with it, and writing the fallback duties from that
     * report would blast the fans on the way OUT of a job that ran a
     * quiet profile. A fan that just sped up gets its spin-up grace. */
    if ((duty_changed || armed_rose) && flood && flood_on &&
        cool_state == Cool_Run && !forced_cool) {
        long was[3] = {cmd_duty[0], cmd_duty[1], cmd_duty[2]};
        fans_run();     /* per-job profile changed mid-run */
        for (int i = 0; i < 3; i++)
            if (cmd_duty[i] > was[i]) {
                fan_grace_until = now + (double)fan_grace_s;
                break;
            }
    }

    if (flood != flood_on) {
        flood_on = flood;
        flood_apply(flood, now);
        if (flood) {
            /* The session start reloaded the settings: resolve the
             * effective limits from them now, so this tick's gate rows,
             * gates_off and the run-start log line agree. */
            limits_apply(&hdr, fresh);
            pthread_mutex_lock(&mu);
            pub_reason[0] = '\0';   /* new session, stale reason gone */
            pthread_mutex_unlock(&mu);
        }
    }

    /* Emission witness: laser_on_sampled counts the ~1 s window's
     * emitting samples on the gated output of the hardware AND-gate -
     * physical evidence, not a commanded state. Emission with no armed
     * window in the recent past (3 s covers the sample-window lag plus
     * the job-end tail) gets the same treatment as a hung controller.
     * The stop repeats while the evidence persists. */
    if (fresh && armed)
        last_armed_at = now;
    long em = rd_long("cnc/laser_on_sampled");
    if (em > 0 && (last_armed_at < 0 || now - last_armed_at > 3.0)) {
        safing_write("cnc/stop", "1");
        safing_write("cnc/laser_latch", "1");
        if (!emission_warned) {
            warn("LASER EMISSION SENSED with no armed window - "
                 "stopping motion, locking laser");
            senslog_event("laser emission sensed with no armed window");
        }
        emission_warned = 1;
    } else if (em == 0)
        emission_warned = 0;

    /* Supply power-good witness: the supply drives the line good the whole
     * time it is healthy (it follows neither HV_ENABLE nor emission), so
     * a window in which most samples read not-good means the supply's
     * supervisor reported a fault. Warn once per session. */
    if (fresh && armed && !pgood_warned) {
        long pg = rd_long("cnc/laser_pgood_sampled");
        if (pg >= 0 && pg < 128) {
            pgood_warned = 1;
            warn("laser supply power-good dropped during the armed window");
            senslog_event("laser supply power-good dropped during the armed window");
        }
    }

    /* Stepper-fault visibility: the kernel latches triggered faults;
     * surface a transition to nonzero during the run window. */
    long faults = rd_long("cnc/faults");
    if (faults > 0 && faults != last_faults && flood_on) {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "stepper driver fault reported (mask %ld)", faults);
        warn(msg);
        senslog_event(msg);
    }
    if (faults >= 0)
        last_faults = faults;

    /* Lid IR fire watch. The tiers judge the sorted readings (the
     * quartiles) against the factory-shaped thresholds through the whole
     * session tail. What the channels read over a job is the sensors
     * logger's job record (senslog.h). */
    long ir[4];
    int have_ir = 1;
    for (int i = 0; i < 4; i++) {
        char attr[20];
        snprintf(attr, sizeof(attr), "pic/lid_ir_%d", i + 1);
        ir[i] = rd_long(attr);
        if (ir[i] < 0)
            have_ir = 0;
    }
    if (have_ir && (cool_state == Cool_Run || cool_state == Cool_Smoke ||
                    cool_state == Cool_Thermal)) {
        long q[4] = {ir[0], ir[1], ir[2], ir[3]};
        for (int i = 0; i < 3; i++)         /* four values: sort them */
            for (int j = i + 1; j < 4; j++)
                if (q[j] < q[i]) { long t = q[i]; q[i] = q[j]; q[j] = t; }
        int crit = (fire_q1_critical > 0 && q[0] > (long)fire_q1_critical) ||
                   (fire_q2_critical > 0 && q[1] > (long)fire_q2_critical);
        int alert = crit ||
                    (fire_q1_alert > 0 && q[0] > (long)fire_q1_alert) ||
                    (fire_q2_alert > 0 && q[1] > (long)fire_q2_alert);
        ir_over_ticks = alert ? ir_over_ticks + 1 : 0;
        if (crit && ir_over_ticks >= FIRE_IR_TICKS && !fire_alarm) {
            fire_alarm = 1;
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "LID IR FIRE SIGNAL (quartiles %ld %ld) - motion "
                     "stopped, laser locked, smoke airflow held", q[0], q[1]);
            safing_write("cnc/stop", "1");
            safing_write("cnc/laser_latch", "1");
            fans_run();     /* full smoke-clear airflow */
            warn(msg);
            /* The kernel writes come first and stand on their own; the
             * controller is ended as well, so no run start can relight
             * what was just locked and the sender sees the job end. */
            if (cool_fail_tier_stop)
                cool_fail_tier_stop("lid IR fire signal");
        } else if (!crit && ir_over_ticks >= FIRE_IR_TICKS && !flame_alert
                   && !fire_alarm) {
            flame_alert = 1;
            flame_clear_ticks = 0;
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "lid IR flame alert (quartiles %ld %ld) - job held, "
                     "fire blocked until the signal clears", q[0], q[1]);
            warn(msg);
        }
        if (flame_alert && !alert) {
            if (++flame_clear_ticks >= FIRE_CLEAR_TICKS) {
                flame_alert = 0;
                info("lid IR flame alert cleared - released");
                pthread_mutex_lock(&mu);
                pub_reason[0] = '\0';
                pthread_mutex_unlock(&mu);
            }
        } else if (flame_alert)
            flame_clear_ticks = 0;
    } else if (flame_alert && cool_state == Cool_Idle) {
        /* The watch stands down at idle and its hold goes with it: a flame
         * still alight then is the acknowledgment's (attention_tick). */
        flame_alert = 0;
        flame_clear_ticks = 0;
    } else if (flame_alert) {
        /* Not judged this tick (a lid IR read failed): the hold stands, and
         * the tick is not a clean one. The release is FIRE_CLEAR_TICKS clean
         * readings in a row, so the count starts over. */
        flame_clear_ticks = 0;
    }
    /* The fire alarm is the session's, its tail included: once the engine
     * is back at idle the watch stands down, and the alarm goes with it.
     * The operator's acknowledgment carries it from there (attention_tick):
     * fire stays blocked until the panel's Acknowledge, motion is free. An
     * IR read that fails mid-session clears nothing. */
    if (fire_alarm && cool_state == Cool_Idle) {
        fire_alarm = 0;
        info("lid IR fire alarm ended with its session; firing waits for the acknowledgment");
    }
    pthread_mutex_lock(&mu);
    pub_fire_watch = fire_alarm ? "ALARM"
                   : flame_alert ? "alert"
                   : (fire_q1_alert > 0 || fire_q1_critical > 0 ||
                      fire_q2_alert > 0 || fire_q2_critical > 0) ? "armed"
                   : "watch";
    pthread_mutex_unlock(&mu);

    /* The head-accelerometer crash watch: the factory mechanism, the
     * LIS2HH12's own interrupt generators armed over i2c-dev while
     * st_accel stays bound (accel.h carries the register story). Armed
     * only inside the laser's armed window: liveness and gfhome read
     * the accel through st_accel in unarmed sessions, and the watch
     * owns the ODR and full scale only while armed. IG1 (per-axis
     * alert) is the pause tier, released after quiet polls; IG2
     * (shared abort) is the fail tier: motion stopped, latch locked,
     * a fault for the rest of the run session. A head that stops
     * answering stands the watch down for the session, said once. */
    {
        int want = armed && (accel_x_alert > 0 || accel_y_alert > 0 ||
                             accel_abort > 0);
        if (want && !crash_hw_armed && crash_hw_state >= 0 &&
            crash_poll_errs < 3) {
            if (crash_hw_state == 0) {
                crash_hw_state = crash_hw_open() == 0 ? 1 : -1;
                if (crash_hw_state < 0)
                    warn("head accelerometer not answering - crash watch off");
            }
            if (crash_hw_state > 0) {
                if (crash_hw_arm((int)accel_x_alert, (int)accel_y_alert,
                                 (int)accel_abort) == 0) {
                    crash_hw_armed = 1;
                    char msg[96];
                    snprintf(msg, sizeof(msg),
                             "crash watch armed (alert x=%d y=%d abort=%d)",
                             (int)accel_x_alert, (int)accel_y_alert,
                             (int)accel_abort);
                    info(msg);
                } else {
                    crash_hw_state = -1;
                    warn("head accelerometer arm failed - crash watch off");
                }
            }
        } else if (!want && crash_hw_armed) {
            crash_hw_disarm();
            crash_hw_armed = 0;
            crash_poll_errs = 0;
            crash_alert_stand_down();
        }
        if (crash_hw_armed) {
            unsigned s1, s2;
            if (crash_hw_poll(&s1, &s2) == 0) {
                crash_poll_errs = 0;
                char msg[96];
                switch (crash_tick(&crash_w, s1, s2)) {
                case CrashEv_Alarm:
                    snprintf(msg, sizeof(msg),
                             "HEAD CRASH SIGNAL (axes %s) - motion stopped, "
                             "laser locked", crash_axes_name(crash_w.axes));
                    safing_write("cnc/stop", "1");
                    safing_write("cnc/laser_latch", "1");
                    warn(msg);
                    if (cool_fail_tier_stop)
                        cool_fail_tier_stop("head crash signal");
                    break;
                case CrashEv_Alert:
                    snprintf(msg, sizeof(msg),
                             "head bump alert (axes %s) - job held, fire "
                             "blocked until the signal clears",
                             crash_axes_name(crash_w.axes));
                    warn(msg);
                    break;
                case CrashEv_Released:
                    info("head bump alert cleared - released");
                    pthread_mutex_lock(&mu);
                    pub_reason[0] = '\0';
                    pthread_mutex_unlock(&mu);
                    break;
                default:
                    break;
                }
            } else if (++crash_poll_errs >= 3) {
                crash_hw_disarm();
                crash_hw_armed = 0;
                crash_alert_stand_down();
                warn("head accelerometer stopped answering - crash watch "
                     "down for this session");
            }
        }
        pthread_mutex_lock(&mu);
        pub_accel_watch = crash_w.alarm ? "ALARM"
                        : crash_w.alert ? "alert"
                        : crash_hw_state < 0 || crash_poll_errs >= 3 ? "off"
                        : crash_hw_armed ? "armed"
                        : "watch";
        pthread_mutex_unlock(&mu);
    }

    /* The airflow gates: a fan is judged while the run profile is
     * applied and the laser is armed, or the fan is commanded at the run
     * duty its floor was measured at (airflow_judged); a job that runs a
     * fan slower unarmed (a hunt, extraction fans off) is measured and
     * published, not judged. Nothing counts during the spin-up grace.
     * The exhaust and the intakes report a period in ns at 2 pulses/rev,
     * the air assist in us at 8, the purge fan a current (no duty: it is
     * always on, so always judged in run). The floors are the effective
     * ones (a header can raise a tach floor for a job). A trip is a
     * fault for the rest of the session: hold, fire blocked, no resume;
     * the fans stay at run duty, which is what a stalled extraction fan
     * needs around it. An off gate (floor 0) still measures and names
     * the first reading that would have tripped the shipped default.
     * The second intake on a machine built with one intake fan is
     * measured and published as not fitted, never judged. */
    {
        fans_verify(now);
        fan_reading[Fan_Exhaust] = airflow_rpm(rd_long("thermal/tach_exhaust"), 1e9, 2);
        fan_reading[Fan_Intake1] = airflow_rpm(rd_long("thermal/tach_intake_1"), 1e9, 2);
        fan_reading[Fan_Intake2] = airflow_rpm(rd_long("thermal/tach_intake_2"), 1e9, 2);
        fan_reading[Fan_AirAssist] = airflow_rpm(rd_long("head/air_assist_tach"), 1e6, 8);
        long purge = rd_long("head/purge_air_current");
        fan_reading[Fan_Purge] = purge > 0 ? (double)purge : 0.0;
        fan_floor[Fan_Exhaust] = eff_lim.exhaust_min_rpm;
        fan_floor[Fan_Intake1] = eff_lim.intake_min_rpm;
        fan_floor[Fan_Intake2] = intake_fans == 1 ? 0.0 : eff_lim.intake_min_rpm;
        fan_floor[Fan_AirAssist] = eff_lim.air_assist_min_rpm;
        fan_floor[Fan_Purge] = purge_min_current;
        static const double fan_default[Fan_N] = {
            TACH_EXHAUST_MIN_RPM, TACH_INTAKE_MIN_RPM, TACH_INTAKE_MIN_RPM,
            TACH_AIR_ASSIST_MIN_RPM, PURGE_MIN_CURRENT };
        static const int fan_duty_ix[Fan_N] = {1, 2, 2, 0, -1};
        static const long fan_local[3] = {AIR_ASSIST_RUN, EXHAUST_RUN, INTAKE_RUN};
        int run = cool_state == Cool_Run;
        int in_grace = now < fan_grace_until;
        int up_now = 1;
        coolfmt_fan_t rows[Fan_N];
        for (int i = 0; i < Fan_N; i++) {
            airflow_state_t st;
            int dx = fan_duty_ix[i];
            /* A fan the machine was built without is never judged. */
            int fitted = i != Fan_Intake2 || intake_fans != 1;
            int judged = !fitted ? 0
                       : dx < 0 ? airflow_judged(run, armed, 1, 1)
                       : airflow_judged(run, armed, cmd_duty[dx], fan_local[dx]);
            if (judged && fan_floor[i] > 0.0 && fan_reading[i] < fan_floor[i])
                up_now = 0;
            if (judged) {
                st = airflow_tick(&fan_gate[i], fan_reading[i], fan_floor[i], in_grace);
                if (!in_grace && !fan_margin_noted[i] && fan_floor[i] > 0.0
                    && fan_reading[i] >= fan_floor[i]
                    && fan_reading[i] < fan_floor[i] * 1.10) {
                    fan_margin_noted[i] = 1;
                    char why[96];
                    snprintf(why, sizeof(why), "%s ran within 10 percent of its floor (%.0f of %.0f)",
                             fan_name[i], fan_reading[i], fan_floor[i]);
                    setup_flag("airflow", why);
                }
                if (st == Air_Tripped && !airflow_alarm) {
                    airflow_alarm = 1;
                    /* The duty commanded and the duty in force beside
                     * the reading: a fan that never got its duty reads
                     * as that, not as a slow fan. */
                    char duty[40] = "";
                    if (dx >= 0)
                        snprintf(duty, sizeof(duty), " (duty %ld, reads %ld)",
                                 fan_cmd[dx], fan_back[dx]);
                    char msg[COOL_REASON_MAX];
                    snprintf(msg, sizeof(msg),
                             "AIRFLOW: %s %.0f under the %.0f floor for %d s%s - "
                             "hold, no resume this job",
                             fan_name[i], fan_reading[i], fan_floor[i],
                             AIRFLOW_DEBOUNCE_TICKS, duty);
                    warn(msg);
                }
                if (st == Air_Off && !in_grace && !fan_would_warned &&
                    fan_reading[i] < fan_default[i]) {
                    fan_would_warned = 1;
                    fflog(LOG_WARNING, "cool: gate %s is OFF and would have "
                          "tripped at the default %.0f (reading %.0f)",
                          fan_name[i], fan_default[i], fan_reading[i]);
                }
            } else {
                st = fan_gate[i].tripped ? Air_Tripped
                   : fan_floor[i] <= 0.0 ? Air_Off : Air_Ok;
            }
            rows[i].name = fan_name[i];
            rows[i].reading = fan_reading[i];
            rows[i].floor = fan_floor[i];
            rows[i].state = !fitted ? "not fitted"
                          : judged ? airflow_state_name(st)
                          : st == Air_Tripped ? "TRIPPED"
                          : st == Air_Off ? "off"
                          : run ? "unjudged" : "idle";
        }
        /* The airflow is up once every gated fan reads its floor; in an
         * armed session that is the moment the window is acknowledged
         * and the beam may start. */
        if (flood_on && up_now && !fans_up) {
            fans_up_at = now;
            if (armed)
                info("fans at their floors: the armed window is acknowledged");
        }
        fans_up = flood_on && up_now;
        char fg[sizeof(pub_fan_gates)];
        if (coolfmt_fan_gates(fg, sizeof(fg), rows, Fan_N) < 0)
            fflog(LOG_ERR, "cool: the fan gate rows do not fit their status "
                  "fragment; publishing none");
        pthread_mutex_lock(&mu);
        snprintf(pub_fan_gates, sizeof(pub_fan_gates), "%s", fg);
        pthread_mutex_unlock(&mu);
    }

    float down = 0, up = 0;
    int have_down = read_temp("pic/water_temp_1", &down);
    int have_up = read_temp("pic/water_temp_2", &up);

    /* The sensors themselves. Blind on either coolant sensor, the
     * ceiling, the floor, the critical line and the warm-up can none of
     * them judge, so past SENSOR_FAIL_TICKS the engine says so (SENSOR:
     * fire blocked, hold) and takes the heater off: a warm-up gets its
     * heater back once the readings are. The gates keep their state
     * throughout; the verdict is released the moment both sensors read
     * again. */
    if (have_down && have_up) {
        sensor_fail_ticks = 0;
        if (sensor_alarm) {
            sensor_alarm = 0;
            info("coolant sensors reading again");
            if (over_temp_gate)
                overtemp_warn(0);
            else {
                pthread_mutex_lock(&mu);
                pub_reason[0] = '\0';
                pthread_mutex_unlock(&mu);
            }
        }
    } else {
        if (sensor_fail_ticks < UINT32_MAX)
            sensor_fail_ticks++;
        if (!sensor_alarm && sensor_fail_ticks >= SENSOR_FAIL_TICKS) {
            sensor_alarm = 1;
            char msg[112];
            snprintf(msg, sizeof(msg),
                     "SENSOR: coolant %s unreadable for %u s - fire blocked, "
                     "hold, heater off",
                     !have_down && !have_up ? "sensors"
                     : !have_up ? "upstream sensor" : "downstream sensor",
                     (unsigned)sensor_fail_ticks);
            warn(msg);
            flowrun_yield("a coolant sensor is unreadable");
            heater_set_pct(0);
        }
    }

    if (have_up && gate_coolant_off) {
        /* The ceiling is at its off end: no gate, and no hold from it.
         * The reading is still watched against the shipped default so
         * a machine running without the gate leaves a record of what
         * it would have done. */
        if (over_temp_gate) {
            over_temp_gate = 0;
            pthread_mutex_lock(&mu);
            pub_reason[0] = '\0';
            pthread_mutex_unlock(&mu);
            if (forced_cool) {
                forced_cool = 0;
                fans_apply_phase();
            }
        }
        if (up > TEMP_MAX_C_DEFAULT && !off_would_warned) {
            off_would_warned = 1;
            fflog(LOG_WARNING, "cool: gate coolant_max is OFF and would "
                  "have tripped at the default %.0f C (coolant %.1f C)",
                  TEMP_MAX_C_DEFAULT, up);
        }
    } else if (have_up) {
        /* Fire gate with hysteresis: gated over the run ceiling, back
         * in service under the resume gate. The hold request rides the
         * same gate: the controller holds while it stands, resumes
         * (its call) once resume_ok returns. */
        int was = over_temp_gate;
        over_temp_gate = up > (over_temp_gate ? eff_temp_resume_c : eff_temp_max_c);
        if (over_temp_gate && !was) {
            last_up_c = up;
            overtemp_warn(1);
            if (cool_state != Cool_Run) {
                fans_cool();
                forced_cool = 1;
            }
        } else if (!over_temp_gate && was) {
            info("coolant temperature recovered");
            pthread_mutex_lock(&mu);
            pub_reason[0] = '\0';
            pthread_mutex_unlock(&mu);
            if (forced_cool) {
                forced_cool = 0;
                fans_apply_phase();
            }
        }
    }

    /* The floor: a fire gate under the ceiling with its own hysteresis
     * (set under the floor, clear a degree above it). At its off end
     * it is no gate. */
    if (have_up && !gate_floor_off) {
        int was = cold_gate;
        cold_gate = gate_floor_trip(cold_gate, up, eff_temp_min_c, TEMP_MIN_HYST_C);
        if (cold_gate && !was) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "COLD: coolant %.1f C under the %.1f C floor%s - fire "
                     "blocked until %.1f C", up, eff_temp_min_c,
                     eff_floor_from_header ? " (the job's)" : "",
                     eff_temp_min_c + TEMP_MIN_HYST_C);
            warn(msg);
        } else if (!cold_gate && was) {
            info("coolant temperature back over the floor");
            pthread_mutex_lock(&mu);
            pub_reason[0] = '\0';
            pthread_mutex_unlock(&mu);
        }
    } else if (cold_gate && gate_floor_off) {
        cold_gate = 0;
        pthread_mutex_lock(&mu);
        pub_reason[0] = '\0';
        pthread_mutex_unlock(&mu);
    }

    /* The warm-up hold: released at the gate into a normal run session
     * (run fans, the spin-up grace). Under the gate a loop that stops
     * warming is named once and keeps holding. */
    if (cool_state == Cool_Warmup && have_up) {
        warmup_win[warmup_win_n % WARMUP_WIN] = up;
        warmup_win_n++;
        float bulk = up;
        if (warmup_win_n >= WARMUP_WIN) {
            for (uint32_t i = 0; i < WARMUP_WIN; i++)
                if (warmup_win[i] < bulk)
                    bulk = warmup_win[i];
        }
        if (warmup_win_n >= WARMUP_WIN && bulk >= warmup_gate_c) {
            heater_set_pct(0);
            cool_state = Cool_Run;
            fans_run();
            fan_grace_until = now + (double)fan_grace_s;
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "warm-up complete: coolant %.1f C at the %.0f C start "
                     "gate - run", bulk, warmup_gate_c);
            info(msg);
            pthread_mutex_lock(&mu);
            pub_reason[0] = '\0';
            pthread_mutex_unlock(&mu);
        } else if (bulk >= warmup_last_up + WARMUP_STALL_C) {
            warmup_last_up = bulk;
            warmup_progress_at = now;
        } else if (!warmup_stall_warned &&
                   now - warmup_progress_at > (double)WARMUP_STALL_S) {
            warmup_stall_warned = 1;
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "warm-up stalled: coolant %.1f C flat for %d s, gate "
                     "%.0f C - lower it or warm the room",
                     bulk, WARMUP_STALL_S, warmup_gate_c);
            warn(msg);
        }
    }

    /* The warm-up heater, decided every tick, and written every tick it may
     * run: the kernel holds a heater write above zero only for its lease
     * (heater_lease_ms, 5 s), so a daemon that stops ticking leaves it off.
     * Off with a blind sensor (SENSOR above), past the downstream cutoff
     * (on again 5 C under it: every gate reads upstream, which sits before
     * the heater), once the budget is spent (the hold stays: a loop that
     * has not reached the gate in 30 minutes is not going to), and with
     * the pump held off by a quiet hold. */
    if (cool_state == Cool_Warmup) {
        if (have_down && !warmup_hot && down >= WARMUP_DOWN_OFF_C) {
            warmup_hot = 1;
            fflog(LOG_WARNING, "cool: warm-up: downstream coolant %.1f C at the %.0f C cutoff - heater off "
                  "until %.0f C", down, WARMUP_DOWN_OFF_C, WARMUP_DOWN_ON_C);
        } else if (have_down && warmup_hot && down <= WARMUP_DOWN_ON_C) {
            warmup_hot = 0;
            info("warm-up: downstream coolant back under the cutoff - heater on");
        }
        if (!warmup_spent && now - warmup_started_at >= (double)WARMUP_HEAT_MAX_S) {
            warmup_spent = 1;
            char msg[112];
            snprintf(msg, sizeof(msg),
                     "WARM-UP: heater off after %d min, coolant %.1f C under the %.0f C gate - hold",
                     WARMUP_HEAT_MAX_S / 60, have_up ? up : NAN, warmup_gate_c);
            warn(msg);
        }
        int heat = have_down && !sensor_alarm && !warmup_hot && !warmup_spent && !cool_quiet_pump_held();
        heater_set_pct(heat ? WARMUP_HEATER_PCT : 0);
    }

    /* The SoC's own thermal governor: it throttles the CPU at its
     * passive trip (85 C on this part) and powers the board off at its
     * critical one (90 C), with no help from here. A throttle is named
     * when it starts and when it ends, since a slower CPU reaches the
     * engine, the camera and the protocol thread before it reaches the
     * step stream (which has the ring in hand). */
    {
        long thr = soc_throttle_state();
        if (thr > 0 && throttle_state <= 0) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "SoC throttled: CPU cooling state %ld (die %.1f C)",
                     thr, soc_degc());
            fflog(LOG_WARNING, "cool: %s", msg);
            senslog_event(msg);
        } else if (thr == 0 && throttle_state > 0) {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "SoC throttle released (die %.1f C)", soc_degc());
            info(msg);
        }
        throttle_state = thr;
    }

    /* The critical tier: the upstream coolant at or over
     * cool_temp_critical_c during a run session is a fault, not a pause
     * (CRITICAL: hold, fire blocked, no resume this session; the session
     * ends it, like a fan fault, and the ceiling's pause tier keeps the
     * hold while the loop is hot). Local only: no header carries a
     * critical line for the coolant. At its far end the gate is off and
     * still measures against the shipped default. */
    if (have_up && cool_state == Cool_Run) {
        if (gate_critical_off) {
            if (up >= TEMP_CRITICAL_C_DEFAULT && !critical_would_warned) {
                critical_would_warned = 1;
                fflog(LOG_WARNING, "cool: gate coolant_critical is OFF and "
                      "would have tripped at the default %.0f C (coolant "
                      "%.1f C)", TEMP_CRITICAL_C_DEFAULT, up);
            }
        } else if (!critical_alarm && up >= temp_critical_c) {
            critical_alarm = 1;
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "CRITICAL: coolant %.1f C at or over the %.0f C critical "
                     "line - hold, no resume this job", up, temp_critical_c);
            warn(msg);
        }
    }

    /* Cooldown phases. The drop to idle duty is additionally gated on
     * the kernel actually being done: the ring can still be playing
     * (cloud mode preloads whole jobs) after the report-driven phases
     * expire, and airflow must never fall below cooldown duty while
     * the machine can still be depositing energy. */
    /* The cooldown airflow a busy engine start took (the kernel not idle
     * at that moment: a takeover's safe state, a respawn mid-move) stands
     * until the machine is idle with no session opened. The idle state
     * never re-applies its own duties, so without this the exhaust ran at
     * cooldown duty for good after a kernel drill's takeover. */
    if (start_cool && cool_state == Cool_Idle && machine_is_idle()) {
        fans_idle();
        fflog(LOG_INFO, "cool: the machine is idle after the busy start; idle airflow");
    }
    if (cool_state == Cool_Smoke && now >= phase_until) {
        if ((have_up && up > temp_resume_c) || cnc_is_running()) {
            cool_state = Cool_Thermal;
            phase_until = now + (double)cooldown_max_s;
            fans_cool();
        } else {
            cool_state = Cool_Idle;
            fans_idle();
        }
    } else if (cool_state == Cool_Thermal) {
        int recovered = have_up && up <= temp_resume_c;
        if ((recovered || now >= phase_until) && cnc_is_running()) {
            phase_until = now + (double)cooldown_max_s;  /* ring still live */
        } else if (recovered || now >= phase_until) {
            if (!recovered) {
                warn("thermal cooldown timed out above the resume gate");
                senslog_event("thermal cooldown timed out above the resume gate");
            }
            cool_state = Cool_Idle;
            fans_idle();
        }
    }

    /* The TEC. Wanted only with the part declared present, a reading in
     * hand, the fans running (its heat sink sits in their airflow), the
     * hysteresis satisfied, and the loop clear of the floor. Off is
     * immediate; on waits the dwell after the last switch. Written on
     * change only (and re-written after a diagnostic hand-back). */
    {
        int airflow = cool_state == Cool_Run || cool_state == Cool_Smoke ||
                      cool_state == Cool_Thermal || forced_cool;
        int want = tec_present && have_up && airflow &&
                   up > (tec_on_state ? tec_off_c : tec_on_c);
        if (!gate_floor_off && have_up &&
            up <= eff_temp_min_c + TEMP_MIN_HYST_C)
            want = 0;
        if (want && !tec_on_state && tec_switched_at > 0 &&
            now - tec_switched_at < (double)TEC_DWELL_S)
            want = tec_on_state;        /* hold the dwell before an on */
        if (want != tec_on_state) {
            tec_on_state = want;
            tec_switched_at = now;
            char msg[96];
            if (want)
                snprintf(msg, sizeof(msg), "TEC on: coolant %.1f C over "
                         "%.1f C, airflow up", up, tec_on_c);
            else
                snprintf(msg, sizeof(msg), "TEC off: %s",
                         !airflow ? "no airflow" : !tec_present ? "not fitted"
                         : have_up && up <= eff_temp_min_c + TEMP_MIN_HYST_C
                           ? "at the coolant floor" : "coolant cooled");
            info(msg);
        }
        if (tec_written != tec_on_state && !cool_quiet_pump_held()) {
            wr_attr("thermal/tec_on", tec_on_state ? "1" : "0");
            tec_written = tec_on_state;
        }
    }

    /* Publish. Enforcement of the pause tier is the controller's: hold
     * asks for a feed hold, resume_ok (= !hold) signals recovery,
     * fire_ok gates the laser. The fail tiers do not wait for it: their
     * kernel writes have landed and the controller is being ended.
     * fire_ok additionally requires a live report: an armed window the
     * engine cannot see must not fire. */
    int warming = cool_state == Cool_Warmup;
    holds_t ext;
    ext_holds_read(now, &ext);
    ext_holds_log(&ext);
    const char *verdict = fire_alarm ? "FIRE"
                        : crash_w.alarm ? "CRASH"
                        : flame_alert ? "FLAME"
                        : crash_w.alert ? "BUMP"
                        : airflow_alarm ? "AIRFLOW"
                        : critical_alarm ? "CRITICAL"
                        : sensor_alarm ? "SENSOR"
                        : over_temp_gate ? "OVERTEMP"
                        : cold_gate ? "COLD"
                        : warming ? "WARMUP"
                        : ext.standing ? "EXT" : "OK";
    /* A failure that waits for the operator's acknowledgment (ACK): its
     * own verdict has gone, and it blocks fire, and holds an armed window,
     * until the panel's Acknowledge. Motion is free. */
    attention_tick(verdict);
    pthread_mutex_lock(&mu);
    int ack_wait = attn_ack_waits();
    /* The fault's words where the verdict is ACK; another verdict keeps its own. */
    if (ack_wait && !strcmp(verdict, "OK"))
        snprintf(pub_reason, sizeof(pub_reason), "%s", attn_reason);
    pthread_mutex_unlock(&mu);
    if (ack_wait && !strcmp(verdict, "OK"))
        verdict = "ACK";
    int hold = fire_alarm || crash_w.alarm || flame_alert || crash_w.alert
             || airflow_alarm || critical_alarm || sensor_alarm
             || over_temp_gate || cold_gate || warming
             || ext.standing || (ack_wait && eff_armed);
    int fire_ok = fresh && !fire_alarm && !crash_w.alarm && !flame_alert
                && !crash_w.alert && !airflow_alarm
                && !critical_alarm && !sensor_alarm
                && !over_temp_gate && !cold_gate && !warming
                && !ext.standing && !ack_wait;
    /* The reason is the hold's while the verdict is: it says which package
     * and why, and another event's words may have stood there since. */
    if (!strcmp(verdict, "EXT")) {
        pthread_mutex_lock(&mu);
        snprintf(pub_reason, sizeof(pub_reason), "%s", ext.reason);
        pthread_mutex_unlock(&mu);
    }

    pthread_mutex_lock(&mu);
    pub_phase = cool_state == Cool_Run ? "run"
              : cool_state == Cool_Warmup ? "warm-up"
              : cool_state == Cool_Smoke ? "smoke"
              : cool_state == Cool_Thermal ? "thermal" : "idle";
    int quiet_now = quiet_held;
    pthread_mutex_unlock(&mu);

    /* The flow watch's context: when it may have the heater, and the fans
     * and the job it sees. */
    {
        flowrun_ctx_t fc;
        fc.t = now;
        /* Never heat a loop the engine has called too hot, or one the TEC
         * is cooling. And never under the floor: the heater sits between the
         * two coolant sensors, and in a loop that is still or slow its heat
         * reaches the upstream one, the reading that releases the floor. */
        int hot_ungated = gate_coolant_off && have_up && up > TEMP_MAX_C_DEFAULT;
        int cooling = cool_state == Cool_Thermal || forced_cool;
        fc.yield = warming || sensor_alarm || quiet_now || over_temp_gate || cold_gate || critical_alarm
                   || tec_on_state || hot_ungated || cooling;
        fc.yield_why = warming ? "warm-up" : sensor_alarm ? "a coolant sensor is unreadable"
                     : quiet_now ? "a quiet hold" : critical_alarm ? "the coolant is at the critical line"
                     : over_temp_gate || hot_ungated ? "the coolant is over the ceiling"
                     : cold_gate ? "the coolant is under the floor"
                     : cooling ? "the engine is cooling the loop"
                     : tec_on_state ? "the TEC is cooling" : NULL;
        fc.offset_counts = (double)cool_coolant_offset_counts();
        fc.temp_offset_c = temp_offset_c;
        fc.session = cool_state == Cool_Run;
        fc.fans = quiet_now ? FW_FANS_OTHER
                : (cool_state == Cool_Run || cool_state == Cool_Smoke) ? FW_FANS_RUN
                : (cool_state == Cool_Thermal || forced_cool || start_cool) ? FW_FANS_COOL
                : FW_FANS_IDLE;
        flowrun_engine(&fc);
    }

    verdict_publish(fire_ok, verdict, hold, have_down, down, have_up, up);
}

/* The tick runs on an absolute one-second grid: the publish period is
 * one second whatever a tick costs, so a reader that polls the verdict
 * at half the staleness window never finds a gap. A tick that overruns
 * its slot is named once. */
static void *engine_main(void *arg)
{
    (void)arg;
    struct timespec next;
    int slow_named = 0;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (1) {
        pthread_mutex_lock(&mu);
        int run = engine_run;
        tick_busy = 1;
        pthread_mutex_unlock(&mu);
        if (!run)
            break;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        engine_tick();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double took = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        pthread_mutex_lock(&mu);
        tick_busy = 0;
        if (isnan(tick_max_ms) || took * 1000.0 > tick_max_ms)
            tick_max_ms = took * 1000.0;
        pthread_mutex_unlock(&mu);
        if (took > 0.5 && !slow_named) {
            slow_named = 1;
            fflog(LOG_WARNING, "cool: an engine tick took %.2f s (budget 1 s)", took);
        }
        next.tv_sec += 1;
        if (t1.tv_sec > next.tv_sec ||
            (t1.tv_sec == next.tv_sec && t1.tv_nsec > next.tv_nsec))
            next = t1;                  /* overran the slot: no catch-up burst */
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
            ;
    }
    pthread_mutex_lock(&mu);
    tick_busy = 0;
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* --------------------------------------------------------------- api */

void cool_init(void)
{
    const char *opt;
    for (size_t i = 0; i < sizeof(tunables) / sizeof(tunables[0]); i++) {
        if ((opt = getenv(tunables[i].env))) {
            /* A limit that is not a finite number is no override: the
             * variable is passed over, and the setting or the default
             * stands (conf_reload). */
            if (tunables[i].f && !isfinite(strtof(opt, NULL))) {
                fflog(LOG_WARNING, "cool: %s=%s is not a finite number; ignored",
                      tunables[i].env, opt);
                continue;
            }
            tunables[i].env_set = 1;
            if (tunables[i].f)
                *tunables[i].f = strtof(opt, NULL);
            else
                *tunables[i].u = (uint32_t)strtoul(opt, NULL, 10);
        }
    }
    conf_reload();

    mkdir(VERDICT_DIR, 0755);

    /* Idle posture. Purge air belongs to the armed/run story the same
     * way air assist does, but the factory holds it on continuously;
     * keep that. A daemon that starts next to an orphaned controller
     * mid-job (a respawn over a crash) must not drop the exhaust under
     * a live cut: a busy machine gets the cooldown duties instead, and
     * the first fresh report opens the session that sets the run
     * profile (the same rule cool_shutdown follows). */
    wr_attr("thermal/water_pump_on", "1");
    wr_attr("thermal/tec_on", "0");
    wr_attr("head/purge_air", "1");
    heater_set_pct(0);
    if (machine_is_idle())
        fans_idle();
    else {
        fflog(LOG_WARNING, "cool: the machine is busy at engine start; "
              "cooldown airflow until it is idle or a session opens");
        start_cool = 1;
        fans_cool();
    }

    engine_run = 1;
    if (pthread_create(&engine_th, NULL, engine_main, NULL) != 0) {
        engine_run = 0;
        fflog(LOG_ERR, "cool: engine thread failed to start");
        return;
    }
    fflog(LOG_INFO, "cool: engine started");
    flowrun_init();
}

void cool_shutdown(void)
{
    pthread_mutex_lock(&mu);
    int was = engine_run;
    engine_run = 0;
    pthread_mutex_unlock(&mu);
    if (was)
        pthread_join(engine_th, NULL);
    /* The flow watch first: its heater off, whatever it was doing. */
    flowrun_shutdown();
    /* The heater is the warm-up's heat source; off is always the right
     * parting state, job or no job. */
    heater_set_pct(0);
    /* The crash watch borrowed the accel's ODR and full scale; hand
     * them back so st_accel's next one-shot reads at its own scale. */
    if (crash_hw_armed) {
        crash_hw_disarm();
        crash_hw_armed = 0;
    }
    crash_hw_close();
    /* A busy machine keeps its orphaned controller running (see
     * super_shutdown), so idling the fans here would drop exhaust under
     * a live cut, and unlinking the verdict would feed-hold it the
     * moment a reader notices. Leave both alone: an aging verdict file
     * reads exactly like a stopped engine within the 2 s staleness
     * window, and the restarted daemon reasserts the right posture. */
    if (machine_is_idle()) {
        fans_idle();
        unlink(VERDICT_FILE);   /* missing file = fire blocked at readers */
    }
}

int cool_state_report(const char *mode, int armed,
                      long air_assist, long exhaust, long intake,
                      const cool_limits_t *lim)
{
    int m;
    if (!strcmp(mode, "idle"))
        m = 0;
    else if (!strcmp(mode, "run"))
        m = 1;
    else if (!strcmp(mode, "cooldown"))
        m = 2;
    else
        return -1;

    pthread_mutex_lock(&mu);
    rep_mode = m;
    rep_armed = !!armed;
    rep_duty[0] = air_assist >= 0 && air_assist <= 1023 ? air_assist : -1;
    rep_duty[1] = exhaust >= 0 && exhaust <= 65535 ? exhaust : -1;
    rep_duty[2] = intake >= 0 && intake <= 65535 ? intake : -1;
    if (lim)
        rep_lim = *lim;
    else {
        cool_limits_t none = {-1, -1, -1, -1, -1};
        rep_lim = none;
    }
    rep_at = wall_s();
    pthread_mutex_unlock(&mu);
    return 0;
}

double cool_report_age(void)
{
    pthread_mutex_lock(&mu);
    double age = rep_at < 0 ? -1.0 : wall_s() - rep_at;
    pthread_mutex_unlock(&mu);
    return age;
}

void cool_controller_stopped(void)
{
    pthread_mutex_lock(&mu);
    rep_at = -1.0;
    rep_mode = 0;
    rep_armed = 0;
    pthread_mutex_unlock(&mu);
}

int cool_status_json(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    double age = rep_at < 0 ? -1 : wall_s() - rep_at;
    coolfmt_status_t st = {
        .phase = pub_phase, .verdict = pub_verdict, .reason = pub_reason,
        .fire_watch = pub_fire_watch, .accel_watch = pub_accel_watch,
        .quiet_hold = quiet_held,
        .fire_ok = pub_fire_ok, .hold = pub_hold,
        .armed = coolfmt_armed(rep_armed, age, REPORT_TIMEOUT_S),
        .down_c = pub_down, .up_c = pub_up,
        .report_age_s = age,
        .gates_off = pub_gates_off, .limits = pub_limits,
        .fan_gates = pub_fan_gates,
        .attn_standing = attn_latched, .attn_live = attn_live,
        .attn_verdict = attn_verdict, .attn_reason = attn_reason, .attn_at = attn_at,
        .attn_cleared_at = attn_cleared_at, .notices = pub_notices,
    };
    int rc = coolfmt_status(buf, len, &st);
    pthread_mutex_unlock(&mu);
    return rc;
}

int cool_armed(void)
{
    pthread_mutex_lock(&mu);
    double age = rep_at < 0 ? -1 : wall_s() - rep_at;
    int armed = coolfmt_armed(rep_armed, age, REPORT_TIMEOUT_S);
    pthread_mutex_unlock(&mu);
    return armed;
}

int cool_gates_off_json(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    snprintf(buf, len, "%s", pub_gates_off);
    pthread_mutex_unlock(&mu);
    return 0;
}

void cool_view(cool_view_t *v)
{
    pthread_mutex_lock(&mu);
    double age = rep_at < 0 ? -1 : wall_s() - rep_at;
    snprintf(v->phase, sizeof(v->phase), "%s", pub_phase);
    snprintf(v->verdict, sizeof(v->verdict), "%s", pub_verdict);
    snprintf(v->reason, sizeof(v->reason), "%s", pub_reason);
    v->hold = pub_hold;
    v->fire_ok = pub_fire_ok;
    v->armed = coolfmt_armed(rep_armed, age, REPORT_TIMEOUT_S);
    pthread_mutex_unlock(&mu);
}

double cool_coolant_c(long raw)
{
    /* read_temp's conversion, the air-assist shift and the per-machine
     * correction included. */
    long raw_c = raw - cool_coolant_offset_counts();
    return coolant_degc(raw_c > 0 ? raw_c : raw) + (double)temp_offset_c;
}

double cool_tick_max_ms(void)
{
    pthread_mutex_lock(&mu);
    double v = tick_max_ms;
    tick_max_ms = NAN;
    pthread_mutex_unlock(&mu);
    return v;
}

void cool_settings_in_force(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    snprintf(buf, len, "%s", pub_inforce);
    pthread_mutex_unlock(&mu);
}
