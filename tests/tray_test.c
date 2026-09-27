/*
 * tray_test.c - host unit test for the crumb tray's mode in the daemon
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The GRBL controller keeps the mode and persists it as the marker
 * tray.out in the data directory; the daemon reads it. Cases: the marker
 * says the mode; the offset setting reads as the controller reads it (the
 * value held to 13 to 60 mm, the default when unset or not a number) and
 * lands on the controller's half-step, 1.35 in on 100; the shift is zero
 * with the tray in; a write of the offset is valid from 13 to 60 mm only;
 * a change of the offset is refused while the tray is out, and a write of
 * the same value, or any write with the tray in, is not; and the setup
 * cards are refused while the tray is out, with the operator's words.
 */
#define _GNU_SOURCE
#include "../src/tray.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
                              else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The settings store on the host: what the test puts there. */
static const char *stub_offset;

int settings_get(const char *key, char *val, size_t len)
{
    if (strcmp(key, "tray_offset_mm") || !stub_offset)
        return -1;
    snprintf(val, len, "%s", stub_offset);
    return 0;
}

static char dir[64], marker[128];

static void set_out(int out)
{
    if (out) {
        FILE *f = fopen(marker, "w");
        fputs("out\n", f);
        fclose(f);
    } else
        unlink(marker);
}

static int near(double a, double b)
{
    return fabs(a - b) < 1e-5;
}

int main(void)
{
    char why[160];

    snprintf(dir, sizeof(dir), "/tmp/tray_test-XXXXXX");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("FORGECTRL_DATA_DIR", dir, 1);
    snprintf(marker, sizeof(marker), "%s/tray.out", dir);

    /* The mode is the marker. */
    set_out(0);
    CHECK(!tray_is_out(), "no marker: the tray is in");
    CHECK(near(tray_shift_mm(), 0), "tray in: no shift");
    set_out(1);
    CHECK(tray_is_out(), "the marker: the tray is out");

    /* The offset, as the controller reads it, on its grid. */
    const struct { const char *v; double mm; long k; const char *what; } off[] = {
        { NULL, 34.29, 100, "unset: the default, 1.35 in, 100 half-steps" },
        { "34.29", 34.29, 100, "the default written out" },
        { "20", 20, 58, "20 mm: 58 half-steps" },
        { "13", 13, 38, "the low end" },
        { "60", 60, 175, "the high end" },
        { "5", 13, 38, "under the range: held to 13 mm" },
        { "99", 60, 175, "over the range: held to 60 mm" },
        { "abc", 34.29, 100, "not a number: the default" },
        { "nan", 34.29, 100, "NaN: the default" },
        { "", 34.29, 100, "empty: the default" },
    };
    for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
        stub_offset = off[i].v;
        CHECK(near(tray_offset_mm(), off[i].mm), "offset %s: %g mm", off[i].what, tray_offset_mm());
        CHECK(near(tray_shift_mm(), off[i].k / 2.922), "shift %s: %.4f mm", off[i].what, tray_shift_mm());
        CHECK(lround(tray_grid_mm() * 2.922) == off[i].k, "grid %s", off[i].what);
    }
    stub_offset = NULL;

    /* What the setting takes. */
    const struct { const char *v; int ok; } val[] = {
        { "13", 1 }, { "34.29", 1 }, { "60", 1 }, { "12.99", 0 }, { "60.01", 0 },
        { "0", 0 }, { "-34", 0 }, { "abc", 0 }, { "34mm", 0 }, { "", 0 },
    };
    for (size_t i = 0; i < sizeof(val) / sizeof(val[0]); i++)
        CHECK(tray_offset_valid(val[i].v) == val[i].ok, "'%s' is %s", val[i].v, val[i].ok ? "valid" : "refused");

    /* A change while the tray is out is refused; nothing else is. */
    set_out(1);
    stub_offset = "34.29";
    CHECK(tray_offset_refusal("20", why, sizeof(why)) == 1 && strstr(why, "tray in"),
          "tray out: a change is refused (%s)", why);
    CHECK(tray_offset_refusal("", why, sizeof(why)) == 1, "tray out: unsetting a set offset is refused");
    CHECK(tray_offset_refusal("34.29", why, sizeof(why)) == 0, "tray out: the same value is no change");
    CHECK(tray_offset_refusal(NULL, why, sizeof(why)) == 0, "tray out: a request without the key");
    stub_offset = NULL;
    CHECK(tray_offset_refusal("", why, sizeof(why)) == 0, "tray out: unsetting an unset offset is no change");
    CHECK(tray_offset_refusal("40", why, sizeof(why)) == 1, "tray out: setting an unset offset is refused");
    set_out(0);
    CHECK(tray_offset_refusal("20", why, sizeof(why)) == 0, "tray in: a change goes ahead");

    /* The setup cards need the tray in. */
    why[0] = '\0';
    CHECK(tray_setup_refusal(why, sizeof(why)) == 0 && !why[0], "tray in: the cards run");
    set_out(1);
    CHECK(tray_setup_refusal(why, sizeof(why)) == 1 && strstr(why, "Put the crumb tray in"),
          "tray out: the cards are refused (%s)", why);

    set_out(0);
    rmdir(dir);
    if (fails) {
        printf("%d FAILURE(S)\n", fails);
        return 1;
    }
    printf("PASS tray_test\n");
    return 0;
}
