/*
 * led.h - the button LED, while no controller owns it (see led.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_LED_H
#define FORGECTRL_LED_H

/* The named patterns forgectrl writes. A controller drives the LED itself
 * while it runs: breathing white while a job waits for the press that
 * unlocks it, solid white while the machine moves. So these are written
 * while the supervisor holds no controller, except the red flash, which
 * stands over any controller; led_release() turns the LED off and hands
 * it back. */
typedef enum {
    LED_OFF = 0,
    LED_BREATHE_TEAL,   /* "press me to accept" */
    LED_SOLID_WHITE,    /* the machine is moving, or a long step has it */
    LED_BLINK_AMBER,    /* attention: the lid, an error, the reset hold */
    LED_SOLID_GREEN,    /* setup complete */
    LED_FLASH_RED,      /* firing is blocked and the panel says why */
} led_pattern_t;

void led_set(led_pattern_t p);
void led_release(void);
/* Whether the button is breathing white, whoever set it: 1 yes, 0 no, -1
 * unreadable. A controller does that only while a job waits for the press,
 * and stops at the press, so this is the press the job waits for. */
int led_asks_press(void);
/* Whether the red flash stands: 1 yes, 0 no, -1 unreadable. */
int led_red_up(void);
/* Whether the button is dark, no channel pulsing and every target at 0:
 * 1 yes, 0 no, -1 unreadable. */
int led_dark(void);
/* A pattern its owner wrote once and still wants: written again when the
 * button reads dark, which is how the red flash leaves it when it ends.
 * Never over a light that stands, the red flash included. The owner calls
 * it on each of its passes while the pattern is wanted. */
void led_keep(led_pattern_t p);
#endif
