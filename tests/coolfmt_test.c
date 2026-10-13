/*
 * coolfmt_test.c - host unit test for the /cool/status document
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The document is built from fixed-size fragments, and a fragment that
 * does not fit its buffer must turn into an empty one, never a cut-off
 * one: a client parses whatever the engine serves. Pinned here: the
 * widest legal rendering of every fragment (the far ends of the gate
 * table, a header-width floor, the longest names) fits the buffer the
 * engine uses; the widest document fits COOL_STATUS_JSON_MAX; the
 * whole thing parses as JSON (jansson); and an oversize value set is
 * refused rather than truncated.
 */
#include "../src/coolfmt.h"

#include <jansson.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* Whether s is one whole JSON document. */
static int is_json(const char *s)
{
    json_error_t e;
    json_t *j = json_loads(s, 0, &e);
    json_decref(j);
    return j != NULL;
}

/* A number at a path of object keys, or NAN. */
static double num_at(const char *s, const char *k1, const char *k2)
{
    json_error_t e;
    json_t *j = json_loads(s, 0, &e);
    json_t *v = json_object_get(j, k1);
    if (k2)
        v = json_object_get(v, k2);
    double d = json_is_number(v) ? json_number_value(v) : NAN;
    json_decref(j);
    return d;
}

/* A string at a path of object keys is want. */
static int str_at(const char *s, const char *k1, const char *k2, const char *want)
{
    json_error_t e;
    json_t *j = json_loads(s, 0, &e);
    json_t *v = json_object_get(json_object_get(j, k1), k2);
    int ok = json_is_string(v) && !strcmp(json_string_value(v), want);
    json_decref(j);
    return ok;
}

int main(void)
{
    char lim[COOL_LIMITS_JSON_MAX], fg[COOL_FAN_GATES_JSON_MAX];
    char doc[COOL_STATUS_JSON_MAX];

    printf("limits\n");
    /* Today's defaults: the set that was cut at 160 bytes. */
    cool_limits_t def = {33.0, 0.0, 3700, 1800, 6000};
    CHECK(coolfmt_limits(lim, sizeof(lim), 33.0, 31.0, 38.0, 0, &def) == 0, "the default limits fit");
    CHECK(is_json(lim) && num_at(lim, "air_assist_min_rpm", NULL) == 6000.0, "and parse, the air assist floor in them");
    /* The far ends of the gate table (gates.c): 60/59 C, floors 20000 /
     * 20000 / 30000, plus the header source. */
    cool_limits_t wide = {60.0, 60.0, 20000, 20000, 30000};
    CHECK(coolfmt_limits(lim, sizeof(lim), 60.0, 59.0, 70.0, 1, &wide) == 0, "the widest table values fit");
    CHECK(is_json(lim), "and parse");
    /* A header floor tightens past the table: a tach window of the
     * width the factory sends (64500), and an absurd one. */
    cool_limits_t hdr = {60.0, 60.0, 64500, 64500, 64500};
    CHECK(coolfmt_limits(lim, sizeof(lim), 60.0, 59.0, 70.0, 1, &hdr) == 0 && is_json(lim), "header-width floors fit");
    cool_limits_t absurd = {60.0, 60.0, 2147483647.0, 2147483647.0, 2147483647.0};
    CHECK(coolfmt_limits(lim, sizeof(lim), 60.0, 59.0, 70.0, 1, &absurd) == 0 && is_json(lim), "int32-width floors fit");
    CHECK(num_at(lim, "coolant_critical_c", NULL) == 70.0, "the critical line is in the limits");
    char tiny[64];
    CHECK(coolfmt_limits(tiny, sizeof(tiny), 33.0, 31.0, 38.0, 0, &def) < 0 && !strcmp(tiny, "{}"), "a fragment that does not fit is refused, and left empty");
    size_t widest_limits = strlen(lim);

    printf("fan gates\n");
    coolfmt_fan_t fans[5] = {
        {"exhaust",    99999, 2147483647.0, "TRIPPED"},
        {"intake_1",   99999, 2147483647.0, "TRIPPED"},
        {"intake_2",   99999, 2147483647.0, "TRIPPED"},
        {"air_assist", 99999, 2147483647.0, "TRIPPED"},
        {"purge",      99999, 2147483647.0, "TRIPPED"},
    };
    CHECK(coolfmt_fan_gates(fg, sizeof(fg), fans, 5) == 0, "five fans at their widest fit");
    CHECK(is_json(fg) && num_at(fg, "purge", "reading") == 99999.0 && num_at(fg, "purge", "floor") == 2147483647.0 &&
          str_at(fg, "purge", "state", "TRIPPED"), "and parse, the last row whole");
    CHECK(coolfmt_fan_gates(fg, sizeof(fg), fans, 0) == 0 && !strcmp(fg, "{}"), "no fans is the empty object");
    char small[100];
    CHECK(coolfmt_fan_gates(small, sizeof(small), fans, 5) < 0 && !strcmp(small, "{}"), "rows that do not fit are refused, not cut mid-row");
    coolfmt_fan_gates(fg, sizeof(fg), fans, 5);
    size_t widest_fans = strlen(fg);

    printf("document\n");
    char reason[COOL_REASON_MAX], gates_off[COOL_GATES_OFF_JSON_MAX];
    memset(reason, 'r', sizeof(reason) - 1);
    reason[sizeof(reason) - 1] = '\0';
    /* The widest gates_off list the engine can publish is every gate
     * key; pad a valid array out to the buffer's full width. */
    snprintf(gates_off, sizeof(gates_off), "[\"%0*d\"]", (int)sizeof(gates_off) - 5, 0);
    coolfmt_status_t st = {
        .phase = "cooldown", .verdict = "OVERTEMP", .reason = reason,
        .fire_watch = "armed", .accel_watch = "armed",
        .fire_ok = 0, .hold = 0, .armed = 0,
        .down_c = -273.15, .up_c = -273.15, .report_age_s = 99999999.9,
        .gates_off = gates_off, .limits = lim, .fan_gates = fg,
    };
    CHECK(coolfmt_status(doc, sizeof(doc), &st) == 0, "the widest document fits COOL_STATUS_JSON_MAX");
    CHECK(is_json(doc), "and parses");
    size_t scalars = strlen(doc) - strlen(reason) - strlen(gates_off) - widest_limits - widest_fans;
    printf("  widest: limits %zu, fan gates %zu, scalars+punctuation %zu, document %zu of %d\n",
           widest_limits, widest_fans, scalars, strlen(doc), COOL_STATUS_JSON_MAX);
    CHECK(scalars < COOL_STATUS_JSON_SCALARS, "the scalar allowance covers the widest scalars");
    CHECK(coolfmt_status(doc, 512, &st) < 0, "a 512-byte buffer is refused, not served cut off");

    /* The document parses with every fragment at its empty fallback. */
    st.gates_off = "[]"; st.limits = "{}"; st.fan_gates = "{}"; st.reason = "";
    CHECK(coolfmt_status(doc, sizeof(doc), &st) == 0 && is_json(doc), "empty fragments still make a document");

    printf("armed flag\n");
    CHECK(coolfmt_armed(1, 0.0, 5.0), "a fresh armed report shows armed");
    CHECK(coolfmt_armed(1, 5.0, 5.0), "at the timeout it still does");
    CHECK(!coolfmt_armed(1, 5.1, 5.0), "past the timeout a stale armed report shows false");
    CHECK(!coolfmt_armed(1, -1.0, 5.0), "and so does no report at all");
    CHECK(!coolfmt_armed(0, 0.0, 5.0), "a fresh unarmed report shows false");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
