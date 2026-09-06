/*
 * recordhtml_test.c - host test: the commissioning record as a page
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A record with every kind of content renders to a page that carries
 * the facts, the acknowledgment, the steps in catalog order with the
 * sentence, the settings written with their values before, the
 * numbers (a table for the dose points), the flags; every value is
 * escaped; an empty record renders too; nothing the record does not
 * hold appears.
 */
#include "../src/recordhtml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); failures++; } } while (0)

static const char record_text[] =
    "{\"schema\":1,\"created\":\"2026-09-04T20:00:00Z\",\"completed\":\"2026-09-06T18:55:00Z\","
    "\"sheet_id\":\"ABCDE-FGHIJ\","
    "\"advisories\":{\"safety-and-risk\":{\"hash\":\"0123456789abcdef0123456789abcdef\","
    "\"accepted\":\"2026-09-04T20:01:00Z\",\"method\":\"typed\"},"
    "\"licenses\":{\"hash\":\"ffff\",\"accepted\":\"2026-09-04T20:02:00Z\",\"method\":\"check\"}},"
    "\"acceptance\":{\"pressed_at\":\"2026-09-04T20:03:00Z\"},"
    "\"account\":{\"name\":\"owner\",\"uid\":1000,\"created\":\"2026-09-04T20:04:00Z\"},"
    "\"machine\":{\"model\":\"pro\",\"tec\":true,\"camera\":\"ov5648\",\"firmware\":\"0.0.1\","
    "\"build_kind\":\"dev\",\"head\":{\"hw_id\":\"3\",\"serial_hash\":\"HHHHH-HHHHH\","
    "\"version\":\"1.2\"}},"
    "\"wizards\":{"
    "\"laser.focus\":{\"version\":1,\"completed\":\"2026-09-06T13:24:00Z\","
    "\"result\":{\"summary\":\"Line 8 was sharpest on 2.8 mm; the focus <sits> 3.48 mm up.\","
    "\"pick\":8,\"thickness_mm\":2.794,\"stops\":{\"found\":true,\"below\":14,\"above\":20},"
    "\"z_passes\":[{\"pass\":1,\"steps\":18},{\"pass\":2,\"steps\":18}]},"
    "\"applied\":{\"lens_hall_edge_z_mm\":{\"from\":\"3.35\",\"to\":\"3.48\"},"
    "\"lens_stop_below_steps\":{\"from\":\"\",\"to\":\"14\"}}},"
    "\"sensors\":{\"version\":1,\"completed\":\"2026-09-04T22:00:00Z\","
    "\"result\":{\"coolant_up_c\":21.5,\"ok\":true},\"applied\":{}},"
    "\"laser.dose-curve\":{\"version\":1,\"completed\":\"2026-09-06T14:00:00Z\","
    "\"result\":{\"summary\":\"The curve rises rung by rung.\","
    "\"points\":[{\"x\":10,\"y\":0.44},{\"x\":100,\"y\":100}]},"
    "\"applied\":{\"laser_dose_curve\":{\"from\":\"\",\"to\":\"10:0.44,100:100\"}}}},"
    "\"flags\":{\"sensors\":{\"level\":\"recommended\",\"reason\":\"the machine was serviced\"},"
    "\"airflow\":{\"level\":\"required\",\"reason\":\"a fan was replaced\"}}}";

static json_t *pairs(const char *const *ids, const char *const *titles, size_t n)
{
    json_t *a = json_array();
    for (size_t i = 0; i < n; i++)
        json_array_append_new(a, json_pack("{s:s,s:s}", "id", ids[i], "title", titles[i]));
    return a;
}

/* Where `a` appears in `page`, or -1. */
static long at(const char *page, const char *a)
{
    const char *p = strstr(page, a);
    return p ? (long)(p - page) : -1;
}

int main(void)
{
    static const char *const doc_ids[] = { "safety-and-risk", "licenses", "privacy" };
    static const char *const doc_titles[] = { "Safety and risk", "Licenses", "Privacy" };
    static const char *const wiz_ids[] = { "sensors", "airflow", "laser.focus", "laser.dose-curve",
                                           "cooling.flow-load" };
    static const char *const wiz_titles[] = { "Sensors", "Airflow", "Focus", "Dose curve",
                                              "Flow under load" };
    json_t *docs = pairs(doc_ids, doc_titles, 3), *wiz = pairs(wiz_ids, wiz_titles, 5);
    json_error_t err;
    json_t *rec = json_loads(record_text, 0, &err);
    CHECK(rec != NULL, "the fixture parses");

    char *page = record_html(rec, docs, wiz, "0.0.1-dev");
    CHECK(page != NULL, "the page renders");
    if (!page)
        return 1;
    CHECK(!strncmp(page, "<!doctype html>", 15) && strstr(page, "</html>"), "a whole document");
    CHECK(!strstr(page, "<script"), "no script");
    CHECK(strstr(page, "ABCDE-FGHIJ") && strstr(page, "0.0.1-dev"), "the sheet id and the version");
    CHECK(strstr(page, "<td>pro</td>") && strstr(page, "fitted") && strstr(page, "HHHHH-HHHHH"),
          "the machine facts");

    /* the acknowledgment: titles from the list, the short hash, the method, the press */
    CHECK(strstr(page, "Safety and risk") && strstr(page, "0123456789abcdef</span>") &&
          strstr(page, "typed") && strstr(page, "checked") && strstr(page, "2026-09-04T20:03:00Z"),
          "the acknowledgment with the press");
    CHECK(strstr(page, "<th>Privacy</th><td>not accepted"), "a document not in the record says so");

    /* the steps in catalog order, only the ones in the record */
    long s = at(page, "<h3>Sensors"), f = at(page, "<h3>Focus"), d = at(page, "<h3>Dose curve");
    CHECK(s >= 0 && f > s && d > f, "the steps follow the catalog order");
    CHECK(at(page, "<h3>Airflow") < 0 && at(page, "<h3>Flow under load") < 0,
          "a wizard the record lacks has no section");
    CHECK(strstr(page, "the focus &lt;sits&gt; 3.48 mm up"), "the sentence, escaped");
    CHECK(strstr(page, "lens_hall_edge_z_mm</span> = 3.48 <span class=\"was\">(was 3.35)"),
          "a setting written with its value before");
    CHECK(strstr(page, "lens_stop_below_steps</span> = 14</div>"),
          "a setting with no value before shows none");
    CHECK(strstr(page, "<th>thickness mm</th><td>2.794</td>"), "a number as a fact");
    CHECK(strstr(page, "<th>stops</th><td>found yes; below 14; above 20</td>"),
          "an object as one row");
    CHECK(strstr(page, "<table class=\"inner\"><tr><th>x</th><th>y</th></tr>"
                       "<tr><td>10</td><td>0.44</td></tr>"),
          "an array of records as a table");
    CHECK(strstr(page, "<th>pass</th><th>steps</th>"), "the lens passes as a table too");
    CHECK(strstr(page, "<th>ok</th><td>yes</td>"), "a boolean as yes");
    CHECK(strstr(page, "sensors") == NULL || strstr(page, "<h3>Sensors"), "ids stay out of the page");

    /* the flags: on a step that ran, and on one that never did */
    CHECK(strstr(page, "Asked for again (recommended): the machine was serviced"),
          "a flag on a completed step");
    CHECK(strstr(page, "<h2>Asked for</h2>") && strstr(page, "<th>Airflow</th><td>required: a fan was replaced"),
          "a flag on a step that never ran, by its title");
    CHECK(strstr(page, "no serial number"), "the footer says what the page carries");
    free(page);

    /* an empty record */
    json_t *empty = json_object();
    page = record_html(empty, docs, wiz, "");
    CHECK(page && strstr(page, "No step has completed yet") && strstr(page, "not accepted") &&
          strstr(page, "not yet") && !strstr(page, "Account"),
          "an empty record renders with nothing invented");
    free(page);
    json_decref(empty);

    /* a record whose values carry markup */
    json_t *hostile = json_pack("{s:s,s:{s:{s:i,s:s,s:{s:s}}}}", "sheet_id", "<b>x</b>",
                                "wizards", "sensors", "version", 1, "completed", "t",
                                "result", "note", "\"quoted\" & <tagged>");
    page = record_html(hostile, docs, wiz, "<v>");
    CHECK(page && !strstr(page, "<b>x</b>") && strstr(page, "&lt;b&gt;x&lt;/b&gt;") &&
          strstr(page, "&quot;quoted&quot; &amp; &lt;tagged&gt;") && !strstr(page, "<v>"),
          "every value is escaped");
    free(page);
    json_decref(hostile);

    json_decref(rec);
    json_decref(docs);
    json_decref(wiz);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
