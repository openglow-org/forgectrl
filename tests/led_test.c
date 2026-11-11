/*
 * led_test.c - host unit test for the button LED's two readers
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * led.c against plain files under the test hook GF_LED_ROOT, in the LED
 * class's shape: button_led_{1,2,3}/{target,speed,pulse_on,pulse_off}.
 * The cooling engine asks led_red_up() before it writes the red flash
 * again, and a wizard asks led_asks_press() before it takes a press as
 * the job's, so what each says of a lamp is a rule:
 *
 *   A. no LED class to read: both answer -1
 *   B. dark: neither
 *   C. breathing white, as a controller writes it while a job waits for
 *      the press: the press is asked for, and that is not the red
 *   D. solid white, as a controller writes it while the machine moves,
 *      its targets read back as the kernel settles them, and as led_set
 *      writes it: neither
 *   E. the red flash, as led_set writes it: the red, and no press asked
 *   F. another pattern's green and blue left lit under a pulsing red: not
 *      the red
 */
#define _GNU_SOURCE
#include "../src/led.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static char root[] = "/tmp/led-test-XXXXXX";
static const char *const attrs[] = { "target", "speed", "pulse_on", "pulse_off" };

static void put(int ch, const char *attr, int val)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/button_led_%d/%s", root, ch, attr);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", val);
        fclose(f);
    }
}

/* A lamp as some writer left it: each channel's target, and one pulse for
 * the channels that pulse (0: none). */
static void lamp(int r, int g, int b, int pulse_r, int pulse_g, int pulse_b)
{
    const int target[3] = { r, g, b }, pulse[3] = { pulse_r, pulse_g, pulse_b };
    for (int ch = 1; ch <= 3; ch++) {
        put(ch, "target", target[ch - 1]);
        put(ch, "speed", 64);
        put(ch, "pulse_on", pulse[ch - 1]);
        put(ch, "pulse_off", pulse[ch - 1]);
    }
}

int main(void)
{
    setenv("FORGECTRL_HOST_TEST", "1", 1);     /* the test hooks' gate (testhook.h) */
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    setenv("GF_LED_ROOT", root, 1);

    printf("A. no LED class to read\n");
    CHECK(led_asks_press() == -1 && led_red_up() == -1, "both answer -1");

    for (int ch = 1; ch <= 3; ch++) {
        char dir[160];
        snprintf(dir, sizeof(dir), "%s/button_led_%d", root, ch);
        mkdir(dir, 0755);
    }

    printf("B. dark\n");
    lamp(0, 0, 0, 0, 0, 0);
    CHECK(led_asks_press() == 0 && led_red_up() == 0, "no press asked, no red");

    printf("C. breathing white\n");
    lamp(255, 255, 255, 1500, 1500, 1500);
    CHECK(led_asks_press() == 1, "the press is asked for");
    CHECK(led_red_up() == 0, "and that is not the red");

    printf("D. solid white\n");
    lamp(254, 254, 254, 0, 0, 0);       /* a settled 255, as the trigger reads it back */
    CHECK(led_asks_press() == 0 && led_red_up() == 0, "no press asked, no red");
    led_set(LED_SOLID_WHITE);           /* as the probe and a setup step write it */
    CHECK(led_asks_press() == 0 && led_red_up() == 0 && led_dark() == 0,
          "led_set's solid white: lit, no pulse, so no press asked and no red");

    printf("E. the red flash\n");
    led_set(LED_FLASH_RED);
    CHECK(led_red_up() == 1, "the red stands");
    CHECK(led_asks_press() == 0, "and asks for no press");

    printf("F. green and blue left lit under a pulsing red\n");
    lamp(255, 255, 255, 1000, 0, 0);
    CHECK(led_red_up() == 0 && led_asks_press() == 0, "not the red, and no press asked");

    /* The red flash stands over every pattern forgectrl writes and leaves
     * the button dark when it ends, so the owner of a pattern written once
     * keeps it: led_keep() writes it again over a dark button, and never
     * over a light that stands. */
    printf("G. dark, as the keeper reads it\n");
    lamp(0, 0, 0, 0, 0, 0);
    CHECK(led_dark() == 1, "nothing pulsing, every target at 0: dark");
    lamp(0, 0, 0, 1000, 0, 0);
    CHECK(led_dark() == 0, "a pulsing channel between pulses (its target at 0) is not dark");
    lamp(0, 0, 40, 0, 0, 0);
    CHECK(led_dark() == 0, "one channel lit is not dark");
    {
        char path[160];
        lamp(0, 0, 0, 0, 0, 0);
        snprintf(path, sizeof(path), "%s/button_led_2/target", root);
        unlink(path);
        CHECK(led_dark() == -1, "a channel that cannot be read: no answer");
        led_keep(LED_BLINK_AMBER);
        CHECK(led_red_up() == 0 && led_dark() == -1, "and the keeper writes nothing on no answer");
        lamp(0, 0, 0, 0, 0, 0);
    }

    printf("H. a pattern its owner keeps\n");
    led_set(LED_BLINK_AMBER);           /* written once, as the enclosure wait writes it */
    led_set(LED_FLASH_RED);             /* the engine's flash stands over it */
    led_keep(LED_BLINK_AMBER);
    CHECK(led_red_up() == 1, "the red flash stands: the keeper leaves it");
    led_release();                      /* the flash ends, as the engine ends it */
    CHECK(led_dark() == 1, "the flash ended: the button is dark");
    led_keep(LED_BLINK_AMBER);
    CHECK(led_dark() == 0 && led_red_up() == 0 && led_asks_press() == 0,
          "the keeper writes the amber again (red and green pulsing: not dark, not the red, no press asked)");
    led_keep(LED_BREATHE_TEAL);
    CHECK(led_asks_press() == 0 && led_red_up() == 0 && led_dark() == 0, "another keeper leaves a light that stands");
    lamp(254, 254, 254, 0, 0, 0);
    led_keep(LED_BREATHE_TEAL);
    CHECK(led_dark() == 0 && led_asks_press() == 0, "and leaves a controller's solid white as it is");
    led_release();

    for (int ch = 1; ch <= 3; ch++) {
        char path[160];
        for (size_t i = 0; i < sizeof(attrs) / sizeof(*attrs); i++) {
            snprintf(path, sizeof(path), "%s/button_led_%d/%s", root, ch, attrs[i]);
            unlink(path);
        }
        snprintf(path, sizeof(path), "%s/button_led_%d", root, ch);
        rmdir(path);
    }
    rmdir(root);
    printf(failures ? "led_test: %d FAILED\n" : "led_test: all ok\n", failures);
    return failures ? 1 : 0;
}
