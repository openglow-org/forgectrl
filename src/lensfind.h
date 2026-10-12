/*
 * lensfind.h - the lens's free travel (see lensfind.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_LENSFIND_H
#define FORGECTRL_LENSFIND_H
#include <jansson.h>

typedef struct {
    int found;
    int below, above;               /* free half-steps from the edge, when found */
    int contact_below, contact_above;
    char why[120];
} lensfind_t;

/* Both stops, from the hall edge, at the run current in half-step mode,
 * with the controller stopped and the lens on its edge. The lens ends on
 * the edge at its hold current; the finding is filed in `r` under
 * "stops". Returns 0 (found or not: st->found says, st->why says why
 * not). */
int lensfind_run(lensfind_t *st, json_t *r);
#endif
