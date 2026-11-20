/*
 * button.h - the physical button as an edge (see button.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_BUTTON_H
#define FORGECTRL_BUTTON_H

/* Start waiting for a press: a release-to-press edge on the switch,
 * within timeout_s. One wait at a time; a second start while one is
 * live returns -1. The wait runs on its own thread; button_state()
 * reports it. */
int button_wait_start(int timeout_s);
/* The wait's judge, asked at every reading with @at_press 0 and once more
 * at a press with 1: 1 a press may count from here, 0 not yet (the
 * readings until then count nothing, so a press counts only from a
 * release read after it says 1), -1 the wait ends "cancelled" (at a
 * press: the press is not this wait's). NULL: every press counts. */
extern int (*button_wait_judge)(int at_press);
/* Called once on the wait's own thread as it ends, however it ends (a
 * press, its timeout, a cancel, the judge), before button_state() leaves
 * "waiting". NULL: nothing. */
extern void (*button_wait_ended)(void);
/* Cancel a live wait. */
void button_wait_cancel(void);
/* "idle" (no wait), "waiting", "pressed", "timeout", "cancelled". */
const char *button_state(void);
/* Take a "pressed" result: returns 1 once, then the state is idle. */
int button_take_pressed(void);
/* Whether the button reads held right now (a level, not an edge). */
int button_held(void);
/* Whether the button was held continuously for hold_s from now: the
 * boot-time reset check. Blocks up to hold_s. */
int button_held_for(int hold_s);
#endif
