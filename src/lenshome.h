/*
 * lenshome.h - lens hall-edge reference (see lenshome.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_LENSHOME_H
#define FORGECTRL_LENSHOME_H

#include <stddef.h>

/* Sweep the lens onto its hall sensor's rising edge, with no controller
 * running. 1 = the lens sits on the edge and the marker is written, 0 =
 * the hunt failed (a hard fault: no head, or the edge never appeared), -1
 * = the hunt could not run at all (no /sys/glowforge, so not this
 * hardware), LENSHOME_STOPPED = stop answered nonzero between two steps
 * and the sweep ended there. stop may be NULL. Every exit after the first
 * step puts the motor back at its hold current and half step, and only a
 * hunt that found the edge leaves a marker. detail gets a short outcome
 * text in every case. */
#define LENSHOME_STOPPED (-2)
int lenshome_run(char *detail, size_t dlen, int (*stop)(void));

/* Drop the marker. A reference is only good while no controller has run
 * since it was taken: a fresh broker hold and every controller exit clear
 * it, and the supervisor sweeps again before the next GRBL controller. */
void lenshome_clear(void);

/* Whether the marker stands. */
int lenshome_present(void);

#endif
