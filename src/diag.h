/*
 * diag.h - forgectrl: hardware diagnostics runner
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef DIAG_H
#define DIAG_H

#include <stddef.h>

/* Startup recovery: if a diagnostic died mid-run (stale marker file),
 * stand the hardware down and restart the motion controller. */
void diag_init(void);

/* Launch a tool ("aa-offset-calibrate") on the runner thread. Returns 0
 * on start, -1 if a diagnostic is already running, -2 if the machine is
 * not idle, -3 for an unknown tool, -4 when the machine lease is
 * somebody else's, with the holder in why, -5 while the cooling engine
 * runs OVERTEMP's cooling airflow. */
int diag_start(const char *tool, char *why, size_t len);

/* Request an abort; the runner stands down and restarts the controller. */
void diag_abort(void);

/* The daemon's stop: a run in flight is aborted and waited for, up to
 * 10 s, so its takeover is handed back and the cooling engine has the
 * thermal hardware again before either stops. */
void diag_shutdown(void);

/* Live runner state as JSON (phase, temperatures, log, result). */
int diag_status_json(char *buf, size_t len);

/* True while a diagnostic owns the hardware (locks settings writes). */
int diag_running(void);

#endif
