/*
 * lens_test.c - host unit test for the lens frame
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Every Z the daemon commands must be one the controller takes. The
 * frame reads the window the controller opens its Z soft limit from, the
 * way the controller reads it (a stop count from 1 to 40, else the
 * fallback; the edge, else the default); the reach lies inside the
 * controller's envelope at every edge height the settings accept and
 * every window from one half-step to the widest; the focus ladder lies
 * inside the reach, its first line at the top of the window and its last
 * at the bottom; and a program line's Z is read from G0 and G1 alone. The
 * envelope is derived here the controller's way, in machine steps and in
 * float, apart from lens_envelope.
 */
#include "../src/lens.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* The settings store on the host: what the test puts there. */
static const char *stub_edge, *stub_below, *stub_above;

int settings_get(const char *key, char *val, size_t len)
{
    const char *v = !strcmp(key, "lens_hall_edge_z_mm") ? stub_edge
                    : !strcmp(key, "lens_stop_below_steps") ? stub_below
                    : !strcmp(key, "lens_stop_above_steps") ? stub_above : NULL;
    if (!v)
        return -1;
    snprintf(val, len, "%s", v);
    return 0;
}

/* The controller's Z soft limit, its way: the edge placed on its step
 * grid (lroundf of the float product, as the driver computes), the limit
 * from (steps - below - 1) to (steps + above + 1) over the scale in
 * machine coordinates, then the G92 every program declares (Z = the edge
 * height, with the lens on that grid step) into the program's frame. */
static void controller_envelope(double edge, double spm, int below, int above, double *lo, double *hi)
{
    float z_spm = (float)spm;
    long steps = lroundf((float)edge * z_spm);
    float mlo = (float)(steps - below - 1) / z_spm, mhi = (float)(steps + above + 1) / z_spm;
    double offset = (double)steps / spm - edge;             /* machine less program */
    *lo = mlo - offset;
    *hi = mhi - offset;
}

static const int windows[][2] = { { 1, 1 }, { 10, 12 }, { 14, 20 }, { 18, 20 }, { 40, 40 }, { 40, 1 }, { 1, 40 } };
#define NWIN (int)(sizeof(windows) / sizeof(*windows))

static void test_window(void)
{
    int down, up;
    stub_edge = stub_below = stub_above = NULL;
    CHECK(lens_window(&down, &up) == 0 && down == LENS_WINDOW_DOWN && up == LENS_WINDOW_UP,
          "no settings: %d/%d, not the fallback", down, up);
    CHECK(fabs(lens_edge_z() - LENS_EDGE_Z_DEFAULT) < 1e-9, "no edge setting: %.2f", lens_edge_z());
    stub_edge = "2.90";
    stub_below = "14";
    stub_above = "20";
    CHECK(lens_window(&down, &up) == 1 && down == 14 && up == 20, "the card's window: %d/%d", down, up);
    CHECK(fabs(lens_edge_z() - 2.90) < 1e-9, "the card's edge: %.2f", lens_edge_z());
    /* One count out of range takes the fallback for that side, and the
     * window is then not the card's. */
    stub_below = "0";
    CHECK(lens_window(&down, &up) == 0 && down == LENS_WINDOW_DOWN && up == 20, "below 0: %d/%d", down, up);
    stub_below = "41";
    CHECK(lens_window(&down, &up) == 0 && down == LENS_WINDOW_DOWN && up == 20, "below 41: %d/%d", down, up);
    stub_below = "abc";
    CHECK(lens_window(&down, &up) == 0 && down == LENS_WINDOW_DOWN, "below abc: %d", down);
    stub_below = "40";
    stub_above = NULL;
    CHECK(lens_window(&down, &up) == 0 && down == 40 && up == LENS_WINDOW_UP, "above unset: %d/%d", down, up);
    stub_edge = "garbage";
    CHECK(fabs(lens_edge_z() - LENS_EDGE_Z_DEFAULT) < 1e-9, "bad edge text: %.2f", lens_edge_z());
    stub_edge = stub_below = stub_above = NULL;
    /* The bench reference machine's fallback reach, as the pages show it. */
    double lo, hi;
    lens_reach(LENS_EDGE_Z_DEFAULT, LENS_STEPS_PER_MM, LENS_WINDOW_DOWN, LENS_WINDOW_UP, &lo, &hi);
    CHECK(fabs(lo + 0.07) < 0.005 && fabs(hi - 7.46) < 0.005, "the fallback reach is %.2f..%.2f", lo, hi);
}

/* The reach inside the controller's envelope, by a half-step or more
 * at each end, at every edge the settings accept and every window; and
 * lens_envelope is the controller's. */
static void test_reach_in_envelope(void)
{
    const double spm = LENS_STEPS_PER_MM, half = 1.0 / spm;
    long checked = 0, bad = 0, mismatched = 0;
    for (int w = 0; w < NWIN; w++) {
        int down = windows[w][0], up = windows[w][1];
        for (int hundredths = -2000; hundredths <= 2000; hundredths++) {
            double edge = hundredths / 100.0, rlo, rhi, elo, ehi, clo, chi;
            lens_reach(edge, spm, down, up, &rlo, &rhi);
            lens_envelope(edge, spm, down, up, &elo, &ehi);
            controller_envelope(edge, spm, down, up, &clo, &chi);
            checked++;
            if (rlo < clo + 0.9 * half || rhi > chi - 0.9 * half) {
                if (bad++ < 3)
                    printf("  edge %.2f window %d/%d: reach %.4f..%.4f, controller %.4f..%.4f\n", edge,
                           down, up, rlo, rhi, clo, chi);
            }
            if (fabs(elo - clo) > 1e-3 || fabs(ehi - chi) > 1e-3)
                mismatched++;
        }
    }
    CHECK(bad == 0, "%ld of %ld reaches are not a half-step inside the controller's envelope", bad, checked);
    CHECK(mismatched == 0, "%ld of %ld envelopes differ from the controller's", mismatched, checked);
}

/* The ladder: n whole half-steps from the top of the window to the
 * bottom, never outside it, every height inside the reach as printed. */
static void test_ladder(void)
{
    enum { N = 12 };
    const double spm = LENS_STEPS_PER_MM;
    for (int w = 0; w < NWIN; w++) {
        int down = windows[w][0], up = windows[w][1], k[N];
        lens_ladder_k(down, up, N, k);
        CHECK(k[0] == up && k[N - 1] == -down, "window %d/%d: the ladder runs %d..%d", down, up, k[0], k[N - 1]);
        for (int hundredths = -2000; hundredths <= 2000; hundredths += 7) {
            double edge = hundredths / 100.0, lo, hi;
            lens_reach(edge, spm, down, up, &lo, &hi);
            for (int i = 0; i < N; i++) {
                if (i && k[i] > k[i - 1])
                    CHECK(0, "window %d/%d: line %d rises", down, up, i + 1);
                if (k[i] < -down || k[i] > up)
                    CHECK(0, "window %d/%d: line %d at %d half-steps leaves the window", down, up, i + 1, k[i]);
                char txt[24];
                snprintf(txt, sizeof(txt), "%.2f", lens_z_of_k(edge, spm, k[i]));
                double z = atof(txt);
                if (z < lo - 0.01 || z > hi + 0.01)
                    CHECK(0, "edge %.2f window %d/%d: line %d prints Z %s, reach %.2f..%.2f", edge, down, up,
                          i + 1, txt, lo, hi);
            }
        }
    }
    int one[1];
    lens_ladder_k(10, 12, 1, one);
    CHECK(one[0] == 12, "a one-line ladder sits at the top: %d", one[0]);
}

static void test_line_z(void)
{
    double z = -99;
    CHECK(lens_line_z("G0 Z7.46", &z) == 1 && fabs(z - 7.46) < 1e-9, "G0 Z: %d %.2f", lens_line_z("G0 Z7.46", &z), z);
    CHECK(lens_line_z("G1 X72.5 Z-1.44 F600", &z) == 1 && fabs(z + 1.44) < 1e-9, "G1 with X and F: %.2f", z);
    CHECK(lens_line_z("  G0 Z1", &z) == 1 && fabs(z - 1) < 1e-9, "leading blanks");
    CHECK(lens_line_z("G00 Z2.5", &z) == 1 && fabs(z - 2.5) < 1e-9, "G00");
    CHECK(lens_line_z("G92 X10.000 Y5.000 Z3.35", &z) == 0, "G92 is not a move");
    CHECK(lens_line_z("G0 X0 Y0", &z) == 0, "no Z word");
    CHECK(lens_line_z("M3 S600", &z) == 0, "M3");
    CHECK(lens_line_z("$X", &z) == 0, "$X");
    CHECK(lens_line_z("G0 Z", &z) == 0, "Z without a number");
    CHECK(lens_line_z("G0 Zabc", &z) == 0, "Z with text");
    CHECK(lens_line_z("G0 X1 ;Z9", &z) == 0, "Z in a comment");
    CHECK(lens_line_z(";record", &z) == 0, "a directive");
    CHECK(lens_line_z("G4 P5", &z) == 0, "a dwell");
}

int main(void)
{
    test_window();
    test_reach_in_envelope();
    test_ladder();
    test_line_z();
    if (fails) {
        printf("lens_test: %d failures\n", fails);
        return 1;
    }
    printf("lens_test: ok\n");
    return 0;
}
