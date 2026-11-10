/*
 * cool.h - cooling engine (see cool.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_COOL_H
#define FORGECTRL_COOL_H

#include <stddef.h>

/* Diagnostic ownership: a diagnostic tool takes the thermal hardware
 * for a bounded takeover and writes only through the guarded helpers.
 * Take succeeds only from an idle engine; release reasserts the idle
 * posture in one place. */
int  cool_diag_take(void);
void cool_diag_release(void);
void cool_diag_pump(int on);
void cool_diag_heater_pct(double pct);
void cool_diag_fans_run(void);
void cool_diag_fans_idle(void);
void cool_diag_aa(long duty);
void cool_diag_purge(int on);       /* the head's purge fan */
/* The quiet hold for a listening to the head accelerometer. cool_quiet_hold
 * (the lens stop finder): every fan off, and while held the engine's own
 * fan writes go to zero as well. cool_quiet_hold_ex with with_pump (the
 * bench's tools through POST /cool/quiet?pump=1): the coolant pump and
 * the TEC off too, so the machine is silent; dry by construction (idle
 * only, the laser latched), the one pump-off state. The release puts the
 * phase's posture back. The engine releases the hold itself when a run
 * session opens (a job never runs with the machine held quiet) and after
 * COOL_QUIET_HOLD_MAX_S, so a listener that died cannot leave it. */
#define COOL_QUIET_HOLD_MAX_S 600
void cool_quiet_hold(int on);
void cool_quiet_hold_ex(int on, int with_pump);
int cool_quiet_held(void);
/* Whether a quiet hold may take the pump now: the engine idle (no warm-up,
 * run, smoke or thermal phase, no diagnostic). */
int cool_quiet_pump_ok(void);
int cool_quiet_pump_held(void);

/* Factory run fan duties (pulse-header ground truth; also the
 * controllers' compiled-in emergency-fallback values per the
 * contract). */
#define COOL_AIR_ASSIST_RUN  1023
#define COOL_EXHAUST_RUN     65535
#define COOL_INTAKE_RUN      43278

/* Start the engine: resolve tunables (GFCOOL_* env > cool_* settings >
 * compiled defaults), take the idle posture (pump on, TEC off, purge
 * on, heater off, fans idle), and launch the 1 Hz policy thread that
 * writes /run/forgefirm/cooling.state. */
void cool_init(void);

/* Stop the thread, stand the hardware down to the idle posture and
 * remove the verdict file (readers treat a missing file as
 * fire-blocked). */
void cool_shutdown(void);

/* Per-job limits a report may carry (cloud mode passes the pulse
 * header's envelope; GRBL mode passes none). Each is a value the
 * engine applies only where it is stricter than its own configured
 * limit; a field at or below zero is absent. */
typedef struct {
    double coolant_max_c;       /* run ceiling, upstream sensor */
    double coolant_min_c;       /* run floor (no gate yet; logged) */
    double exhaust_min_rpm;     /* fan floors (no gate yet; logged) */
    double intake_min_rpm;
    double air_assist_min_rpm;
} cool_limits_t;

/* Job-state report from the active controller (POST /cool/state).
 * mode is "idle", "run" or "cooldown"; armed = laser armed (forces the
 * run profile whatever mode says). The duty
 * arguments override the run fan profile for this job; pass -1 to use
 * the configured/factory values. lim carries the job's limits, or
 * NULL for none. Returns 0, or -1 on a bad mode. */
int cool_state_report(const char *mode, int armed,
                      long air_assist, long exhaust, long intake,
                      const cool_limits_t *lim);

/* Engine state as JSON (verdict, temps, phase, last report age, the
 * effective limits and the per-fan gate states) for the UI and bench
 * tooling. A buffer of COOL_STATUS_JSON_MAX bytes always holds the
 * whole document (coolfmt.h sizes the fragments, and a fragment that
 * does not fit is published empty, never cut); returns -1 when the
 * buffer given was too small, so a caller never serves a cut-off
 * document as JSON. */
#define COOL_STATUS_JSON_MAX 3072
int cool_status_json(char *buf, size_t len);

/* The operator's acknowledgment of a failure the panel shows. Returns 0, or
 * -1 while its cause still stands: the red flash stays until that is gone. */
int cool_attention_ack(void);

/* Dismiss the notices of faults that cleared on their own, up to and including
 * this sequence number (the panel's last one shown). */
void cool_notices_dismiss(unsigned upto);

/* The armed window as `GET /cool/status` publishes it: the running
 * controller's last report while that report is fresh, and false when
 * there is none or it has gone stale. Callers that must not add work
 * beside the step stream ask this. */
int cool_armed(void);

/* Seconds since the last job-state report, or -1 if none has ever
 * arrived (the supervisor uses this to see a controller come alive). */
double cool_report_age(void);

/* The supervisor stopped the controller on purpose, or reaped one that
 * died: its last report is forgotten, so the engine has no reporter
 * until the next controller speaks. A stop and a death are the
 * supervisor's, not a hang; without this the hang dead-man would count
 * the seconds since the last report and stop the supervisor's own
 * liveness probe, the one program that plays with no controller alive. */
void cool_controller_stopped(void);

/* The fail tiers (FIRE, CRASH) end the controller: after the engine's
 * own kernel writes (motion stopped, latch locked) this is called with
 * the reason, once per episode. main.c points it at the supervisor's
 * restart; a host test counts the calls. NULL means the kernel writes
 * stand alone. */
extern void (*cool_fail_tier_stop)(const char *why);

/* The counts the air-assist fan's ground shift adds to a raw coolant
 * thermistor reading at the duty the engine last commanded (more counts
 * read colder): cool_aa_offset_counts scaled by the fan's current above
 * its start, bounded; zero when the setting is zero or the fan is idle.
 * Every consumer of the two readings takes it off before the conversion. */
long cool_coolant_offset_counts(void);

/* The gates whose setting sits at its off end, as a JSON array of gate
 * names ("[]" when every gate is on), from the engine's resolved
 * tunables at the last run start. /status carries it beside the
 * settings reply's per-key state. */
int cool_gates_off_json(char *buf, size_t len);

/* The engine as the sensors logger samples it (senslog.h): the phase,
 * the verdict and its reason as published, and the armed window. */
typedef struct {
    char phase[12], verdict[12], reason[112];
    int hold, fire_ok, armed;
} cool_view_t;
void cool_view(cool_view_t *v);
/* A raw coolant count in degrees, converted as the engine converts its
 * own readings. */
double cool_coolant_c(long raw);
/* The slowest engine tick, in ms, since the last call (NAN: none). */
double cool_tick_max_ms(void);
/* Every cool_* tunable as resolved at the last settings reload, as
 * space-separated key=value pairs. */
void cool_settings_in_force(char *buf, size_t len);

#endif
