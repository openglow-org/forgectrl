/*
 * wizcalc_test.c - host test: the dark wizards' decisions
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#include "../src/wizcalc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); failures++; } } while (0)

/* A fan that spins up over 4 s to about 12000 rpm, sampled at 8 Hz for 35 s. */
static int fan_trace(double *rpm, double steady, double spin_s, double hz, double secs)
{
    int n = (int)(hz * secs);
    for (int i = 0; i < n; i++) {
        double t = (double)i / hz;
        double v = t >= spin_s ? steady : steady * (t / spin_s);
        rpm[i] = v + ((i % 3) - 1) * 20.0;      /* a little tach jitter */
    }
    return n;
}

int main(void)
{
    double rpm[400];
    wizcalc_fan_t f;

    int n = fan_trace(rpm, 12000.0, 4.0, 8.0, 35.0);
    CHECK(wizcalc_fan_analyze(rpm, n, 8.0, 1000.0, &f) == 0, "a 35 s trace is judged");
    CHECK(fabs(f.steady_rpm - 12000.0) <= 25.0, "steady is the median of the last 10 s");
    CHECK(f.spinup_s >= 3.4 && f.spinup_s <= 3.8, "spin-up is the time to 90 percent");
    CHECK(f.ok, "a fan at 12000 rpm is above the 1000 rpm minimum");

    n = fan_trace(rpm, 600.0, 2.0, 8.0, 35.0);
    wizcalc_fan_analyze(rpm, n, 8.0, 1000.0, &f);
    CHECK(!f.ok, "a fan at 600 rpm fails the minimum");

    CHECK(wizcalc_fan_analyze(rpm, 40, 8.0, 1000.0, &f) == -1, "a 5 s trace is too short");

    CHECK(fabs(wizcalc_floor(12000.0, 0.55, 1000.0, 20000.0) - 6600.0) < 1e-9,
          "the floor is 55 percent of steady");
    CHECK(wizcalc_floor(12000.0, 0.55, 8000.0, 20000.0) == 8000.0, "the floor clamps to the low end");
    CHECK(wizcalc_floor(60000.0, 0.55, 1000.0, 20000.0) == 20000.0, "the floor clamps to the high end");
    CHECK(wizcalc_grace_s(3.6, 5.0, 60.0) == 8.6, "the grace is spin-up plus 5 s");
    CHECK(wizcalc_grace_s(0.0, 5.0, 60.0) == 5.0, "the grace clamps low");

    CHECK(wizcalc_next_duty(0.0) == 40.0 && wizcalc_next_duty(40.0) == 55.0
          && wizcalc_next_duty(55.0) == 70.0 && wizcalc_next_duty(70.0) < 0.0,
          "the heater ladder is 40, 55, 70, then stop");

    wizcalc_sensors_t s = {
        .down_c = 22.4, .up_c = 21.9, .chassis_c = 28.0, .soc_c = 45.0,
        .ir = { 110, 120, 105, 118 }, .ir_alert = 275.0, .accel_events = 0,
        .pgood = 1, .hv_current = 0, .tach_exhaust = 3400.0, .tach_intake = 2200.0,
    };
    char why[256];
    CHECK(wizcalc_sensors_judge(&s, why, sizeof(why)) == 0, "a machine at rest reads plausible");
    CHECK(why[0] == '\0', "no reason when everything is fine");

    s.up_c = 24.5;
    CHECK(wizcalc_sensors_judge(&s, why, sizeof(why)) == 1 && strstr(why, "disagree"),
          "coolant sensors 2.6 C apart are flagged");
    s.up_c = 21.9;
    s.ir[2] = 300;
    s.pgood = 0;
    s.hv_current = 40;
    int bad = wizcalc_sensors_judge(&s, why, sizeof(why));
    CHECK(bad == 3 && strstr(why, "lid IR 3") && strstr(why, "power-good") && strstr(why, "HV current"),
          "IR at the tier, no power-good, and HV current are three reasons");
    s.ir[2] = 105;
    s.pgood = 1;
    s.hv_current = 0;
    s.tach_exhaust = 0.0;
    CHECK(wizcalc_sensors_judge(&s, why, sizeof(why)) == 1 && strstr(why, "exhaust"),
          "a silent exhaust tach is flagged");
    s.tach_exhaust = 3400.0;
    s.chassis_c = NAN;
    s.soc_c = NAN;
    CHECK(wizcalc_sensors_judge(&s, why, sizeof(why)) == 0, "an unreadable chassis or SoC is skipped");
    s.down_c = NAN;
    CHECK(wizcalc_sensors_judge(&s, why, sizeof(why)) == 1 && strstr(why, "unreadable"),
          "an unreadable coolant sensor is a reason");

    long pos[5] = { 40, 42, 41, 40, 42 };
    CHECK(wizcalc_z_concur(pos, 5, 4), "passes within 4 steps agree");
    pos[4] = 60;
    CHECK(!wizcalc_z_concur(pos, 5, 4), "a pass 20 steps off does not agree");

    /* The lens stop finder, on the bench reference machine's ring: the
     * even half-steps from the edge ring at 15000 to 21000 summed, the
     * odd ones at 6000 to 8300, and at the stop every step goes quiet. */
    long ring[] = { 6293, 15002, 6356, 21091, 8287, 15972, 6821, 20591, 6237, 19000,
                    6000, 18500, 6100, 17000, 1148, 1271, 987, 2100 };
    int slipped = 1;
    CHECK(wizcalc_stop_call(ring, 4, &slipped) == 0, "nothing is judged before the fifth step");
    CHECK(wizcalc_stop_call(ring, 14, &slipped) == 0 && !slipped, "a free ladder calls nothing");
    CHECK(wizcalc_stop_call(ring, 15, &slipped) == 0, "a quiet odd step is the quiet parity, no call");
    CHECK(wizcalc_stop_call(ring, 16, &slipped) == 16 && !slipped,
          "the first strong-parity step at the quiet level calls contact");
    CHECK(wizcalc_stop_call(ring, 18, &slipped) == 16, "the call stands at the first such step");
    long burst[] = { 6293, 15002, 6356, 21091, 8000, 45000 };
    CHECK(wizcalc_stop_call(burst, 6, &slipped) == 6 && slipped,
          "a burst over 1.8 times the strong level calls contact as a slip");
    long odd[] = { 15000, 6000, 16000, 6100, 2000 };
    CHECK(wizcalc_stop_call(odd, 5, &slipped) == 5 && !slipped,
          "the strong parity is learned from the first two: the odd steps here");
    /* A minute after a burn, the fans at their run duty, the bench
     * reference machine's bottom leg (2026-09-06 18:04): the quiet
     * steps lifted to 6400 to 10800 and a free strong step at 14000
     * among 17200 to 19800, which a midpoint rule called a stop. */
    long warm[] = { 9800, 17200, 6400, 19800, 10800, 14000, 6600, 15000, 7000, 15500, 9000, 9500 };
    CHECK(wizcalc_stop_call(warm, 11, &slipped) == 0, "a weak free strong step after a burn is not a stop");
    CHECK(wizcalc_stop_call(warm, 12, &slipped) == 12 && !slipped,
          "a strong step at the quiet level after a burn is");
    CHECK(wizcalc_ring_readable(warm, 12, 6000), "a 16000 to 9800 ring after a burn reads (1.6 to 1)");
    /* The same run's top leg: a weak free strong step at 14100 among
     * 17100 to 22600 at step 16, the stop at 22. */
    long top[] = { 6100, 20400, 5800, 19800, 7100, 21100, 7900, 18500, 7100, 17300, 7000, 19100,
                   6500, 17100, 5200, 14100, 6900, 22600, 6400, 19900, 7100, 5900 };
    CHECK(wizcalc_stop_call(top, 21, &slipped) == 0, "the top leg rings to the stop");
    CHECK(wizcalc_stop_call(top, 22, &slipped) == 22 && !slipped, "and calls it at 22");
    CHECK(wizcalc_ring_readable(ring, 16, 6000), "strong steps at 15000 and up over 6000 read");
    long flat[] = { 3000, 3200, 2900, 3100, 3000, 3300 };
    CHECK(!wizcalc_ring_readable(flat, 6, 6000), "a flat ring is not readable");
    long weak[] = { 2000, 5000, 2100, 5200, 2000, 5100 };
    CHECK(!wizcalc_ring_readable(weak, 6, 6000), "a ring under the floor is not readable");
    long dull[] = { 9000, 12000, 9200, 12500, 9100, 12200 };
    CHECK(!wizcalc_ring_readable(dull, 6, 6000), "a ring under 1.5 to 1 is not readable");
    CHECK(!wizcalc_ring_readable(ring, 3, 6000), "one strong step is not enough to judge");

    CHECK(wizcalc_jog_witnessed(2102, 701, 232, 191, 2.5, 400), "an X jog at nine times rest is witnessed");
    CHECK(wizcalc_jog_witnessed(486, 1060, 232, 191, 2.5, 400), "a Y jog: the busier axis is the one judged");
    CHECK(!wizcalc_jog_witnessed(250, 230, 232, 191, 2.5, 400), "a rest-level reading is not");
    CHECK(!wizcalc_jog_witnessed(390, 120, 60, 50, 2.5, 400), "under the floor is not, whatever the ratio");
    CHECK(!wizcalc_jog_witnessed(1100, 500, 500, 480, 2.5, 400), "a noisy rest wants the ratio");

    /* One browser drives a run; a second mirrors it. */
    CHECK(wizcalc_may_act("abc", "abc"), "the owner acts");
    CHECK(!wizcalc_may_act("abc", "def"), "another session does not");
    CHECK(wizcalc_may_act("", "def"), "a run nobody owns takes anyone");
    CHECK(wizcalc_may_act(NULL, "def"), "no owner at all takes anyone");
    CHECK(wizcalc_may_act("abc", ""), "a tool with no session acts");
    CHECK(wizcalc_may_act("abc", NULL), "a tool with no session acts, NULL form");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
