/*
 * liveness.h - motion-liveness probe (see liveness.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_LIVENESS_H
#define FORGECTRL_LIVENESS_H

#include <stddef.h>

/* Command a small motion through the pulse-device fd and verify it
 * PHYSICALLY happened via the head accelerometer. Requires the kernel
 * idle and no motion controller running (the supervisor guarantees
 * both). Returns 1 = motion confirmed, 0 = the gantry did not move
 * (wedged stepper drivers), -1 = probe could not run, -2 = a lid or the
 * interlock is open (or the switches cannot be read): the gantry was
 * not moved. detail gets a short human-readable result line either
 * way. */
int liveness_probe(int pulse_fd, char *detail, size_t dlen);
/* The enclosure as the probe sees it: 0 = closed (the gantry may move),
 * 1 = a lid or the interlock is open, -1 = the switch device cannot be
 * read (no license to move: treated as open). why gets the reason in
 * the operator's words ("the lid is open"), empty when closed. */
int liveness_enclosure(char *why, size_t wlen);
/* The classification alone, from the EV_SW word (host-testable):
 * 0 closed, 1 open, why as above. */
int liveness_enclosure_classify(unsigned sw, char *why, size_t wlen);
/* One raw reading of the head accelerometer's X and Y (the motion
 * wizard's witness during a jog). 0, or -1 without the part. */
int liveness_accel_read(long *x, long *y);

#endif
