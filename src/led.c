/*
 * led.c - the button LED, while no controller owns it
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The three channels under /sys/class/leds/button_led_{1,2,3} are the
 * red, green, and blue emitters behind the button, each with the
 * ledtrig_smooth attributes: target (0 to 255, faded to), speed (the
 * fade rate), and pulse_on/pulse_off in milliseconds, which together
 * make the LED breathe between off and the target on its own. A
 * pattern is written once and the hardware keeps it up, so the wizard
 * spends no thread on it.
 */
#define _GNU_SOURCE
#include "led.h"
#include "testhook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The LED class directory, or the test hook GF_LED_ROOT in a host-test
 * run (testhook.h). */
static const char *led_root(void)
{
    const char *r = ff_test_hook("GF_LED_ROOT");
    return r ? r : "/sys/class/leds";
}

static void wr(int ch, const char *attr, int val)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/button_led_%d/%s", led_root(), ch, attr);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%d\n", val);
    fclose(f);
}

/* Channel 1 red, 2 green, 3 blue. */
static void write_pattern(int r, int g, int b, int on_ms, int off_ms, int speed)
{
    const int rgb[3] = { r, g, b };
    for (int ch = 1; ch <= 3; ch++) {
        /* Stop any pulse before the new target so the fade is clean. */
        wr(ch, "pulse_on", 0);
        wr(ch, "pulse_off", 0);
        wr(ch, "speed", speed);
        wr(ch, "target", rgb[ch - 1]);
        if (on_ms && rgb[ch - 1]) {
            wr(ch, "pulse_on", on_ms);
            wr(ch, "pulse_off", off_ms);
        }
    }
}

void led_set(led_pattern_t p)
{
    switch (p) {
    case LED_BREATHE_TEAL:  write_pattern(0, 180, 200, 1400, 1400, 16); break;
    case LED_SOLID_WHITE:   write_pattern(255, 255, 255, 0, 0, 64); break;
    case LED_BLINK_AMBER:   write_pattern(255, 110, 0, 350, 350, 160); break;
    case LED_SOLID_GREEN:   write_pattern(0, 255, 40, 0, 0, 64); break;
    case LED_FLASH_RED:     write_pattern(255, 0, 0, 1000, 1000, 160); break;
    case LED_OFF:
    default:                write_pattern(0, 0, 0, 0, 0, 64); break;
    }
}

void led_release(void)
{
    led_set(LED_OFF);
}

/* One channel's pulse_on in milliseconds, or -1. The target is no use here:
 * a pulsing channel's target swings between 0 and 255. */
static int rd_pulse(int ch)
{
    char path[160], text[16];
    snprintf(path, sizeof(path), "%s/button_led_%d/pulse_on", led_root(), ch);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fgets(text, sizeof(text), f) != NULL;
    fclose(f);
    return ok ? atoi(text) : -1;
}

int led_asks_press(void)
{
    int p[3] = { rd_pulse(1), rd_pulse(2), rd_pulse(3) };
    if (p[0] < 0 && p[1] < 0 && p[2] < 0)
        return -1;
    return p[0] > 0 && p[1] > 0 && p[2] > 0;
}

/* One channel's target, or -1. */
static int rd_target(int ch)
{
    char path[160], text[16];
    snprintf(path, sizeof(path), "%s/button_led_%d/target", led_root(), ch);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fgets(text, sizeof(text), f) != NULL;
    fclose(f);
    return ok ? atoi(text) : -1;
}

/* The red flash, and nothing over it: red pulsing, green and blue dark (a
 * write of another pattern landing in the middle of the red's leaves them
 * lit, and that is not the red). */
int led_red_up(void)
{
    int p[3] = { rd_pulse(1), rd_pulse(2), rd_pulse(3) };
    if (p[0] < 0 && p[1] < 0 && p[2] < 0)
        return -1;
    return p[0] > 0 && p[1] <= 0 && p[2] <= 0 && rd_target(2) <= 0 && rd_target(3) <= 0;
}

/* Dark: nothing pulsing, every target at 0. One channel that cannot be read
 * is no answer. */
int led_dark(void)
{
    for (int ch = 1; ch <= 3; ch++) {
        int p = rd_pulse(ch), t = rd_target(ch);
        if (p < 0 || t < 0)
            return -1;
        if (p > 0 || t > 0)
            return 0;
    }
    return 1;
}

/* The red flash stands over every pattern here and leaves the button dark
 * when it ends, so a pattern written once is gone after it. Its owner puts
 * it back on its next pass; a button that is lit, by the flash or by
 * anything else, is left as it is. */
void led_keep(led_pattern_t p)
{
    if (led_dark() == 1)
        led_set(p);
}
