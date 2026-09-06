/*
 * led.h - the button LED, while no controller owns it (see led.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_LED_H
#define FORGECTRL_LED_H

/* The named patterns the wizard uses. The controller drives the LED
 * itself while it runs (white for an arm wait), so these are only
 * written while the supervisor holds no controller; led_release()
 * turns the LED off and hands it back. */
typedef enum {
    LED_OFF = 0,
    LED_BREATHE_TEAL,   /* "press me to accept" */
    LED_BREATHE_WHITE,  /* a long step is running */
    LED_BLINK_AMBER,    /* attention: the lid, an error, the reset hold */
    LED_SOLID_GREEN,    /* commissioned */
} led_pattern_t;

void led_set(led_pattern_t p);
void led_release(void);
/* Whether the button is lit now, whoever lit it: 1 lit, 0 dark, -1
 * unreadable. The controller lights it white for its arm wait and puts
 * it out at the press, so a lit button under a running job is the
 * press the job waits for. */
int led_lit(void);
#endif
