/*
 * diag.h - forgectrl: hardware diagnostics runner
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef DIAG_H
#define DIAG_H

#include <stddef.h>

/* The flow tools' safety rail: a heater trial stops the moment the
 * downstream sensor reads this, pump on or off. */
#define DIAG_ABORT_DOWN_C 48.0

/* Startup recovery: if a diagnostic died mid-run (stale marker file),
 * stand the hardware down and restart the motion controller. */
void diag_init(void);

/* Launch a tool ("flow-verify" or "flow-calibrate") on the runner
 * thread. Returns 0 on start, -1 if a diagnostic is already running,
 * -2 if the machine is not idle, -3 for an unknown tool, -4 when the
 * machine lease is somebody else's, with the holder in why. */
int diag_start(const char *tool, char *why, size_t len);

/* diag_start with a heater duty for this run alone: the setup's flow
 * calibration heats a rung's trials at heater_pct in place of
 * cool_flow_heater_pct, and nothing is written to the settings for it.
 * Only "flow-calibrate" takes a duty, above 0 and at most 100; any other
 * tool, or a duty outside that range, is -3. A heater_pct of 0 is
 * diag_start. */
int diag_start_duty(const char *tool, double heater_pct, char *why, size_t len);

/* Request an abort; the runner stands down and restarts the controller. */
void diag_abort(void);

/* Live runner state as JSON (phase, temperatures, log, result). */
int diag_status_json(char *buf, size_t len);

/* True while a diagnostic owns the hardware (locks settings writes). */
int diag_running(void);

#endif
