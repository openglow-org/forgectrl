/*
 * coolant.h - the coolant thermistors' counts: the conversion, and the
 * counts that are no temperature
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * One rule for every reader of pic/water_temp_1 and pic/water_temp_2 (the
 * cooling engine, the status page, the flow watch's means), the same rule
 * the cloud client applies (Sensors, "Coolant thermistors"):
 *
 * - a count of 0 or less: by the circuit, a shorted thermistor holds its
 *   divider at ground;
 * - a count of 1023 or more: the 10-bit converter's rail, where an open or
 *   unplugged thermistor sits. The attribute is a 16-bit register, so a
 *   garbled frame can carry any count up to 65535; this is judged on the
 *   count itself (the 0 C bound below covers it too, since the conversion
 *   maps every count from 983 up under 0 C);
 * - a count that converts under 0 C (982.94 and up): an open sensor can
 *   read a few counts under the rail, and a loop that cold is not fit to
 *   run either.
 *
 * Every rule is judged on the count as the sensor gave it, before the
 * air-assist lift is taken off and before the per-machine correction is
 * added: either one moves an open sensor's counts back over 0 C.
 */
#ifndef FORGECTRL_COOLANT_H
#define FORGECTRL_COOLANT_H

#include <math.h>

/* The converter's rail: an open thermistor's count. */
#define COOLANT_RAIL 1023

/* The factory conversion (10k B3380 NTC behind a 10k divider and a 1.3
 * gain, 10-bit converter). Counts at or past either end of the divider
 * have no resistance to convert and map to -273.15. */
static inline double coolant_c_of_counts(double raw)
{
    const double adc_f = 1024.0 * 1.3;
    if (raw <= 0.0 || raw >= adc_f)
        return -273.15;
    double r = 10000.0 / (adc_f / raw - 1.0);
    double rinf = 10000.0 * exp(-3380.0 / 298.15);
    return 3380.0 / log(r / rinf) - 273.15;
}

/* 1 when a count, or a mean of counts, is no temperature. */
static inline int coolant_no_reading_counts(double raw)
{
    if (!(raw > 0.0) || raw >= (double)COOLANT_RAIL)
        return 1;
    return coolant_c_of_counts(raw) < 0.0;
}

static inline int coolant_no_reading(long raw)
{
    return coolant_no_reading_counts((double)raw);
}

#endif
