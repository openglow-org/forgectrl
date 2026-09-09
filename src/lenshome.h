/*
 * lenshome.h - lens hall-edge reference (see lenshome.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_LENSHOME_H
#define FORGECTRL_LENSHOME_H

#include <stddef.h>

/* Sweep the lens onto its hall sensor's rising edge, in the motion-verify
 * window, before any controller starts. 1 = the lens sits on the edge and
 * the marker is written, 0 = the hunt failed (a hard fault: no head, or
 * the edge never appeared), -1 = the hunt could not run at all (no
 * /sys/glowforge, so not this hardware). detail gets a short outcome text
 * in every case. */
int lenshome_run(char *detail, size_t dlen);

/* Drop the marker. A reference is only good for the broker hold that made
 * it, so a fresh hold clears it before the hunt re-runs. */
void lenshome_clear(void);

#endif
