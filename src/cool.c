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
 *   noise cannot chatter the part. The pump and the TEC are written
 *   every tick, so a write that did not take is put right at the next;
 *   the kernel holds the TEC on only for its lease, which those writes
 *   renew, so a TEC the engine stopped writing goes off by itself.
 * - COLD / WARM-UP: the coolant floor (cool_temp_min) is a fire gate
 *   under the ceiling with a degree of hysteresis; the warm-up gate
 *   (cool_temp_start) holds a session that opens under it with the
 *   loop heater on and the fans idle, releases once the bulk reading
 *   (a one-minute rolling minimum upstream, blind to the heater's
 *   slugs passing the sensor) reaches the gate, then
 *   brings the run fans up. Either at its off end (0) is no gate.
 *   A warm-up that stops making progress (the heater's plateau in a
 *   cold room) is named once and keeps holding: no release under the
 *   gate. A session that opens with no upstream reading to decide on
 *   holds WARMUP undecided, heater off and the window unacknowledged,
 *   until the first reading decides.
 * - SENSOR: a coolant sensor with no reading - a read that fails, or a
 *   count that is no temperature (coolant.h: 0, 1023 or more, under 0 C,
 *   judged before the corrections) - on two ticks in a row, on two ticks
 *   with one good tick between, or on six ticks within 60 s, and a
 *   reading over 70 C (a short reads hundreds of degrees) in its own
 *   tick, whatever the gates are set to, blocks fire
 *   and holds, heater off, until three or four good ticks in a row
 *   (SENSOR_FAIL_*, SENSOR_WINDOW_*); so does a lid
 *   IR channel unreadable for two ticks in the fire watch's phases. A
 *   short in a run session (a reading over 70 C, or a count of 0) is
 *   SENSOR's fail tier: motion stopped and the latch locked in that tick,
 *   the controller ended through the supervisor as at FIRE and CRASH,
 *   SENSOR standing until the job has ended.
 * - OVER-TEMP: if the upstream temp exceeds the run ceiling the
 *   verdict goes OVERTEMP with hold=true; an idle or thermal phase runs
 *   the cooldown airflow for as long as the gate stands (a run or smoke
 *   phase keeps its higher run duties), and no diagnostic or pump hold
 *   is taken under it. It recovers (resume_ok) once the temp is back
 *   under the resume gate. The upstream sensor gates because it reads
 *   the coolant entering the tube.
 * - GATE SETTINGS (gates.c): the ceiling, its resume gate and the
 *   other limits are plain settings with a wide legal range, a
 *   recommended band and an off end; a ceiling at its top turns that
 *   gate off by value. An off gate is skipped, still measured, logged
 *   at every run start, and published as gates_off.
 * - AIRFLOW GATES (airflow.c): while the run profile is applied, each
 *   fan is held to a floor (the exhaust, intakes and air assist by
 *   tachometer, the purge fan by current) after a spin-up grace;
 *   three consecutive ticks under the floor trip a fault for the
 *   rest of the run session (verdict AIRFLOW: motion stopped and the
 *   latch locked at the trip, hold, fire blocked, no resume; the
 *   controller takes the verdict on its own terms), the fans held at
 *   run duty. A header's tach window can raise a floor for a job, never
 *   lower it. A machine built with one
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
 * - The engine's laser latch locks (the fail tiers, a silence, emission
 *   with no armed window) go through its own holder in the kernel,
 *   cnc/laser_latch_cooling, which no controller unlock undoes. The
 *   engine writes the release on every tick on which no reason stands,
 *   before that tick's verdict; the release leaves the latch locked until
 *   a controller unlocks it again (cool_latch_lock).
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
#include "coolant.h"
#include "setup.h"
#include "coolfmt.h"
#include "fflog.h"
#include "flowrun.h"
#include "gates.h"
#include "holds.h"
#include "led.h"
#include "paths.h"
#include "senslog.h"
#include "settings.h"
#include "status.h"
#include "super.h"
#include "testhook.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
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
/* The verdict's directory: the run directory (paths.h), which a host-test
 * run moves with GF_RUN_DIR as it moves every runtime file; a unit test's
 * build names its own. */
#ifdef VERDICT_DIR
static const char *verdict_dir(void) { return VERDICT_DIR; }
#else
static const char *verdict_dir(void) { return ff_run_dir(); }
#endif
static void verdict_path(char *buf, size_t len, const char *name)
{
    snprintf(buf, len, "%s/%s", verdict_dir(), name);
}

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
/* A coolant sensor with no reading (a read that fails, or a count that is
 * no temperature: coolant.h) leaves every coolant gate blind for that
 * tick: the gates keep their state and the sample is skipped. Such a tick
 * adds SENSOR_FAIL_UP to a score that every tick with both sensors reading
 * takes one from; the score is held to SENSOR_FAIL_CAP. SENSOR rises at
 * SENSOR_FAIL_SCORE (two no-reading ticks in a row, or two with one good
 * tick between: a sensor that drops out every other tick) and is released
 * once the score is back to 0 (three or four good ticks in a row). One
 * dropout tick alone (a read that fails, or a count that is no
 * temperature) is tolerated, and said at most once a minute with the count
 * of the minute's tolerated ticks. SENSOR_WINDOW_N tolerated ticks within
 * SENSOR_WINDOW_S raise SENSOR too, with the score set to its cap, so a
 * sensor failing at a lasting rate is noticed; the same score rule
 * releases it. A reading over COOLANT_READING_MAX_C is never tolerated:
 * it sets the score to its cap in its own tick. */
#define SENSOR_FAIL_UP         2
#define SENSOR_FAIL_SCORE      3
#define SENSOR_FAIL_CAP        4
#define SENSOR_TOLERATED_SAY_S 60.0     /* a tolerated tick is said at most this often */
#define SENSOR_WINDOW_N        6
#define SENSOR_WINDOW_S        60.0
/* The highest coolant reading the engine takes as a temperature: the
 * critical line's highest setting, its off end (gates.c). A shorted or
 * near-shorted thermistor reads a few counts over 0, hundreds of degrees,
 * and with the floor, the ceiling and the critical line at their off ends
 * no gate would judge it. So a reading over this, on the count's own
 * conversion or after the corrections, is no reading (read_temp), and a
 * short: SENSOR in its own tick, whatever the gates are set to, and in a
 * run session SENSOR's fail tier (short_alarm), as a count of 0 is there. */
#define COOLANT_READING_MAX_C 70.0f
/* The lid IR fire watch with a channel unreadable (a read that fails, or
 * a count past the converter's 1023): this many judged ticks in a row is
 * the SENSOR verdict, released once all four read again. */
#define IR_BLIND_TICKS         2
/* cnc/state unreadable counts as running (the ring may be playing) for
 * this long, then as not running, said once: a module that has gone plays
 * nothing, and the safing writes behind it fail as its reads do. */
#define CNC_UNREADABLE_S       5.0
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

/* The emission witness: the kernel's count of the gated laser output over
 * its ~1 s window. A count above 0 with no armed report in the last
 * EMISSION_WINDOW_TOL_S (the sample window's lag and a job's tail) is
 * EMISSION. A count that cannot be read for EMISSION_BLIND_TICKS ticks in a
 * row is named once, and outside that tolerance it gets the kernel pair
 * once per stretch. */
#define EMISSION_WINDOW_TOL_S 3.0
#define EMISSION_BLIND_TICKS  3

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
/* The armed window's identity: the time its opening was reported, in ms on
 * CLOCK_MONOTONIC, greater than every one before it in this boot (a report
 * that opens a window after a closed or a stale one is an opening). 0 =
 * none yet. GET /cool/status shows it with armed, and the extension host
 * names in its status file the window it has paused its packages for. */
static unsigned long long rep_window = 0;

/* Published snapshot for cool_status_json. */
static char pub_verdict[COOL_VERDICT_MAX] = "OK";
static char pub_reason[COOL_REASON_MAX];
static int pub_fire_ok, pub_hold;
static int pub_armed_ack;           /* the acknowledgment as published */
static int pub_ack_wait, pub_reserved;  /* the document's ack_wait and button_reserved, as published */
/* One writer of the verdict file at a time: the engine's publish, and the
 * report route's withdrawal of the acknowledgment. Taken before mu. */
static pthread_mutex_t pub_mu = PTHREAD_MUTEX_INITIALIZER;
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
static double tec_switched_at = 0;
static int pump_write_failing = 0;  /* a pump write failed; named once until one takes */
static int tec_write_failing = 0;   /* a TEC write failed; named once until one takes */
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
/* The quiet hold as the engine's tick last applied it (quiet_apply, its
 * fans; quiet_pump_settle, its pump): the hold's callers set the two flags
 * above, and the posture is written on the engine thread alone.
 * Engine-thread only. */
static int quiet_applied = 0;
static int quiet_pump_applied = 0;
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
static float warmup_open_up = 0;        /* the upstream reading the warm-up began on */
static int warmup_heat_said = -1;       /* the heater as the warm-up said it: -1 not said yet */
static int warmup_saw_sensor = 0;       /* a SENSOR stood over the warm-up since it was last said */
/* The release judges the mixed bulk, not the heater's slug passing the
 * sensor: the minimum of the upstream reading over the last WARMUP_WIN
 * seconds (between slugs the reading falls back to the bulk). */
static float warmup_win[WARMUP_WIN];
static uint32_t warmup_win_n = 0;
static int critical_alarm = 0;      /* latched coolant fault, this run session */
static uint32_t sensor_fail_score = 0;  /* the no-reading score (SENSOR_FAIL_*) */
static uint32_t sensor_fail_ticks = 0;  /* no-reading ticks since the score was last 0 */
static double sensor_tolerated_said = -1e9;
static double sensor_tolerated_at[SENSOR_WINDOW_N];    /* the last tolerated ticks, a ring */
static unsigned sensor_tolerated_n = 0;                  /* tolerated ticks ever (the ring's head) */
static int sensor_alarm = 0;        /* SENSOR: blind on the coolant, fire blocked, hold */
/* SENSOR's fail tier: a coolant short in a run session, a reading over
 * COOLANT_READING_MAX_C or a count of 0 on either sensor. The kernel pair
 * is written in that tick, the controller is ended through the
 * supervisor (cool_fail_tier_stop) as at FIRE and CRASH, and SENSOR, its
 * hold and the engine's latch hold stand, past the session's end, until
 * the job has ended (the controller reports idle with no window, or no
 * controller remains, as after that stop), whatever the readings do
 * meanwhile. */
static int short_alarm = 0;
/* A session opened with the warm-up gate on and no upstream reading the
 * cold side takes: WARMUP with the heater off and the armed window not
 * acknowledged, until the first reading decides between the warm-up and
 * the run. */
static int warmup_pending = 0;
static long throttle_state = 0;     /* the kernel's CPU cooling state last seen */
static int critical_would_warned = 0;   /* off gate, would have tripped: once */
static int forced_cool = 0;         /* over-temp's cooling airflow stands in idle or thermal */
static int pub_cooling_forced = 0;  /* over_temp_gate || forced_cool, as the tick last published it (under mu) */
static int pub_session = 0;         /* a run session open, published at its edge before the engine opens it (under mu) */
static int flood_on = 0;            /* effective run window */
static int silent_warned = 0;
static int silent_safed = 0;        /* hang dead-man fired this episode */

void (*cool_fail_tier_stop)(const char *why) = NULL;
int (*cool_button_reserved)(char *why, size_t len) = NULL;

/* The machine's button reserved for a prompt (cool.h): asked once a tick,
 * and published as the verdict's button_reserved. */
static int button_withheld = 0;
static double button_on_file_at = -1.0; /* the first publish of this stretch; <0 = none */

/* Physical-evidence witnesses. */
static float fire_q1_alert = 275.0f;    /* the factory's header defaults */
static float fire_q1_critical = 688.0f;
static float fire_q2_alert = 374.0f;
static float fire_q2_critical = 1022.0f;
static int flame_alert = 0;         /* the pause tier's hold */
static int flame_clear_ticks = 0;
static double last_armed_at = -1.0; /* last fresh armed report seen */
/* EMISSION, the emission witness's fail tier: the gated laser output
 * counted with no armed window in the last EMISSION_WINDOW_TOL_S. It stands
 * while the count reads above 0, and the operator's acknowledgment carries
 * it from there (attention_tick). */
static int emission_alarm = 0;
static int emission_blind_ticks = 0;    /* ticks in a row the count could not be read */
static int emission_blind_said = 0;     /* the unreadable stretch named */
static int emission_blind_safed = 0;    /* its pair written */
static int pgood_warned = 0;        /* once per run session */
static long last_faults = 0;
static int ir_over_ticks = 0;
static int fire_alarm = 0;          /* latched lid-IR fire signal */
static int ir_blind_ticks = 0;      /* judged ticks in a row with a lid IR channel unreadable */
static int ir_blind_alarm = 0;      /* SENSOR: a lid IR channel unreadable, fire blocked, hold */
static char ir_blind_reason[COOL_REASON_MAX];
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
static int crash_hw_state = 0;      /* 0 untried this session, 1 armed, -1 off for the session */
static int crash_hw_armed = 0;
static int crash_poll_errs = 0;     /* consecutive; 3 stands the watch down */
static const char *pub_accel_watch = "watch";
static long run_duty[3] = {-1, -1, -1};
static long cmd_duty[3] = {-1, -1, -1};   /* what fans_run last wrote */
static int eff_armed = 0;                 /* the armed window, as reported */
static unsigned long long eff_window = 0; /* its identity (rep_window), as the tick last took it */
static unsigned long long tick_window = 0; /* rep_window at this tick's snapshot of the report */
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
/* The airflow behind the armed window's acknowledgment. Up on a tick on
 * which every judged fan reads at or above its floor, so the first fire
 * waits for the airflow. A warm-up judges no fan, so there it is up with
 * no fan's word, and WARMUP keeps fire blocked until the release drops
 * it and the run's fans are judged. It is the window's own: dropped at
 * the session's opening, at the window's opening and at the warm-up's
 * release, so it is never carried into a window the fans have not met.
 * Withdrawn only once a
 * judged fan has read under its floor for AIRFLOW_DEBOUNCE_TICKS ticks in
 * a row, the count the airflow gate trips on, so one low tach reading
 * does not take it. */
static int fans_up = 0;
static int fans_low_ticks = 0;       /* ticks in a row with a judged fan under, while up */
static int fans_clean_ticks = 0;     /* ticks in a row with every judged fan at its floor (to the count) */
static double fans_up_at = -1.0;     /* when they first were, this session */
/* The armed window owes its fans: it has been acknowledged in this run, or
 * it was already open when this engine first heard from the controller (a
 * restart of the daemon under a busy job), so it may have fired. While it
 * owes them, a judged fan that takes the acknowledgment away (FANS) blocks
 * fire and holds the window, grace or not. A window this engine saw open
 * waits for its acknowledgment unheld: its controller fires nothing before
 * it. */
static int window_owes_fans = 0;
static int window_unarmed_seen = 0;  /* a fresh report with no window, this engine's life */
static int fans_held = 0;            /* FANS, this tick */
static char fans_low_name[24];       /* the judged fan last read under its floor */
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

/* A sysfs store replaces the attribute's value; O_TRUNC, which sysfs
 * ignores, makes a plain-file tree (the host tests') hold the value
 * written and no tail of a longer one. */
static int wr_attr(const char *attr, const char *val)
{
    char path[128];
    snprintf(path, sizeof(path), "%s%s", sysfs_root(), attr);
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int ret = write(fd, val, strlen(val)) < 0 ? -1 : 0;
    close(fd);
    return ret;
}

/* The engine's own safing (stop motion, lock the laser): a write that
 * fails is named at the highest severity. The kernel refuses a stop only
 * in its fault state, where nothing plays and every run start is refused
 * (a write that cannot open the attribute finds no module to play
 * anything), and a stop refuses a run start already in flight (the
 * module's cnc_cmd_answer; pulse-feeder-contract.md, "run" and "stop").
 * So the other fail tiers (FIRE, CRASH, AIRFLOW, CRITICAL, and a short's
 * SENSOR) write their stop once, at their rising edge, and their latch
 * lock on every tick they stand (latch_hold_tick), which lands whether or
 * not the stop took; a silence with the kernel running and emission sensed
 * with no armed window (EMISSION) write the pair on every tick their
 * reason stands. */
static int stop_fault_said = 0;     /* a stop refused in the fault state, said this stretch */

/* Whether the kernel's state reads "fault" now. */
static int cnc_reads_fault(void)
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
    return strncmp(buf, "fault", 5) == 0;
}

/* A stop the kernel refuses in its fault state (EPERM), where nothing plays
 * and FIRE is parked already, is no failure: said once a stretch, below the
 * highest severity. Any other failure is named at the highest. */
static void safing_write(const char *attr, const char *val)
{
    if (wr_attr(attr, val) == 0) {
        if (!strcmp(attr, "cnc/stop"))
            stop_fault_said = 0;
        return;
    }
    int err = errno;
    if (err == EPERM && !strcmp(attr, "cnc/stop") && cnc_reads_fault()) {
        if (!stop_fault_said) {
            stop_fault_said = 1;
            fflog(LOG_INFO, "cool: the kernel is in its fault state: no stop to make");
        }
        return;
    }
    fflog(LOG_CRIT, "cool: safing write %s=%s failed: %s", attr, val, strerror(err));
}

/* The engine's own hold on the laser latch: the kernel's cooling holder,
 * apart from the controller's cnc/laser_latch. Its lock takes the
 * controller's hold with it, no controller unlock is taken while it
 * stands, and only the engine releases it, which never unlocks the latch
 * by itself: a controller's next unlock does. The engine holds it while
 * a reason it locked for stands (latch_hold_tick names them), and writes the release on every tick on which
 * none stands: the kernel also sets the holder at every final close of
 * the pulse device, which can come at any time (an unmanaged controller,
 * or a homing runner that outlived a daemon restart, closing the device
 * later), and a release
 * changes nothing where the engine does not hold. */
#define COOL_LATCH_ATTR "cnc/laser_latch_cooling"
static int cool_latch_release_named = 0;

/* The engine's lock, then the controller's hold as well. With the cooling
 * holder in the kernel the second write changes nothing (the cooling lock
 * has locked the controller's hold already); on a module without that
 * holder it is the lock that lands. It comes second, so a controller
 * unlock that falls between the two is refused, never taken. */
static void cool_latch_lock(void)
{
    safing_write(COOL_LATCH_ATTR, "1");
    safing_write("cnc/laser_latch", "1");
}

/* The release, on every tick on which no reason stands: called before the
 * tick's verdict is published, so the verdict that permits fire again
 * never has the engine's hold behind it. A release that does not take
 * leaves the hold where it was (every controller unlock is refused while
 * it stands), is written again the next tick, and is named once until
 * one takes. */
static void cool_latch_release_tick(int wanted)
{
    if (wanted)
        return;
    if (wr_attr(COOL_LATCH_ATTR, "0") == 0) {
        cool_latch_release_named = 0;
        return;
    }
    if (!cool_latch_release_named) {
        cool_latch_release_named = 1;
        fflog(LOG_ERR, "cool: the cooling holder of the laser latch could not be released: %s",
              strerror(errno));
    }
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

/* A coolant reading: 1 with *c in degrees, 0 for none. None is a read
 * that fails, a count that is no temperature (coolant.h: 0, 1023 or more,
 * or under 0 C), judged on the count as the sensor gave it, and a reading
 * over COOLANT_READING_MAX_C, on the count's own conversion or after the
 * corrections (either can move a near-short across the line). *rawp, when
 * given, gets the count read (-1 for a read that failed); *overp, when
 * given, is SHORT_OVER for a reading over the line, SHORT_ZERO for a count
 * of 0, and 0 otherwise. */
#define SHORT_OVER 1
#define SHORT_ZERO 2
static int read_temp(const char *attr, float *c, long *rawp, int *overp)
{
    long raw = rd_long(attr);
    if (rawp)
        *rawp = raw;
    if (overp)
        *overp = 0;
    if (raw < 0 || coolant_no_reading(raw)) {
        /* A count of 0 is the shorted thermistor itself (coolant.h): a
         * dropout at idle, a short in a run session (SHORT_ZERO). */
        if (overp && raw == 0)
            *overp = SHORT_ZERO;
        return 0;
    }
    /* The air-assist ground shift lifts the raw counts (more counts read
     * colder): take them off before the conversion. */
    long raw_c = raw - cool_coolant_offset_counts();
    /* The per-machine correction from the settings file. */
    float t = (float)coolant_degc(raw_c > 0 ? raw_c : raw) + temp_offset_c;
    if (coolant_degc(raw) > COOLANT_READING_MAX_C || t > COOLANT_READING_MAX_C) {
        if (overp)
            *overp = SHORT_OVER;
        return 0;
    }
    *c = t;
    return 1;
}

/* cnc/state == "running" is the one state in which the machine can be
 * depositing energy (the ring is being clocked). It feeds protective
 * actions (the hang dead-man's running case, the cooldown's airflow), so
 * a state that cannot be read counts as running, for CNC_UNREADABLE_S
 * from the first failed read; past that it counts as not running, said
 * once, until it reads again. The state is read once at the top of every
 * tick (cnc_state_sample), so the bound runs on wall time, and every
 * caller in the tick gets that sample (cnc_is_running). */
static double cnc_unreadable_since = -1.0;
static int cnc_unreadable_named = 0;
static int cnc_running = 0;             /* this tick's sample */

static void cnc_state_sample(double now)
{
    char path[128], buf[16];
    snprintf(path, sizeof(path), "%scnc/state", sysfs_root());
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = -1;
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
    }
    if (n <= 0) {
        if (cnc_unreadable_since < 0.0)
            cnc_unreadable_since = now;
        cnc_running = now - cnc_unreadable_since < CNC_UNREADABLE_S;
        if (!cnc_running && !cnc_unreadable_named) {
            cnc_unreadable_named = 1;
            fflog(LOG_WARNING, "cool: cnc/state unreadable for %.0f s: taken as not running",
                  now - cnc_unreadable_since);
        }
        return;
    }
    cnc_unreadable_since = -1.0;
    cnc_unreadable_named = 0;
    buf[n] = '\0';
    cnc_running = strncmp(buf, "running", 7) == 0;
}

static int cnc_is_running(void)
{
    return cnc_running;
}

static uint32_t heater_pct = 0;     /* the duty last commanded */

static void heater_set_pct(uint32_t pct)
{
    heater_pct = pct;
    wr_attr_long("thermal/heater_pwm", (long)pct * 65535 / 100);
}

/* An on/off output the engine holds every tick: written, and a write that
 * fails is named once, until one takes again (*failing). */
static void hw_assert(const char *attr, int on, int *failing)
{
    if (wr_attr(attr, on ? "1" : "0") == 0) {
        if (*failing) {
            *failing = 0;
            fflog(LOG_INFO, "cool: %s takes writes again", attr);
        }
        return;
    }
    if (!*failing) {
        *failing = 1;
        fflog(LOG_ERR, "cool: writing %s=%d failed: %s; it is written again every tick",
              attr, on ? 1 : 0, strerror(errno));
    }
}

/* The three fans the engine drives, every write to them read back. The
 * head's air-assist register and the two thermal PWMs report the duty in
 * force (a thermal PWM the last duty its device took: a refused write
 * fails and leaves it), so a write the bus dropped, or a value the device did not take,
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

/* The duties in force, read back once a tick: a value the head lost since
 * it was written (its MCU reset, a dropped transaction) is put back and
 * named, so the air assist is never judged against a duty it was not
 * given. The two thermal PWMs read back the duty their PWM last took, the
 * module's record of it, so this catches a write that never took but
 * cannot see a duty their hardware lost after taking it: the tach floors
 * judge those two fans. A fan whose duty was raised back gets its spin-up
 * grace again, up to a few times a session: a device that keeps losing its
 * value is judged at the tach like any other, not excused forever.
 * fan_back is what each read this tick, for the fault line. */
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

static void fans_idle(void)
{
    if (quiet_applied) {
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
        quiet_applied == was_quiet && eff_armed == was_armed)
        return;
    was[0] = cmd_duty[0];
    was[1] = cmd_duty[1];
    was[2] = cmd_duty[2];
    was_quiet = quiet_applied;
    was_armed = eff_armed;
    fflog(LOG_INFO, "cool: fan duty: air assist %ld (machine %d, job %ld), exhaust %ld "
          "(machine %d, job %ld), intake %ld (machine %d, job %ld); armed %d%s",
          cmd_duty[0], AIR_ASSIST_RUN, run_duty[0],
          cmd_duty[1], EXHAUST_RUN, run_duty[1],
          cmd_duty[2], INTAKE_RUN, run_duty[2], eff_armed,
          quiet_applied ? "; the quiet hold stands, the fans are held off instead" : "");
}

static void fans_run(void)
{
    cmd_duty[0] = airflow_run_duty(AIR_ASSIST_RUN, run_duty[0], eff_armed);
    cmd_duty[1] = airflow_run_duty(EXHAUST_RUN, run_duty[1], eff_armed);
    cmd_duty[2] = airflow_run_duty(INTAKE_RUN, run_duty[2], eff_armed);
    fans_log_duty();
    if (quiet_applied) {
        fans_quiet();
        return;
    }
    aa_write(cmd_duty[0]);
    fan_write(Fx_Exhaust, cmd_duty[1]);
    fan_write(Fx_Intake, cmd_duty[2]);
}

static void fans_cool(void)
{
    if (quiet_applied) {
        fans_quiet();
        return;
    }
    aa_write(AIR_ASSIST_IDLE);
    fan_write(Fx_Exhaust, EXHAUST_COOL);
    fan_write(Fx_Intake, INTAKE_COOL);
}

/* The fan profile the current phase calls for. OVERTEMP's cooling airflow
 * is a state, not an edge: while the ceiling's gate stands, an idle or a
 * thermal phase runs the cooldown duties and says so (forced_cool), so a
 * phase that ends into idle under the gate, a quiet hold's release and the
 * busy start's end all land on the cooling airflow, and the gate's
 * recovery takes it back. A run or a smoke phase keeps its run duties,
 * which are higher. */
static void fans_apply_phase(void)
{
    forced_cool = over_temp_gate && (cool_state == Cool_Idle || cool_state == Cool_Thermal);
    if (forced_cool) {
        fans_cool();
        return;
    }
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
static int ext_enabled_last = -1;       /* ext_enabled as last read; -1 never read */
static int ext_unreadable_named;

/* A settings file that cannot be read is not "extensions off": the value
 * last read stands, so a hold a package asked for keeps standing through
 * a failed read; before the first read (a daemon start) the hold files
 * are honored. An absent key is off. */
static void ext_holds_read(double now, holds_t *h)
{
    char v[8];
    memset(h, 0, sizeof(*h));
    int rc = settings_get("ext_enabled", v, sizeof(v));
    if (rc == SETTINGS_UNREADABLE) {
        if (!ext_unreadable_named) {
            ext_unreadable_named = 1;
            fflog(LOG_WARNING, "cool: the settings file could not be read; %s",
                  ext_enabled_last < 0 ? "the extension holds stand until it reads"
                  : ext_enabled_last ? "extensions stay on" : "extensions stay off");
        }
    } else {
        ext_unreadable_named = 0;
        ext_enabled_last = rc == 0 && strcmp(v, "1") == 0;
    }
    if (access(ext_safe_file, F_OK) == 0 || !ext_enabled_last)
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

/* ------------------------------------ the packages, for the armed window */

/* The extension host freezes every package's service for an armed window,
 * or holds one the operator lets run through a job to EXT_JOB_CPU_PCT of
 * the core, each in a group of its own under EXT_CG_DIR. It learns of the
 * window at its next turn, which can come after a window's first fire
 * could, and a turn that read the window closed can still start or thaw a
 * service after the window is reported. So the window's acknowledgment
 * also waits for two words, read once a tick while a window is reported:
 * the host's, that it has paused every service for this window (or runs
 * none and will start none before it says otherwise), and the kernel's,
 * that every group is still. A group is still when it holds no process,
 * when the kernel reads it frozen, or when its cpu.max is at most the
 * job's share. No directory of groups is no package. Anything else that
 * does not read is not still: the acknowledgment waits, and the first fire
 * with it. */
#ifndef EXT_CG_DIR                      /* the unit test points it at a fake tree */
#define EXT_CG_DIR      "/sys/fs/cgroup/ffx"
#endif
#define EXT_JOB_CPU_PCT 3               /* forgeext's RUN_JOB_CPU_PCT */
#define EXT_WAIT_WORDS  "PACKAGES: "    /* the reason's mark while the packages keep the window waiting */

static const char *ext_cg_dir = EXT_CG_DIR;

/* The host's status file: extpkg.h's EXTPKG_STATUS_FILE, in the run
 * directory a host-test run moves; the unit test names its own. */
#ifdef EXT_HOST_STATUS
static const char *ext_host_status(void) { return EXT_HOST_STATUS; }
#else
static const char *ext_host_status(void)
{
    static char p[256];
    snprintf(p, sizeof(p), "%s/ext/status.json", ff_run_dir());
    return p;
}
#endif

static int ext_unfrozen;                /* the packages kept the window waiting at the last tick with a window reported */
static char ext_wait_words[COOL_REASON_MAX];    /* why, as the verdict's reason says it */

/* The host's word on the window: 1 when no turn of the host can still
 * start or thaw a service for want of having seen this window (win, its
 * identity); 0 with the reason's words in why. The status file is written
 * whole at the end of a turn, and only by a running host.
 * - No file: no host has written one since the machine started, and a host
 *   writes its word before it starts a service.
 * - Its writer gone: the respawn wrapper kills every group with it, and a
 *   host that starts writes its word before it starts a service.
 * - "quiet" true: no service runs, and the host starts none before its
 *   file says otherwise.
 * - "window" equal to win: it has frozen or held every service for this
 *   window, and no turn since has started or thawed one.
 * Anything else, a file that does not read included, keeps the window
 * waiting. A file with no "quiet" was written by a host older than this
 * engine, which gives no word on a window: it waits, with words of its own. */
static int ext_host_word(unsigned long long win, char *why, size_t wlen)
{
    int fd = open(ext_host_status(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT)
            return 1;
        snprintf(why, wlen, EXT_WAIT_WORDS "the extension host's status does not read (%s)", strerror(errno));
        return 0;
    }
    json_error_t jerr;
    json_t *j = json_loadfd(fd, 0, &jerr);
    close(fd);
    json_t *pid = json_object_get(j, "pid"), *w = json_object_get(j, "window"), *q = json_object_get(j, "quiet");
    json_int_t p = json_is_integer(pid) ? json_integer_value(pid) : 0;
    int ok = 0;
    if (!json_is_object(j) || p <= 1 || p > INT32_MAX)
        snprintf(why, wlen, EXT_WAIT_WORDS "the extension host's status does not read");
    else if (!json_is_boolean(q))
        snprintf(why, wlen, EXT_WAIT_WORDS "the extension host is older than this firmware");
    else if (kill((pid_t)p, 0) != 0 && errno == ESRCH)
        ok = 1;
    else if (json_is_true(q))
        ok = 1;
    else if (win > 0 && json_is_integer(w) && json_integer_value(w) > 0 &&
             (unsigned long long)json_integer_value(w) == win)
        ok = 1;
    else
        snprintf(why, wlen, EXT_WAIT_WORDS "waiting for the extension host to pause them for this window");
    json_decref(j);
    return ok;
}

/* A group's file, whole: 0, or -1 with errno. */
static int ext_cg_read(const char *dir, const char *grp, const char *file, char *buf, size_t len)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s/%s", dir, grp, file);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, len - 1);
    int e = errno;
    close(fd);
    if (n < 0) {
        errno = e;
        return -1;
    }
    buf[n] = '\0';
    return 0;
}

/* The value of key in a flat-keyed text ("populated 1\nfrozen 0\n"), or -1. */
static long long ext_cg_key(const char *text, const char *key)
{
    size_t klen = strlen(key);
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
        if (!strncmp(l, key, klen) && l[klen] == ' ')
            return atoll(l + klen + 1);
    return -1;
}

/* One group: 1 still, 0 not, -1 gone. */
static int ext_group_still(const char *dir, const char *grp)
{
    char text[256];
    if (ext_cg_read(dir, grp, "cgroup.events", text, sizeof(text)) != 0)
        return errno == ENOENT ? -1 : 0;
    long long populated = ext_cg_key(text, "populated");
    if (populated == 0)
        return 1;
    if (populated != 1)
        return 0;
    if (ext_cg_key(text, "frozen") == 1)
        return 1;
    long long quota, period;
    if (ext_cg_read(dir, grp, "cpu.max", text, sizeof(text)) != 0)
        return 0;
    if (sscanf(text, "%lld %lld", &quota, &period) != 2 || quota < 0 || period <= 0)
        return 0;                       /* "max": no limit */
    return quota * 100 <= (long long)EXT_JOB_CPU_PCT * period;
}

/* 1 when every package's group is still; 0 with the reason's words in
 * who, naming the first group that is not. */
static int ext_packages_still(const char *dir, char *who, size_t wlen)
{
    who[0] = '\0';
    DIR *d = opendir(dir);
    if (!d) {
        if (errno == ENOENT)
            return 1;
        snprintf(who, wlen, EXT_WAIT_WORDS "their groups do not read (%s)", strerror(errno));
        return 0;
    }
    int still = 1;
    struct dirent *e;
    for (errno = 0; still && (e = readdir(d)); errno = 0) {
        struct stat sb;
        char p[512];
        int g;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (stat(p, &sb) != 0)
            g = errno == ENOENT ? -1 : 0;   /* gone, or not readable */
        else if (!S_ISDIR(sb.st_mode))
            continue;                   /* one of the parent's own files */
        else
            g = ext_group_still(dir, e->d_name);
        if (g == 0) {
            /* a package's id is at most 63 */
            snprintf(who, wlen, EXT_WAIT_WORDS "waiting for %.63s to pause for the job", e->d_name);
            still = 0;
        }
    }
    if (still && errno != 0) {
        snprintf(who, wlen, EXT_WAIT_WORDS "their groups do not read (%s)", strerror(errno));
        still = 0;
    }
    closedir(d);
    return still;
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
        int rc = settings_get(tunables[i].key, v, sizeof(v));
        const gate_setting_t *g = gate_setting_find(tunables[i].key);
        double gv;
        /* A value is taken whole or not at all: one longer than the read
         * holds is never read as its prefix. The default stands, and the
         * line says so at every run start. */
        if (rc == SETTINGS_TOO_LONG) {
            fflog(LOG_WARNING, "cool: %s is longer than %zu characters; the default %g stands",
                  tunables[i].key, sizeof(v) - 1, (double)tunables[i].def);
        } else if (rc == 0 && g) {
            /* A gate setting is read by the parse the settings route
             * validates with (gates.c), so the engine, the route and the
             * panel read one value: text it refuses (too long, out of the
             * legal range, not a finite number) leaves the default. */
            if (gate_parse(g, v, &gv))
                f = (float)gv;
            else
                fflog(LOG_WARNING, "cool: %s = %s is not a value the setting takes; the default %g stands",
                      tunables[i].key, v, (double)tunables[i].def);
        } else if (rc == 0) {
            float parsed = strtof(v, &end);
            /* "nan" and "inf" parse as numbers. Every comparison against a
             * NaN is false, so a limit set to one never trips and never
             * reads as off: the default stands instead, and the line says
             * so at every run start. The coolant correction holds to the
             * range the route takes. */
            if (end != v && *end == '\0' && !isfinite(parsed))
                fflog(LOG_WARNING, "cool: %s = %s is not a finite number; the default %g stands",
                      tunables[i].key, v, (double)tunables[i].def);
            else if (end != v && *end == '\0' && tunables[i].f == &temp_offset_c &&
                     fabsf(parsed) > (float)COOL_TEMP_OFFSET_MAX_C)
                fflog(LOG_WARNING, "cool: %s = %s is outside %g to %g; the default %g stands",
                      tunables[i].key, v, -COOL_TEMP_OFFSET_MAX_C, COOL_TEMP_OFFSET_MAX_C,
                      (double)tunables[i].def);
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

/* Never while OVERTEMP's cooling airflow stands (pub_cooling_forced, -2,
 * said to the operator as the cooldown it is): the tools would take the
 * fans off a loop above its ceiling. */
/* Under mu. */
static int diag_refusal(void)
{
    if (pub_cooling_forced)
        return -2;
    if (diag_owned || cool_state != Cool_Idle || strcmp(pub_phase, "idle"))
        return -1;
    return 0;
}

int cool_diag_can_take(void)
{
    pthread_mutex_lock(&mu);
    int rc = diag_refusal();
    pthread_mutex_unlock(&mu);
    return rc;
}

int cool_diag_take(void)
{
    pthread_mutex_lock(&mu);
    int rc = diag_refusal();
    if (rc != 0) {
        pthread_mutex_unlock(&mu);
        return rc;
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

/* The fans to the phase's posture (fans_apply_phase): the idle duties, or
 * OVERTEMP's cooling airflow where a gate rose while the tools held the
 * hardware. */
void cool_diag_release(void)
{
    heater_set_pct(0);
    wr_attr("thermal/water_pump_on", "1");
    fans_apply_phase();
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

/* The quiet hold's flags, set with no terms: the engine's own releases
 * (on 0), and the host test's hold set past the terms. Every hold a caller
 * takes goes through cool_quiet_hold_try. The engine's next tick writes the
 * posture (quiet_apply, quiet_pump_settle), so no write of the hold's races
 * the engine's own. The heater is taken from the flow watch here, at once,
 * after the hold is published (a tick between the two carries it), so
 * nothing lets the watch take the heater back before the pump stops. A
 * hold taken again without the pump gives the pump back: the pump hold is
 * the hold's that names it. */
static void cool_quiet_hold_ex(int on, int with_pump)
{
    pthread_mutex_lock(&mu);
    quiet_held = on ? 1 : 0;
    quiet_pump_held = (on && with_pump) ? 1 : 0;
    if (on)
        quiet_since = wall_s();
    pthread_mutex_unlock(&mu);
    if (on)
        flowrun_yield("a quiet hold");
}

/* The pump hold's terms past the session's and the ceiling's, under mu:
 * the pump only from an idle phase (never a smoke clear or a thermal
 * cooldown) with no diagnostic. */
static int quiet_pump_ok_locked(void)
{
    return !diag_owned && !strcmp(pub_phase, "idle");
}

/* Every hold a caller takes (POST /cool/quiet, the lens stop finder): the
 * terms checked and the flags set in one hold of mu. The hold is taken only
 * in the idle phase with no diagnostic owning the hardware (from the
 * diagnostic's take, before its first tick publishes the phase), so it
 * never stands over the airflow of a run session (AIRFLOW's run duty, a
 * FIRE's smoke airflow), a job's smoke clear or its thermal cooldown, nor
 * under a diagnostic. A run session
 * publishes its opening under mu at its edge, before the engine opens it,
 * so a hold set before that is seen by the opening tick's late look and
 * released with the session's edge, and one asked for after it is refused:
 * no quiet hold ever stands in a run session. None is taken while
 * OVERTEMP's cooling airflow stands either: the engine releases a hold the
 * moment the ceiling's gate rises (engine_tick). */
int cool_quiet_hold_try(int on, int with_pump)
{
    pthread_mutex_lock(&mu);
    if (on && pub_session) {
        pthread_mutex_unlock(&mu);
        return -1;
    }
    if (on && pub_cooling_forced) {
        pthread_mutex_unlock(&mu);
        return -3;
    }
    if (on && (diag_owned || strcmp(pub_phase, "idle") || (with_pump && !quiet_pump_ok_locked()))) {
        pthread_mutex_unlock(&mu);
        return -2;
    }
    quiet_held = on ? 1 : 0;
    quiet_pump_held = (on && with_pump) ? 1 : 0;
    if (on)
        quiet_since = wall_s();
    pthread_mutex_unlock(&mu);
    if (on)
        flowrun_yield("a quiet hold");
    return 0;
}

/* The quiet hold's fans, on the engine thread, from the flag its callers
 * set: every fan off while it stands; on its release the purge back on and
 * the fans to the phase's profile. Its pump hold is the tick's to judge
 * later, once the phase and the gates are settled (quiet_pump_settle). */
static void quiet_apply(void)
{
    pthread_mutex_lock(&mu);
    int q = quiet_held;
    pthread_mutex_unlock(&mu);
    if (q && !quiet_applied) {
        quiet_applied = 1;
        fans_quiet();
        info("every fan off for a listening");
    }
    if (!q && quiet_applied) {
        quiet_applied = 0;
        wr_attr("head/purge_air", "1");
        fans_apply_phase();
        info("the fans back in their posture after the listening");
    }
}

int cool_quiet_hold(int on)
{
    return cool_quiet_hold_try(on, 0);
}

/* The quiet hold's pump hold, judged once in every tick, after the tick's
 * session edge, phase and gates are settled and ahead of the warm-up
 * heater's write and the tick's pump write (the pump and the TEC are
 * written every tick, engine_tick): the pump and the TEC off only in the
 * idle phase with no OVERTEMP gate standing, whatever the caller saw
 * (quiet_pump_ok_locked). Anywhere else, or the moment the hold drops it,
 * the pump is given back with a write here, before the heater can be
 * written: the tick that opens a session or raises the gate ends with the
 * pump on, and a hold that waits through a session applies by itself at
 * the idle after it. The finder keeps the pump running: it was proven
 * that way. */
static void quiet_pump_settle(void)
{
    pthread_mutex_lock(&mu);
    int qp = quiet_pump_held;
    pthread_mutex_unlock(&mu);
    int hold = qp && cool_state == Cool_Idle && !over_temp_gate;
    if (hold && !quiet_pump_applied) {
        quiet_pump_applied = 1;
        info("the pump and the TEC off too: the machine silent");
    } else if (!hold && quiet_pump_applied) {
        quiet_pump_applied = 0;
        hw_assert("thermal/water_pump_on", 1, &pump_write_failing);
        info("the pump back on after the listening");
    }
}

int cool_quiet_held(void)
{
    pthread_mutex_lock(&mu);
    int held = quiet_held;
    pthread_mutex_unlock(&mu);
    return held;
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
 * separators; the six booleans; the numbers (seq, ts_mono and the two
 * temperatures); and the two bounded strings. */
#define VERDICT_PUNCTUATION 135
#define VERDICT_BOOLEANS     30
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
 * the airflow instead of the fans catching up under the beam, and for
 * every extension package to be still for the window (frozen, held to the
 * job's share, or empty) with the host's word that it has paused them for
 * this window. It is the precondition of a window's first fire (the arm,
 * the re-arm, the cloud client before its run); inside an open window
 * fire_ok alone gates. It is the window's own: a document carries it only
 * when the window this tick judged is still the one reported, and a report
 * that closes the window or opens a new one, or a GET /cool/status that
 * shows it closed, takes it off the file at once (ack_withdraw), whatever
 * the tick.
 *
 * "button_reserved" says the machine's button is reserved for a prompt
 * that is not this window's job (cool_button_reserved): fire_ok is false,
 * the window is not acknowledged, and hold is true while a window is
 * open, so a press made for that prompt arms, resumes and fires nothing.
 * The name is BUTTON only where no other verdict stands; a controller that
 * must not take a press as its own reads this instead.
 *
 * "ack_wait" says a failure waits for the operator's acknowledgment
 * (attention_tick), whatever the verdict's name: the name is ACK only
 * where no other verdict stands, and a controller that must not ask for a
 * press it would then refuse reads this instead. It is read from the
 * attention's own state here, so every document carries it. fire_ok is
 * false whenever it is true. */
static int attn_ack_waits(void);

/* The document from the published fields, under mu: its length, as
 * snprintf gives it. */
static int verdict_body_locked(char *body, size_t len)
{
    /* To the millisecond, rounded down: a reader on this clock never
     * sees a stamp from its future. Both readers refuse one, and the
     * cloud client reads with no cache, so a refused read is a lost
     * verdict there. */
    double stamp = floor(wall_s() * 1000.0) / 1000.0;
    return snprintf(body, len,
        "{\"seq\":%lu,\"ts_mono\":%.3f,\"fire_ok\":%s,"
        "\"verdict\":\"%s\",\"hold\":%s,\"resume_ok\":%s,\"armed\":%s,"
        "\"ack_wait\":%s,\"button_reserved\":%s,"
        "\"reason\":\"%s\",\"down_c\":%.2f,\"up_c\":%.2f}\n",
        ++verdict_seq, stamp, pub_fire_ok ? "true" : "false",
        pub_verdict, pub_hold ? "true" : "false", pub_hold ? "false" : "true",
        pub_armed_ack ? "true" : "false", pub_ack_wait ? "true" : "false",
        pub_reserved ? "true" : "false",
        pub_reason, pub_down, pub_up);
}

/* The document into place, under pub_mu: 1 when it landed. snprintf
 * reports the untruncated length; never publish more than the buffer
 * holds, and never a document whose closing brace was cut off (the client
 * refuses an incomplete verdict). */
static int verdict_write(const char *body, int n, size_t cap)
{
    if (n < 0)
        return 0;
    if ((size_t)n >= cap) {
        fflog(LOG_ERR, "cool: verdict too long (%d bytes), "
                       "not published", n);
        return 0;
    }
    char file[256], tmp[256];
    verdict_path(file, sizeof(file), "cooling.state");
    verdict_path(tmp, sizeof(tmp), ".cooling.state.tmp");
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0)
        return 0;
    int ok = write(fd, body, (size_t)n) == n;
    close(fd);
    return ok && rename(tmp, file) == 0;
}

/* The acknowledgment off the file now, under pub_mu (mu not held): the
 * last document again with armed=false, if it carried armed=true. */
static void ack_withdraw(void)
{
    char body[VERDICT_BODY_MAX];
    int n = -1;
    pthread_mutex_lock(&mu);
    if (pub_armed_ack) {
        pub_armed_ack = 0;
        n = verdict_body_locked(body, sizeof(body));
    }
    pthread_mutex_unlock(&mu);
    if (n >= 0)
        verdict_write(body, n, sizeof(body));
}

static void verdict_publish(int fire_ok, const char *verdict, int hold,
                            int have_down, float down,
                            int have_up, float up)
{
    char body[VERDICT_BODY_MAX];
    /* Never while a session's warm-up decision waits for a reading: an arm
     * waits for it rather than read the undecided hold as a refusal. Never
     * while the button is reserved for another prompt: a press made for
     * that prompt must not complete an arm or a re-arm. Never while the
     * extension packages keep the window waiting (ext_unfrozen). */
    int armed_ack = eff_armed && fans_up && !warmup_pending &&
                    !button_withheld && !ext_unfrozen;
    int reserved = button_withheld;
    pthread_mutex_lock(&pub_mu);
    pthread_mutex_lock(&mu);
    /* The window this tick judged must still be the one reported, and open:
     * a close or a new window reported since the tick's snapshot has taken
     * the acknowledgment off the file, and this document must not put it
     * back. */
    double age = rep_at < 0 ? -1 : wall_s() - rep_at;
    armed_ack = armed_ack && rep_window == tick_window && coolfmt_armed(rep_armed, age, REPORT_TIMEOUT_S);
    int ack_wait = attn_ack_waits();
    if (!reserved)
        button_on_file_at = -1.0;
    snprintf(pub_verdict, sizeof(pub_verdict), "%s", verdict);
    pub_fire_ok = fire_ok;
    pub_hold = hold;
    pub_armed_ack = armed_ack;
    pub_ack_wait = ack_wait;
    pub_reserved = reserved;
    pub_down = have_down ? down : -273.15;
    pub_up = have_up ? up : -273.15;
    int n = verdict_body_locked(body, sizeof(body));
    pthread_mutex_unlock(&mu);
    int landed = verdict_write(body, n, sizeof(body));
    pthread_mutex_unlock(&pub_mu);
    /* The reservation's time on file starts with the first document that
     * carries it, never with the request: a reader can only have acted on
     * what it could read (cool_button_on_file_s). */
    if (reserved && landed) {
        pthread_mutex_lock(&mu);
        if (button_on_file_at < 0)
            button_on_file_at = wall_s();
        pthread_mutex_unlock(&mu);
    }
}

double cool_button_on_file_s(void)
{
    pthread_mutex_lock(&mu);
    double s = button_on_file_at < 0 ? -1.0 : wall_s() - button_on_file_at;
    pthread_mutex_unlock(&mu);
    return s;
}

/* ------------------------------------------------------------ engine */

/* Enter/leave the effective run window: reported mode "run" OR armed -
 * fire must never happen without the cut airflow, which only lives
 * inside a run session. */
/* The operator's attention. Whatever blocks firing and needs the operator
 * flashes the button red, and the panel says why: FIRE, CRASH, EMISSION,
 * FLAME, BUMP, AIRFLOW, CRITICAL, SENSOR, and a failed motion check. Holds
 * that clear on their own (the warm-up, a cold or a hot loop, an extension's
 * hold) do not: they are the machine's own to wait out.
 *
 * Red always means firing is blocked. A failure of the fail tier (FIRE,
 * CRASH, EMISSION, AIRFLOW, CRITICAL), or one that still stands when its
 * session ends, ends with the session (EMISSION: with the emission it saw)
 * as far as its own verdict goes, so it stays up as
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
    static const char *const names[] = { "FIRE", "CRASH", "EMISSION", "FLAME", "BUMP", "AIRFLOW",
                                         "CRITICAL", "SENSOR" };
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
        if (!strcmp(verdict, names[i]))
            return 1;
    return 0;
}

static int attn_fail_tier(const char *verdict)
{
    return !strcmp(verdict, "FIRE") || !strcmp(verdict, "CRASH") || !strcmp(verdict, "EMISSION") ||
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

/* The warm-up hold proper, on an upstream reading under the gate: the
 * loop heater on, the fans idle (their airflow cools the loop), fire
 * blocked, until the upstream reading reaches the gate. */
static void warmup_begin(double now, float up)
{
    cool_state = Cool_Warmup;
    warmup_pending = 0;
    warmup_gate_c = temp_start_c;
    warmup_last_up = up;
    warmup_progress_at = now;
    warmup_stall_warned = 0;
    warmup_win_n = 0;
    warmup_started_at = now;
    warmup_spent = 0;
    warmup_hot = 0;
    warmup_open_up = up;
    warmup_heat_said = -1;      /* said by the tick's heater block, with the heater it writes */
    warmup_saw_sensor = 0;
    fans_idle();
    flowrun_yield("warm-up");
}

/* A session that opens with the warm-up gate on and no upstream reading
 * (read_temp: none, or one over COOLANT_READING_MAX_C) is not decided
 * on it: it holds as WARMUP with the heater off, and the armed window is
 * not acknowledged, so a controller's arm waits for the decision instead
 * of reading the hold as the warm-up's refusal. The first reading decides
 * (engine_tick); a reading that never comes is SENSOR after its ticks. */
static void warmup_hold_pending(double now)
{
    cool_state = Cool_Warmup;
    warmup_pending = 1;
    warmup_gate_c = temp_start_c;
    warmup_win_n = 0;
    warmup_started_at = now;
    warmup_spent = 0;
    warmup_hot = 0;
    fans_idle();
    flowrun_yield("warm-up");
    heater_set_pct(0);
    warn("WARM-UP: no upstream coolant reading at the session's start - heater off, "
         "hold until one decides");
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
        fans_low_ticks = 0;
        fans_clean_ticks = 0;
        fans_up_at = -1.0;
        memset(fan_lost, 0, sizeof(fan_lost));
        fan_would_warned = 0;
        memset(fan_margin_noted, 0, sizeof(fan_margin_noted));
        critical_alarm = 0;
        critical_would_warned = 0;
        fan_grace_until = now + (double)fan_grace_s;
        /* A session that opens under the warm-up gate holds there
         * (warmup_begin); one that opens with no reading to decide on
         * holds undecided (warmup_hold_pending). */
        /* The session's run duties replace OVERTEMP's cooling airflow;
         * the gate itself stands, and its recovery or the session's end
         * puts the phase's profile back. */
        forced_cool = 0;
        float up_now = 0;
        int have_up_now = read_temp("pic/water_temp_2", &up_now, NULL, NULL);
        if (!gate_warmup_off && !have_up_now) {
            warmup_hold_pending(now);
        } else if (!gate_warmup_off && up_now < temp_start_c) {
            warmup_begin(now, up_now);
        } else {
            warmup_pending = 0;
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
        crash_hw_state = 0;         /* a watch that could not open or arm tries again */
        pgood_warned = 0;
    } else if (cool_state == Cool_Run || cool_state == Cool_Warmup) {
        if (cool_state == Cool_Warmup) {
            heater_set_pct(0);
            warmup_pending = 0;
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
        /* A short's fail tier is not the session's: it stands until the
         * job is gone (engine_tick, short_alarm). The controller's restart
         * at the trip ends the job; the tier stands until then. */
        /* What the job's sensors saw is the sensors logger's job record
         * (senslog.h). */
    }
}

/* Dead-man for a HUNG controller, not just a dead one (the supervisor
 * covers deaths): a reporter that went silent with the armed window open,
 * or with the kernel still playing the ring, has nobody left to stop the
 * beam - stop motion and lock the laser here. The armed case fires once per
 * silence episode; the running case repeats until the stop takes, so a
 * failed write cannot be a one-shot miss. Gated on at >= 0: a freshly
 * (re)started engine must not shoot down a healthy unmanaged controller, or
 * a homing runner a stopped daemon's controller left behind, that has not
 * reported to it yet. Run on every tick, a diagnostic's included. */
static void hang_deadman(double at, int fresh, int armed)
{
    if (fresh) {
        silent_safed = 0;
        return;
    }
    if (at >= 0 && ((armed && !silent_safed) || cnc_is_running())) {
        /* The writes first, the words after: a log line waits for
         * nothing, but nothing waits for a log line. */
        safing_write("cnc/stop", "1");
        cool_latch_lock();
        if (!silent_safed) {
            warn("controller silent while armed/running - "
                 "stopping motion, locking laser");
            senslog_event("controller silent while armed/running");
        }
        silent_safed = 1;
    }
}

/* Emission witness: laser_on_sampled counts the ~1 s window's emitting
 * samples on the gated output of the hardware AND-gate - physical evidence,
 * not a commanded state. Emission with no armed window in the recent past
 * (EMISSION_WINDOW_TOL_S) is EMISSION, the fail tier: the latch lock (the
 * light is live, and the latch is what cuts it) and then the stop, on every
 * tick the count stands, then the words, then the controller ended through
 * the supervisor, once (under a diagnostic, where the supervisor holds the
 * controller down, that starts nothing; with no controller running it ends
 * any respawn backoff). An armed report while the count reads above 0
 * clears nothing and stops no write: once the tier stands, the pair is
 * written on every tick the count reads above 0. The tier blocks
 * fire and stands while the count reads above 0; the acknowledgment
 * follows it (attention_tick). A count that cannot be read neither raises
 * nor clears the tier: past EMISSION_BLIND_TICKS ticks in a row it is
 * named once, and outside the tolerance it gets the pair once per
 * stretch. Run on every tick, a diagnostic's included, with the armed
 * report's time kept there too. */
static void emission_witness(double now, int fresh_armed)
{
    if (fresh_armed)
        last_armed_at = now;
    int in_tol = last_armed_at >= 0 && now - last_armed_at <= EMISSION_WINDOW_TOL_S;
    long em = rd_long("cnc/laser_on_sampled");
    if (em < 0) {
        if (emission_blind_ticks < EMISSION_BLIND_TICKS)
            emission_blind_ticks++;
        if (emission_blind_ticks < EMISSION_BLIND_TICKS)
            return;
        if (!in_tol && !emission_blind_safed) {
            emission_blind_safed = 1;
            cool_latch_lock();
            safing_write("cnc/stop", "1");
        }
        if (!emission_blind_said) {
            emission_blind_said = 1;
            fflog(LOG_WARNING, "cool: the laser emission count cannot be read (%d ticks): %s",
                  EMISSION_BLIND_TICKS, in_tol ? "the armed window is open, so only said"
                                              : "motion stopped, laser locked");
            senslog_event("laser emission count unreadable");
        }
        return;
    }
    if (emission_blind_said)
        info("the laser emission count reads again");
    emission_blind_ticks = 0;
    emission_blind_said = 0;
    emission_blind_safed = 0;
    if (em > 0 && (!in_tol || emission_alarm)) {
        cool_latch_lock();
        safing_write("cnc/stop", "1");
        if (!emission_alarm) {
            emission_alarm = 1;
            warn("EMISSION: laser emission sensed with no armed window - "
                 "motion stopped, laser locked, controller ended");
            senslog_event("laser emission sensed with no armed window");
            if (cool_fail_tier_stop)
                cool_fail_tier_stop("laser emission with no armed window");
        }
    } else if (em == 0 && emission_alarm) {
        emission_alarm = 0;
        info("laser emission no longer sensed; firing waits for the acknowledgment");
    }
}

/* The engine's hold on the latch stands while a reason it locked for
 * stands: the fail tiers' alarms (FIRE, CRASH, AIRFLOW, CRITICAL, and
 * SENSOR's on a short in a run), a silence it safed (until a report is
 * fresh again), and EMISSION (until the witness reads none). The fail
 * tiers' latch lock (EMISSION's included, also on a tick its count cannot
 * be read) is written again on every tick they stand, so a lock
 * that did not take is not a one-shot miss (the stop is not: the kernel
 * refuses it in its fault state, safing_write). Released here, on every
 * tick, a diagnostic's included, before the verdict. */
static void latch_hold_tick(void)
{
    if (fire_alarm || crash_w.alarm || airflow_alarm || critical_alarm || short_alarm ||
        emission_alarm)
        cool_latch_lock();
    cool_latch_release_tick(fire_alarm || crash_w.alarm || airflow_alarm || critical_alarm ||
                            short_alarm || silent_safed || emission_alarm);
}

/* The live cause the last diagnostic tick passed to the operator's attention
 * was EMISSION: once the witness reads none, nothing live stands, and an
 * older record that waits for its acknowledgment is not made live again by
 * it. Cleared by every tick outside a diagnostic. */
static int diag_emission_live = 0;

/* One engine tick at 1 Hz. */
static void engine_tick(void)
{
    double now = wall_s();
    cnc_state_sample(now);

    /* Diagnostics own the hardware between cool_diag_take and
     * cool_diag_release: publish the phase, keep fire blocked, and touch
     * no thermal output until the release reasserts the idle posture. The
     * hang dead-man and the emission witness run all the same. */
    pthread_mutex_lock(&mu);
    int dia = diag_owned;
    pthread_mutex_unlock(&mu);
    if (dia) {
        pthread_mutex_lock(&mu);
        pub_phase = "diag";
        int d_armed = rep_armed;
        double d_at = rep_at;
        pthread_mutex_unlock(&mu);
        int d_fresh = d_at >= 0 && now - d_at <= REPORT_TIMEOUT_S;
        hang_deadman(d_at, d_fresh, d_armed);
        emission_witness(now, d_fresh && d_armed);
        flowrun_ctx_t fc = { .t = now, .yield = 1, .yield_why = "diagnostics", .session = 0,
                             .fans = FW_FANS_OTHER };
        flowrun_engine(&fc);
        /* Nothing else is sensed while a diagnostic owns the hardware, so
         * the operator's attention stands as the last tick left it: a cause
         * that was live is still counted live (no notice says it cleared),
         * the red flash is kept up over whatever the diagnostic's side
         * wrote, and an acknowledgment that waits keeps its name. EMISSION
         * is sensed here: it takes the name while it stands, and once the
         * witness reads none it waits for the acknowledgment. Fire is
         * blocked and the hold stands either way. */
        char held[COOL_VERDICT_MAX];
        pthread_mutex_lock(&mu);
        snprintf(held, sizeof(held), "%s",
                 emission_alarm ? "EMISSION"
                 : attn_live && !diag_emission_live ? attn_verdict : "OK");
        pthread_mutex_unlock(&mu);
        diag_emission_live = emission_alarm;
        attention_tick(held);
        pthread_mutex_lock(&mu);
        int ack_wait = attn_latched && !attn_live;
        pthread_mutex_unlock(&mu);
        latch_hold_tick();
        verdict_publish(0, emission_alarm ? "EMISSION" : ack_wait ? "ACK" : "OK", 1, 0, 0, 0, 0);
        return;
    }

    diag_emission_live = 0;

    /* Snapshot the report. */
    pthread_mutex_lock(&mu);
    int mode = rep_mode, armed = rep_armed;
    unsigned long long win = rep_window;
    double at = rep_at;
    long duty[3] = {rep_duty[0], rep_duty[1], rep_duty[2]};
    cool_limits_t hdr = rep_lim;
    pthread_mutex_unlock(&mu);

    int fresh = at >= 0 && now - at <= REPORT_TIMEOUT_S;
    limits_apply(&hdr, fresh);
    hang_deadman(at, fresh, armed);
    if (!fresh) {
        mode = 0;
        armed = 0;
        if (at >= 0 && !silent_warned && (flood_on || cool_state == Cool_Run)) {
            silent_warned = 1;
            warn("controller went silent - standing down");
        }
    } else {
        silent_warned = 0;
    }

    /* A short's fail tier ends when the job does: a fresh report of idle
     * with no window (a GRBL reset, a cloud print canceled or ended), or no
     * controller at all (cool_controller_stopped: the supervisor's stop,
     * which the trip asks for itself). A session's end alone
     * (a window that ran out of grace, reported as cooldown) leaves it
     * standing, and so does a silent controller. */
    if (short_alarm && ((fresh && mode == 0 && !armed) || at < 0)) {
        short_alarm = 0;
        info("coolant sensor short: the job has ended; SENSOR stands until the sensors read again");
    }

    int flood = fresh && (mode == 1 || armed);
    int duty_changed = memcmp(run_duty, duty, sizeof(run_duty)) != 0;
    memcpy(run_duty, duty, sizeof(run_duty));
    /* A window that closed and a next that opened between two ticks is a
     * new window all the same: its identity tells it from the last. */
    int armed_rose = armed && (!eff_armed || win != eff_window);
    eff_armed = armed;
    eff_window = armed ? win : 0;
    tick_window = win;
    if (fresh && !armed)
        window_unarmed_seen = 1;
    /* A window that opens is acknowledged on its own fans: a fan the
     * session ran slower before it (unjudged then) is judged from here.
     * One this engine never saw closed was open before it started, and
     * owes its fans from the start. */
    if (armed_rose) {
        fans_up = 0;
        fans_low_ticks = 0;
        fans_clean_ticks = 0;
        window_owes_fans = !window_unarmed_seen;
    }
    /* A quiet hold (a listening with the fans off) never meets a run
     * session opening: the opening edge releases it (no hold is taken
     * outside the idle phase, cool_quiet_hold_try), and so does the
     * clock, so a listener that died cannot leave the fans off. */
    pthread_mutex_lock(&mu);
    int quiet = quiet_held;
    double quiet_age = now - quiet_since;
    pthread_mutex_unlock(&mu);
    if (quiet && ((flood && !flood_on) || armed_rose)) {
        cool_quiet_hold_ex(0, 0);
        warn("quiet hold released: a run session opened");
    } else if (quiet && quiet_age > (double)COOL_QUIET_HOLD_MAX_S) {
        cool_quiet_hold_ex(0, 0);
        warn("quiet hold released: it ran out of time");
    }
    /* The hold's fans, as its flag now stands, before anything below
     * writes a fan. */
    quiet_apply();
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
        /* The session's edge published first, under mu: a tools' hold
         * asked for from here on is refused (cool_quiet_hold_try). */
        pthread_mutex_lock(&mu);
        pub_session = flood;
        pthread_mutex_unlock(&mu);
        flood_on = flood;
        flood_apply(flood, now);
        if (flood) {
            /* A quiet hold set inside this opening tick, after the edge's
             * look above and before the edge was published (the route read
             * the idle phase the tick had not yet left), is released by the
             * same rule now that the session is open: its fans here, its
             * pump at the tick's settle. */
            pthread_mutex_lock(&mu);
            int late_quiet = quiet_held;
            pthread_mutex_unlock(&mu);
            if (late_quiet) {
                cool_quiet_hold_ex(0, 0);
                warn("quiet hold released: a run session opened");
                quiet_apply();
            }
            /* The session start reloaded the settings: resolve the
             * effective limits from them now, so this tick's gate rows,
             * gates_off and the run-start log line agree. */
            limits_apply(&hdr, fresh);
            pthread_mutex_lock(&mu);
            pub_reason[0] = '\0';   /* new session, stale reason gone */
            pthread_mutex_unlock(&mu);
        }
    }

    emission_witness(now, fresh && armed);

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
    /* A channel that cannot be read, or that reads past the converter's
     * 1023, is unreadable. 0 is a reading (a dark lid reads a count or
     * two). The readable channels are judged on their own while one is
     * not: with two or three, their quartiles sit at or over those of all
     * four, so nothing the four would trip goes unjudged; with one, its
     * reading stands for both quartiles and can sit under the four's
     * second, for the tick before SENSOR. A tick that is missing a channel
     * is not a clean one, and two in a row is SENSOR (pause tier: fire
     * blocked, hold), released once all four read. */
    long ir[4], q[4];
    int nq = 0, blind_ch = 0;
    for (int i = 0; i < 4; i++) {
        char attr[20];
        snprintf(attr, sizeof(attr), "pic/lid_ir_%d", i + 1);
        ir[i] = rd_long(attr);
        if (ir[i] < 0 || ir[i] > 1023) {
            if (!blind_ch)
                blind_ch = i + 1;
        } else {
            q[nq++] = ir[i];
        }
    }
    int have_ir = nq == 4;
    int ir_judged_phase = cool_state == Cool_Run || cool_state == Cool_Smoke ||
                          cool_state == Cool_Thermal;
    if (ir_judged_phase && !have_ir) {
        ir_blind_ticks++;
        if (!ir_blind_alarm && ir_blind_ticks >= IR_BLIND_TICKS) {
            ir_blind_alarm = 1;
            snprintf(ir_blind_reason, sizeof(ir_blind_reason),
                     "SENSOR: lid IR channel %d unreadable for %d s - fire blocked, hold",
                     blind_ch, ir_blind_ticks);
            warn(ir_blind_reason);
        }
    } else {
        ir_blind_ticks = 0;
        if (ir_blind_alarm) {
            ir_blind_alarm = 0;
            info(have_ir ? "lid IR channels reading again - released"
                         : "lid IR watch stood down at idle with a channel unreadable");
            pthread_mutex_lock(&mu);
            if (!strcmp(pub_reason, ir_blind_reason))
                pub_reason[0] = '\0';
            pthread_mutex_unlock(&mu);
        }
    }
    if (nq > 0 && ir_judged_phase) {
        for (int i = 0; i < nq - 1; i++)    /* the readings: sort them */
            for (int j = i + 1; j < nq; j++)
                if (q[j] < q[i]) { long t = q[i]; q[i] = q[j]; q[j] = t; }
        if (nq == 1)
            q[1] = q[0];
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
            cool_latch_lock();
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
        /* Only a tick with all four channels read is a clean one. */
        if (flame_alert && !alert && have_ir) {
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
        /* Not judged this tick (no lid IR channel read): the hold stands,
         * and the tick is not a clean one. The release is FIRE_CLEAR_TICKS
         * clean readings in a row, so the count starts over. */
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
     * answering, or that cannot be opened or armed, stands the watch
     * down for the session, said once; the next session tries again.
     * The engine owns the bus handle: every arm claims it first, opening
     * it when none is open, and a setup check only borrows the part,
     * never while the watch is armed (accel.h). */
    {
        int want = armed && (accel_x_alert > 0 || accel_y_alert > 0 ||
                             accel_abort > 0);
        if (want && !crash_hw_armed && crash_hw_state >= 0 &&
            crash_poll_errs < 3) {
            if (crash_hw_open() != 0) {
                crash_hw_state = -1;
                warn("head accelerometer not answering - crash watch off "
                     "for this session");
            } else if (crash_hw_arm((int)accel_x_alert, (int)accel_y_alert,
                                    (int)accel_abort) == 0) {
                crash_hw_state = 1;
                crash_hw_armed = 1;
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "crash watch armed (alert x=%d y=%d abort=%d)",
                         (int)accel_x_alert, (int)accel_y_alert,
                         (int)accel_abort);
                info(msg);
            } else {
                crash_hw_state = -1;
                warn("head accelerometer arm failed - crash watch off "
                     "for this session");
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
                    cool_latch_lock();
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
            if (judged && fan_floor[i] > 0.0 && fan_reading[i] < fan_floor[i]) {
                up_now = 0;
                snprintf(fans_low_name, sizeof(fans_low_name), "%s", fan_name[i]);
            }
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
                    /* The kernel pair first, the words after: motion
                     * stopped and the engine's latch hold locked, which
                     * stands for the session (cool_latch_release_tick).
                     * The controller is not ended through the supervisor:
                     * it takes the verdict on its own terms. */
                    safing_write("cnc/stop", "1");
                    cool_latch_lock();
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
         * armed run the window's acknowledgment waits for it (and for the
         * rest of verdict_publish's conditions, which say when it goes
         * out). A warm-up judges no fan, so nothing is said
         * there: its release drops the airflow, and the run's fans are
         * judged before the acknowledgment the first fire waits for. Once
         * up, it goes only with a fan that stays under its floor for the
         * airflow gate's count of ticks, which runs in the spin-up grace
         * too. An acknowledged window owes its fans from then on. */
        if (!flood_on) {
            fans_up = 0;
            fans_low_ticks = 0;
            fans_clean_ticks = 0;
        } else if (up_now) {
            if (!fans_up) {
                fans_up_at = now;
                if (armed && run)
                    info("fans at their floors in the armed window");
            }
            fans_up = 1;
            fans_low_ticks = 0;
            if (fans_clean_ticks < AIRFLOW_DEBOUNCE_TICKS)
                fans_clean_ticks++;
            if (armed && run)
                window_owes_fans = 1;
        } else {
            fans_clean_ticks = 0;
            if (fans_up && ++fans_low_ticks >= AIRFLOW_DEBOUNCE_TICKS) {
                fans_up = 0;
                fans_low_ticks = 0;
                if (armed)
                    info("a fan under its floor: the armed window's acknowledgment is withdrawn");
            }
        }
        char fg[sizeof(pub_fan_gates)];
        if (coolfmt_fan_gates(fg, sizeof(fg), rows, Fan_N) < 0)
            fflog(LOG_ERR, "cool: the fan gate rows do not fit their status "
                  "fragment; publishing none");
        pthread_mutex_lock(&mu);
        snprintf(pub_fan_gates, sizeof(pub_fan_gates), "%s", fg);
        pthread_mutex_unlock(&mu);
    }

    float down = 0, up = 0;
    long down_raw = -1, up_raw = -1;
    int down_over = 0, up_over = 0;
    int have_down = read_temp("pic/water_temp_1", &down, &down_raw, &down_over);
    int have_up = read_temp("pic/water_temp_2", &up, &up_raw, &up_over);

    /* The sensors themselves. Blind on either coolant sensor (a read that
     * fails, a count that is no temperature, or a reading over
     * COOLANT_READING_MAX_C), the ceiling, the floor,
     * the critical line and the warm-up can none of them judge, so at
     * SENSOR_FAIL_SCORE the engine says so (SENSOR: fire blocked, hold)
     * and takes the heater off: a warm-up gets its heater back once the
     * readings are. A reading over the line is a short, not a dropout: it
     * sets the score to its cap, so SENSOR rises in that tick and no tick
     * of it is tolerated; in a run session it, and a count of 0, is
     * SENSOR's fail tier (short_alarm). The gates keep their state
     * throughout; the verdict is released once the score has decayed to 0
     * and no short's fail tier stands. */
    const char *which = !have_down && !have_up ? "sensors"
                      : !have_up ? "upstream sensor" : "downstream sensor";
    if (have_down && have_up) {
        if (sensor_fail_score > 0)
            sensor_fail_score--;
        if (sensor_fail_score == 0)
            sensor_fail_ticks = 0;
        if (sensor_alarm && sensor_fail_score == 0 && !short_alarm) {
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
        /* A reading over the line: SENSOR in its own tick, anywhere. A
         * count of 0 joins it in a run session only; at idle it is a
         * dropout like any count that is no temperature. */
        int shorted = (!have_down && down_over == SHORT_OVER) || (!have_up && up_over == SHORT_OVER);
        int zeroed = (!have_down && down_over == SHORT_ZERO) || (!have_up && up_over == SHORT_ZERO);
        sensor_fail_score += SENSOR_FAIL_UP;
        if (sensor_fail_score > SENSOR_FAIL_CAP || shorted)
            sensor_fail_score = SENSOR_FAIL_CAP;
        if (sensor_fail_ticks < UINT32_MAX)
            sensor_fail_ticks++;
        /* A short (over the line, or a count of 0) in a run session is
         * SENSOR's fail tier: the kernel pair first, the words after, then
         * the controller ended through the supervisor, as at FIRE and
         * CRASH, once, over a SENSOR already standing too, and it stands
         * until the job has ended (above, before flood). A run session is
         * the one phase in which a job can fire (CRITICAL is judged in it
         * alone); in a warm-up, at idle and in a job's tail a short is the
         * plain SENSOR. */
        int short_rose = (shorted || zeroed) && cool_state == Cool_Run && !short_alarm;
        if (short_rose) {
            sensor_fail_score = SENSOR_FAIL_CAP;
            short_alarm = 1;
            safing_write("cnc/stop", "1");
            cool_latch_lock();
        }
        /* The counts read (down, up), when one read as none: 0, or a few
         * counts over it (over 70 C), is a short, 1023 and over the rail
         * an open sensor sits on; -1 a read that failed. */
        char counts[24] = "";
        if ((!have_up && up_raw >= 0) || (!have_down && down_raw >= 0))
            snprintf(counts, sizeof(counts), " (counts %d, %d)",
                     (int)(down_raw > 65535 ? 65535 : down_raw < -1 ? -1 : down_raw),
                     (int)(up_raw > 65535 ? 65535 : up_raw < -1 ? -1 : up_raw));
        /* A tick the score tolerates joins the window; the window's
         * SENSOR_WINDOW_N-th within SENSOR_WINDOW_S is not tolerated. */
        unsigned in_window = 0;
        if (!sensor_alarm && sensor_fail_score < SENSOR_FAIL_SCORE) {
            sensor_tolerated_at[sensor_tolerated_n % SENSOR_WINDOW_N] = now;
            sensor_tolerated_n++;
            unsigned have = sensor_tolerated_n < SENSOR_WINDOW_N ? sensor_tolerated_n : SENSOR_WINDOW_N;
            for (unsigned i = 0; i < have; i++)
                if (now - sensor_tolerated_at[i] < SENSOR_WINDOW_S)
                    in_window++;
            if (in_window >= SENSOR_WINDOW_N)
                sensor_fail_score = SENSOR_FAIL_CAP;
            else if (now - sensor_tolerated_said >= SENSOR_TOLERATED_SAY_S) {
                sensor_tolerated_said = now;
                fflog(LOG_WARNING, "cool: coolant %s gave no reading for a tick%s: tolerated, "
                      "the gates keep their state (%u tolerated in the last %.0f s)",
                      which, counts, in_window, SENSOR_WINDOW_S);
            }
        }
        if ((!sensor_alarm && sensor_fail_score >= SENSOR_FAIL_SCORE) || short_rose) {
            sensor_alarm = 1;
            char msg[COOL_REASON_MAX];
            if (short_rose)
                snprintf(msg, sizeof(msg),
                         "SENSOR: coolant %s %s, a short%s - job stopped, "
                         "no resume this job",
                         which, shorted ? "over 70 C" : "at count 0", counts);
            else if (shorted)
                snprintf(msg, sizeof(msg),
                         "SENSOR: coolant %s over %.0f C, a short%s - fire blocked, "
                         "hold, heater off",
                         which, COOLANT_READING_MAX_C, counts);
            else if (in_window >= SENSOR_WINDOW_N)
                snprintf(msg, sizeof(msg),
                         "SENSOR: coolant %s unreadable on %u ticks in %.0f s - fire blocked, "
                         "hold, heater off",
                         which, in_window, SENSOR_WINDOW_S);
            else
                snprintf(msg, sizeof(msg),
                         "SENSOR: coolant %s unreadable on %u ticks%s - fire blocked, "
                         "hold, heater off",
                         which, (unsigned)sensor_fail_ticks, counts);
            warn(msg);
            /* A short's fail tier ends the controller as FIRE and CRASH
             * do, after its kernel writes: the job ends on every path,
             * and no held job is left to resume. */
            if (short_rose && cool_fail_tier_stop)
                cool_fail_tier_stop("coolant sensor short");
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
            /* Idle and thermal take the cooling airflow and hold it while
             * the gate stands (fans_apply_phase); a run or a smoke phase
             * keeps its higher run duties. */
            if (cool_state == Cool_Idle || cool_state == Cool_Thermal)
                fans_apply_phase();
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

    /* No quiet hold stands over the ceiling: the gate's rise releases the
     * whole hold, fans and pump, and so does a hold found standing under
     * it, so OVERTEMP's cooling airflow and the TEC run from this tick
     * (quiet_apply puts the phase's fans back; the settle gives the pump
     * back). The requester finds the hold gone at its next read. The
     * cooling airflow is published in the same hold of mu as the look, so
     * a hold the route asks for after it is refused (-3), not set behind
     * the release for the rest of this tick. The release goes to the log
     * alone: the reason published stays OVERTEMP's own. */
    if (over_temp_gate) {
        pthread_mutex_lock(&mu);
        pub_cooling_forced = 1;
        int q = quiet_held;
        pthread_mutex_unlock(&mu);
        if (q) {
            cool_quiet_hold_ex(0, 0);
            fflog(LOG_WARNING, "cool: quiet hold released: the coolant is over its ceiling");
            quiet_apply();
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

    /* A session that opened undecided (warmup_hold_pending) is decided by
     * its first reading the cold side takes: under the gate, the warm-up
     * proper; at it or over, the run, with the fans' spin-up grace. */
    if (cool_state == Cool_Warmup && warmup_pending && have_up) {
        if (up < warmup_gate_c) {
            warmup_begin(now, up);
        } else {
            warmup_pending = 0;
            cool_state = Cool_Run;
            fans_run();
            fan_grace_until = now + (double)fan_grace_s;
            /* The undecided hold judged no fan: as at the warm-up's
             * release, the run's fans are judged from the next tick. */
            fans_up = 0;
            fans_low_ticks = 0;
            fans_clean_ticks = 0;
            fans_up_at = -1.0;
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "warm-up not needed: coolant %.1f C at the %.0f C start gate - run",
                     up, warmup_gate_c);
            info(msg);
            pthread_mutex_lock(&mu);
            pub_reason[0] = '\0';
            pthread_mutex_unlock(&mu);
        }
    }

    /* The warm-up hold: released at the gate into a normal run session
     * (run fans, the spin-up grace). Under the gate a loop that stops
     * warming is named once and keeps holding. */
    if (cool_state == Cool_Warmup && !warmup_pending && have_up) {
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
            /* The warm-up judged no fan: the run's are judged from the
             * next tick, and the window is acknowledged no earlier than
             * when they read their floors, never on this tick's release. */
            fans_up = 0;
            fans_low_ticks = 0;
            fans_clean_ticks = 0;
            fans_up_at = -1.0;
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

    /* The phase and the gates are settled: the pump hold, before the
     * heater below and the pump write. */
    quiet_pump_settle();

    /* The warm-up heater, decided every tick, and written every tick it may
     * run: the kernel holds a heater write above zero only for its lease
     * (heater_lease_ms, 5 s), so a daemon that stops ticking leaves it off.
     * Off with a blind sensor (SENSOR above), past the downstream cutoff
     * (on again 5 C under it: every gate reads upstream, which sits before
     * the heater), once the budget is spent (the hold stays: a loop that
     * has not reached the gate in 30 minutes is not going to), with the
     * pump held off by a quiet hold, and while the session is undecided. */
    if (cool_state == Cool_Warmup && warmup_pending) {
        heater_set_pct(0);
    } else if (cool_state == Cool_Warmup) {
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
        int heat = have_down && !sensor_alarm && !warmup_hot && !warmup_spent && !cool_quiet_pump_held() &&
                   !quiet_pump_applied;
        heater_set_pct(heat ? WARMUP_HEATER_PCT : 0);
        /* The warm-up is said, as the hold's reason, with the heater as
         * written here: on its first tick, when the heater first comes on,
         * and once a SENSOR that stood over it is released (the release
         * empties the reason). Under a standing SENSOR the line is only
         * logged, and SENSOR keeps its words. */
        if (sensor_alarm)
            warmup_saw_sensor = 1;
        if (warmup_heat_said < 0 || (heat && !warmup_heat_said) || (warmup_saw_sensor && !sensor_alarm)) {
            char msg[112];
            snprintf(msg, sizeof(msg), "WARM-UP: coolant %.1f C under the %.0f C start gate - %s, hold",
                     warmup_open_up, warmup_gate_c, heat ? "heater on" : "heater off for now");
            if (sensor_alarm)
                fflog(LOG_WARNING, "cool: %s", msg);
            else {
                warn(msg);
                warmup_saw_sensor = 0;
            }
            warmup_heat_said = heat;
        }
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
     * (CRITICAL: motion stopped and the latch locked at the trip, hold,
     * fire blocked, no resume this session; the session
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
            /* The kernel pair first, as at AIRFLOW: the controller is not
             * ended through the supervisor. */
            safing_write("cnc/stop", "1");
            cool_latch_lock();
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
        start_cool = 0;
        fans_apply_phase();
        fflog(LOG_INFO, "cool: the machine is idle after the busy start; %s airflow",
              forced_cool ? "OVERTEMP's cooling" : "idle");
    }
    if (cool_state == Cool_Smoke && now >= phase_until) {
        if ((have_up && up > temp_resume_c) || cnc_is_running()) {
            cool_state = Cool_Thermal;
            phase_until = now + (double)cooldown_max_s;
            fans_cool();
        } else {
            cool_state = Cool_Idle;
            fans_apply_phase();
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
            fans_apply_phase();
        }
    }

    /* The TEC. Wanted only with the part declared present, a reading in
     * hand, the fans running (its heat sink sits in their airflow; a quiet
     * hold stops them), the hysteresis satisfied, and the loop clear of the
     * floor. Off is immediate; on waits the dwell after the last switch. */
    {
        int airflow = (cool_state == Cool_Run || cool_state == Cool_Smoke ||
                       cool_state == Cool_Thermal || forced_cool) && !quiet_applied;
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
                         : !have_up ? "no coolant reading"
                         : up <= eff_temp_min_c + TEMP_MIN_HYST_C
                           ? "at the coolant floor" : "coolant cooled");
            info(msg);
        }
        /* The pump and the TEC, written every tick: the pump on and the
         * TEC at what the engine wants, both off while a quiet hold's pump
         * hold stands. A write that fails (or one a device did not keep)
         * is put right by the next tick's, and the kernel holds a TEC on
         * only for its lease (heater_lease_ms), which these writes renew:
         * a TEC whose engine stopped writing goes off by itself. */
        hw_assert("thermal/water_pump_on", !quiet_pump_applied, &pump_write_failing);
        hw_assert("thermal/tec_on", quiet_pump_applied ? 0 : tec_on_state, &tec_write_failing);
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
    pthread_mutex_lock(&mu);
    int quiet_now = quiet_held;
    pthread_mutex_unlock(&mu);
    /* The machine's button reserved for a prompt that is not this window's
     * job: asked outside the lock, once a tick. */
    char button_why[64] = "";
    button_withheld = cool_button_reserved && cool_button_reserved(button_why, sizeof(button_why));
    /* The packages, while a window is reported, once a tick: the host's
     * word for this window first, then the kernel's on every group. Each
     * change of what keeps the acknowledgment waiting is said once. */
    {
        char why[sizeof(ext_wait_words)] = "";
        int was = ext_unfrozen;
        ext_unfrozen = eff_armed && (!ext_host_word(eff_window, why, sizeof(why)) ||
                                     !ext_packages_still(ext_cg_dir, why, sizeof(why)));
        if (ext_unfrozen && (!was || strcmp(why, ext_wait_words)))
            fflog(LOG_NOTICE, "cool: the armed window is not acknowledged yet: %s", why);
        else if (!ext_unfrozen && was && eff_armed)
            info("the extension packages are paused for the armed window");
        snprintf(ext_wait_words, sizeof(ext_wait_words), "%s", ext_unfrozen ? why : "");
    }
    /* A window that owes its fans with the acknowledgment gone (a judged fan
     * under its floor for the gate's count of ticks, grace or not): the pause
     * tier, until every judged fan has read its floor for the same count of
     * ticks in a row, so a fan that dips and comes back for a tick does not
     * cycle the job lit and dark. AIRFLOW still trips once the grace is over
     * and the fan has not come back. Its rise goes to the log alone: the
     * reason is the verdict's, and a verdict over it (AIRFLOW, which rises
     * in the same tick past the grace, CRITICAL, SENSOR) keeps its own
     * words; the reason block below writes FANS's while FANS is the name. */
    {
        int was = fans_held;
        fans_held = eff_armed && window_owes_fans && cool_state == Cool_Run &&
                    (!fans_up || (was && fans_clean_ticks < AIRFLOW_DEBOUNCE_TICKS));
        if (fans_held && !was) {
            fflog(LOG_WARNING, "cool: FANS: %s under its floor in the armed window - fire blocked, hold",
                  fans_low_name);
        } else if (!fans_held && was)
            info("FANS cleared: every judged fan at its floor, or the window closed");
    }
    /* The name is the highest verdict standing. Every name that ends the
     * job or waits for the operator stands over FANS, the pause tier, so
     * the controllers' fail tier, the red flash and its acknowledgment, the
     * flight recorder and the cloud client's fault all see it. */
    const char *verdict = fire_alarm ? "FIRE"
                        : crash_w.alarm ? "CRASH"
                        : emission_alarm ? "EMISSION"
                        : flame_alert ? "FLAME"
                        : crash_w.alert ? "BUMP"
                        : airflow_alarm ? "AIRFLOW"
                        : critical_alarm ? "CRITICAL"
                        : sensor_alarm || ir_blind_alarm ? "SENSOR"
                        : fans_held ? "FANS"
                        : over_temp_gate ? "OVERTEMP"
                        : cold_gate ? "COLD"
                        : warming ? "WARMUP"
                        : ext.standing ? "EXT"
                        : quiet_now ? "QUIET"
                        : button_withheld ? "BUTTON" : "OK";
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
    /* A quiet hold (every fan off for a listening) and a reserved button
     * block fire like ACK does: always, and they hold only an open window,
     * so motion stays free. */
    int hold = fire_alarm || crash_w.alarm || emission_alarm || flame_alert || crash_w.alert
             || airflow_alarm || fans_held || critical_alarm || sensor_alarm || ir_blind_alarm
             || over_temp_gate || cold_gate || warming
             || ext.standing || (ack_wait && eff_armed)
             || ((quiet_now || button_withheld) && eff_armed);
    int fire_ok = fresh && !fire_alarm && !crash_w.alarm && !emission_alarm && !flame_alert
                && !crash_w.alert && !airflow_alarm && !fans_held
                && !critical_alarm && !sensor_alarm && !ir_blind_alarm
                && !over_temp_gate && !cold_gate && !warming
                && !ext.standing && !ack_wait
                && !quiet_now && !button_withheld;
    /* The reason is the hold's while the verdict is: it says which package
     * and why, and another event's words may have stood there since. The
     * lid IR channels' SENSOR keeps its own words the same way (a coolant
     * sensor's SENSOR, when both stand, keeps the coolant's), and so does
     * the reservation, which says whose prompt it is. */
    if (!strcmp(verdict, "EXT")) {
        pthread_mutex_lock(&mu);
        snprintf(pub_reason, sizeof(pub_reason), "%s", ext.reason);
        pthread_mutex_unlock(&mu);
    } else if (!strcmp(verdict, "SENSOR") && ir_blind_alarm && !sensor_alarm) {
        pthread_mutex_lock(&mu);
        snprintf(pub_reason, sizeof(pub_reason), "%s", ir_blind_reason);
        pthread_mutex_unlock(&mu);
    } else {
        int fans_words_gone = 0;
        pthread_mutex_lock(&mu);
        if (!strcmp(verdict, "QUIET"))
            snprintf(pub_reason, sizeof(pub_reason), "QUIET: every fan is held off for a listening - fire blocked");
        else if (!strcmp(verdict, "BUTTON"))
            snprintf(pub_reason, sizeof(pub_reason), "BUTTON: %s - fire blocked until it is done", button_why);
        else if (!strcmp(verdict, "FANS"))
            snprintf(pub_reason, sizeof(pub_reason), "FANS: %s under its floor in the armed window - "
                     "hold", fans_low_name);
        /* Nothing else standing, and the packages keep the window waiting:
         * the reason says so while the job waits, dark: the GRBL controller
         * relays it to the sender, and the panel shows it. */
        else if (!strcmp(verdict, "OK") && ext_unfrozen)
            snprintf(pub_reason, sizeof(pub_reason), "%s", ext_wait_words);
        else if (!strncmp(pub_reason, "QUIET: ", 7) || !strncmp(pub_reason, "BUTTON: ", 8) ||
                 !strncmp(pub_reason, "FANS: ", 6) || !strncmp(pub_reason, EXT_WAIT_WORDS, strlen(EXT_WAIT_WORDS))) {
            fans_words_gone = !strncmp(pub_reason, "FANS: ", 6);
            pub_reason[0] = '\0';           /* the words go with their verdict */
        }
        pthread_mutex_unlock(&mu);
        /* The ceiling's hold stood under FANS: it names itself again, as it
         * does when a SENSOR or a CRITICAL over it clears. */
        if (fans_words_gone && !strcmp(verdict, "OVERTEMP"))
            overtemp_warn(0);
    }

    pthread_mutex_lock(&mu);
    pub_phase = cool_state == Cool_Run ? "run"
              : cool_state == Cool_Warmup ? "warm-up"
              : cool_state == Cool_Smoke ? "smoke"
              : cool_state == Cool_Thermal ? "thermal" : "idle";
    pub_cooling_forced = over_temp_gate || forced_cool;
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

    latch_hold_tick();
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
    /* A starting engine has heard from no controller and knows no window
     * or session: a window reported open from here on that it never saw
     * closed was opened before it started. */
    pthread_mutex_lock(&mu);
    rep_at = -1.0;
    rep_armed = 0;
    pthread_mutex_unlock(&mu);
    eff_armed = 0;
    eff_window = 0;
    flood_on = 0;
    cool_state = Cool_Idle;
    fans_up = 0;
    fans_low_ticks = 0;
    fans_clean_ticks = 0;
    window_owes_fans = 0;
    window_unarmed_seen = 0;
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

    mkdir(verdict_dir(), 0755);

    /* Idle posture. Purge air belongs to the armed/run story the same
     * way air assist does, but the factory holds it on continuously;
     * keep that. A daemon that starts next to an unmanaged controller
     * mid-job, or a homing runner its last controller left behind, must
     * not drop the exhaust under live motion: a busy machine gets the
     * cooldown duties instead, and
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
    /* The TEC off before the engine's join as well as after it: an engine
     * that hangs never returns from the join, and a TEC left on chills a
     * loop with nothing watching it (the kernel's lease takes it off within
     * heater_lease_ms of the engine's last write in any case). Off is
     * always the right parting state: the restarted daemon decides it
     * again. The pump on, before and after: the pump runs whenever the
     * machine is on, and a quiet pump hold ends with the daemon. */
    wr_attr("thermal/tec_on", "0");
    wr_attr("thermal/water_pump_on", "1");
    pthread_mutex_lock(&mu);
    int was = engine_run;
    engine_run = 0;
    pthread_mutex_unlock(&mu);
    if (was)
        pthread_join(engine_th, NULL);
    /* The flow watch first: its heater off, whatever it was doing. */
    flowrun_shutdown();
    /* The heater is the warm-up's heat source; off is always the right
     * parting state, job or no job. So is the TEC, after a tick that was
     * in flight at the first write. */
    heater_set_pct(0);
    wr_attr("thermal/tec_on", "0");
    wr_attr("thermal/water_pump_on", "1");
    /* The crash watch took the accel's ODR and full scale; hand them
     * back so st_accel's next one-shot reads at its own scale. */
    if (crash_hw_armed) {
        crash_hw_disarm();
        crash_hw_armed = 0;
    }
    crash_hw_close();
    /* On a machine still busy (the deceleration of the stop super_shutdown
     * made, or a GRBL homing runner that homes on), idling the
     * fans here would drop the exhaust under it, and unlinking the verdict
     * would feed-hold it the moment a reader notices. Leave both alone: an
     * aging verdict file reads exactly like a stopped engine within the
     * 2 s staleness window, and the restarted daemon reasserts the right
     * posture. */
    if (machine_is_idle()) {
        char file[256];
        fans_idle();
        verdict_path(file, sizeof(file), "cooling.state");
        unlink(file);           /* missing file = fire blocked at readers */
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

    /* A report that closes the window, or opens a new one, takes the
     * acknowledgment off the file before it is answered: an acknowledgment
     * computed for one window is never on file once its close is reported.
     * pub_mu keeps the engine's publish from landing between the two. */
    pthread_mutex_lock(&pub_mu);
    pthread_mutex_lock(&mu);
    double now = wall_s();
    int was_open = coolfmt_armed(rep_armed, rep_at < 0 ? -1 : now - rep_at, REPORT_TIMEOUT_S);
    int opens = armed && !was_open;
    if (opens) {
        unsigned long long id = (unsigned long long)(now * 1000.0);
        rep_window = id > rep_window ? id : rep_window + 1;
    }
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
    rep_at = now;
    pthread_mutex_unlock(&mu);
    if (!armed || opens)
        ack_withdraw();
    pthread_mutex_unlock(&pub_mu);
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
    pthread_mutex_lock(&pub_mu);
    pthread_mutex_lock(&mu);
    rep_at = -1.0;
    rep_mode = 0;
    rep_armed = 0;
    pthread_mutex_unlock(&mu);
    ack_withdraw();
    pthread_mutex_unlock(&pub_mu);
}

int cool_status_json(char *buf, size_t len)
{
    /* The extension host reads the window here, and a window it reads
     * closed it may thaw for: a window this document shows closed (a report
     * gone stale) has its acknowledgment off the file first. The report's
     * age is read once, for the withdrawal and the document both; pub_mu
     * keeps every report out meanwhile. */
    pthread_mutex_lock(&pub_mu);
    pthread_mutex_lock(&mu);
    double age = rep_at < 0 ? -1 : wall_s() - rep_at;
    int open_now = coolfmt_armed(rep_armed, age, REPORT_TIMEOUT_S);
    pthread_mutex_unlock(&mu);
    if (!open_now)
        ack_withdraw();
    pthread_mutex_lock(&mu);
    coolfmt_status_t st = {
        .phase = pub_phase, .verdict = pub_verdict, .reason = pub_reason,
        .fire_watch = pub_fire_watch, .accel_watch = pub_accel_watch,
        .quiet_hold = quiet_held,
        .fire_ok = pub_fire_ok, .hold = pub_hold,
        .armed = open_now,
        .window = rep_window,
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
    pthread_mutex_unlock(&pub_mu);
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
     * correction included; NAN for a count that is no temperature
     * (coolant.h). */
    if (coolant_no_reading(raw))
        return NAN;
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
