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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *led_root(void)
{
    const char *r = getenv("GF_LED_ROOT");
    return r && *r ? r : "/sys/class/leds";
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
    case LED_BREATHE_WHITE: write_pattern(200, 200, 200, 1800, 1800, 12); break;
    case LED_BLINK_AMBER:   write_pattern(255, 110, 0, 350, 350, 160); break;
    case LED_SOLID_GREEN:   write_pattern(0, 255, 40, 0, 0, 64); break;
    case LED_OFF:
    default:                write_pattern(0, 0, 0, 0, 0, 64); break;
    }
}

void led_release(void)
{
    led_set(LED_OFF);
}

/* The commanded level of one channel (the smooth trigger's target, or
 * the brightness where no target exists), or -1. */
static int rd_level(int ch)
{
    static const char *const attrs[] = { "target", "brightness" };
    for (size_t i = 0; i < sizeof(attrs) / sizeof(*attrs); i++) {
        char path[160], text[16];
        snprintf(path, sizeof(path), "%s/button_led_%d/%s", led_root(), ch, attrs[i]);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        int ok = fgets(text, sizeof(text), f) != NULL;
        fclose(f);
        if (ok)
            return atoi(text);
    }
    return -1;
}

int led_lit(void)
{
    int readable = 0;
    for (int ch = 1; ch <= 3; ch++) {
        int v = rd_level(ch);
        if (v < 0)
            continue;
        readable = 1;
        if (v > 0)
            return 1;
    }
    return readable ? 0 : -1;
}
