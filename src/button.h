/*
 * button.h - the physical button as an edge (see button.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_BUTTON_H
#define FORGECTRL_BUTTON_H

/* Start waiting for a press: a release-to-press edge on the switch,
 * within timeout_s. One wait at a time; a second start while one is
 * live returns -1. The wait runs on its own thread; button_state()
 * reports it. */
int button_wait_start(int timeout_s);
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
