/*
 * super.h - controller-mode supervisor (see super.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_SUPER_H
#define FORGECTRL_SUPER_H

#include <stddef.h>

/* Start the supervisor thread and spawn the controller selected by the
 * controller_mode setting (grbl unless set to cloud). If an unmanaged
 * controller process is already running (legacy init scripts), the
 * supervisor stands by instead of fighting it. */
void super_init(void);
/* The daemon's HTTP listener is up: nothing moves and no controller starts
 * before this call. */
void super_listening(void);

/* The daemon's stop, in two calls. super_shutdown_begin, as the stop
 * begins: the safing pair (cnc/stop and the latch) lands at once, nothing
 * starts from then on, a live controller is sent its SIGTERM, a probe or a
 * sweep in flight ends between its steps, and the waits that hold a
 * connection thread end. super_shutdown,
 * at its end: the controller stopped the deliberate way, busy or idle
 * (SIGTERM, escalating), then the lifecycle thread, then the broker. It
 * begins the stop itself when nobody did. */
void super_shutdown_begin(void);
void super_shutdown(void);

/* Whether the motion check failed: no controller runs until it is retried. */
int super_motion_fault(void);

/* Switch the active controller mode ("grbl" or "cloud"): idle-gated,
 * stops the active controller, persists the setting, starts the other,
 * and waits for its first job-state report. Returns 0 on success, -1
 * with err filled on refusal/failure. */
/* as: the machine-lease owner the caller is (a wizard switching the mode
 * inside its own hold), or NULL for anybody else. */
int super_mode_switch(const char *mode, const char *as, char *err, size_t elen);

/* The stop lever (POST /controller/stop): the safing pair at once, and the
 * controller's SIGTERM from the caller's thread, then stop the active
 * controller without changing the selected mode and hold supervision
 * suspended. Returns 0 once it is down; 1 at once while the supervisor
 * stands by beside a controller it did not start (the pair is in, the hold
 * stands, and that controller is ended once the machine is idle); -1 when
 * it has not exited in 15 s (the hold stands, and the supervisor ends it
 * when it gets to it). The start resumes supervision,
 * clears a motion fault (the operator's retry) and lets the selected
 * mode's controller come back, whichever that is; neither it nor a mode
 * switch resumes anything once the daemon's stop has begun. */
int super_controller_stop(void);
void super_controller_start(void);
/* A setup check's or a diagnostic's takeover: the controller stopped for
 * the check's own reason (0 once it is down), and the hand-back, which
 * ends that reason alone. A stop lever pulled meanwhile, or a motion
 * fault the check found or found standing, outlives the hand-back. */
int super_takeover_begin(void);
void super_takeover_end(void);
/* What keeps the controllers down whatever a check does: the stop lever's
 * hold, a motion fault. A check that needs a controller of its own refuses
 * to start under them, in their words. */
#define SUPER_HELD    1u
#define SUPER_FAULTED 2u
unsigned super_holds(void);
/* The cooling engine's fail tier (a fire signal, a head crash): the
 * controller is stopped the deliberate way (the safing pair before the
 * signal) and started again at once by the lifecycle thread. Never
 * blocks; safe from any thread. */
void super_controller_restart(const char *why);
/* SIGCHLD wakes the lifecycle thread (super_init installs it; a host
 * test installs it alone). */
void super_sigchld_init(void);

/* {"mode":"grbl","controller":"running|stopped|standby|waiting|gated|motion-fault",
 *  "pid":N,"motion":"verified|unverified|fault","stepper_fault":bool,
 *  "gated":bool,"local":bool,"why":"..."}
 * stepper_fault: a motion fault stands and the kernel reads fault, however
 * the fault was found: POST /mode refuses it, and POST /controller/start is
 * the retry.
 * waiting: the motion gate holds the spawn until a lid or the interlock
 * closes. why names what holds the machine (the gate, the enclosure),
 * else the probe's own words behind a faulted or unverified verdict. */
int super_status_json(char *buf, size_t len);
/* The wizard's posture: while on, the setup gate does not
 * apply and the Grbl controller binds to loopback only. A change
 * restarts a running Grbl controller with the new bind. */
void super_set_local(int on);
/* The motion wizard's visible probe: with the controller stopped and any
 * homing runner it left ended, run the liveness probe sequence on the
 * broker's device; under the stop lever's hold it answers 4 and moves
 * nothing. 1 verified, 0 a motion fault (no motion after the
 * ladder, a lens with no edge, a stepper enable the kernel refused, or a
 * probe the machine failed), 2 could not run, 3 it ended before a
 * verdict (the enclosure, a busy kernel), 4 the stop lever ended it;
 * detail says why. The supervisor's own gate takes the outcome too, and
 * 3 and 4 leave motion unverified, so the lifecycle probes again before
 * any controller starts. */
int super_probe_motion(char *detail, size_t dlen);
/* Whether the gate holds controllers back right now, and why. */
int super_gated(char *why, size_t len);

/* True while the machine is in GRBL mode with its controller process
 * alive: the gate for echoing the controller's published state files
 * (a dead controller's files must read as absent, not as truth). */
int super_grbl_running(void);
/* The running GRBL controller's pid, 0 with none: a new pid is a new
 * controller, whatever lay between. */
long super_grbl_pid(void);

/* The controller's report channel: whether presented is the secret the
 * running controller was handed at its spawn. Never true with no
 * controller. */
int super_report_secret_ok(const char *presented);

#endif
