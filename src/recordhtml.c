/*
 * recordhtml.c - the commissioning record as a printable page
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The page the owner prints or saves beside the sheet: the machine's
 * facts, the acknowledgment, and every step with its sentence, the
 * settings it wrote (each with the value before), and the numbers
 * behind it. Everything in the record is text the daemon wrote itself,
 * but every value is escaped on the way out all the same. The record
 * carries no serial number, no hostname, and no secret, so the page
 * carries none either.
 */
#define _GNU_SOURCE
#include "recordhtml.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------ a string builder */

typedef struct {
    char *s;
    size_t n, cap;
    int oom;
} sb_t;

static void sb_grow(sb_t *b, size_t more)
{
    if (b->oom)
        return;
    if (b->n + more + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + more + 1)
        cap *= 2;
    char *s = realloc(b->s, cap);
    if (!s) {
        b->oom = 1;
        return;
    }
    b->s = s;
    b->cap = cap;
}

static void sb_put(sb_t *b, const char *t)
{
    size_t m = strlen(t);
    sb_grow(b, m);
    if (b->oom)
        return;
    memcpy(b->s + b->n, t, m + 1);
    b->n += m;
}

/* Text: the five characters HTML reads specially become entities. */
static void sb_esc(sb_t *b, const char *t)
{
    for (; t && *t; t++) {
        switch (*t) {
        case '&': sb_put(b, "&amp;"); break;
        case '<': sb_put(b, "&lt;"); break;
        case '>': sb_put(b, "&gt;"); break;
        case '"': sb_put(b, "&quot;"); break;
        case '\'': sb_put(b, "&#39;"); break;
        default: {
            char c[2] = { *t, 0 };
            sb_put(b, c);
        }
        }
    }
}

static void sb_printf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sb_t *b, const char *fmt, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    sb_put(b, text);
}

/* ------------------------------------------------------------- values */

static const char *sget(const json_t *o, const char *k)
{
    const char *v = json_string_value(json_object_get(o, k));
    return v ? v : "";
}

/* A key as words: underscores to spaces, a trailing _mm kept (the
 * unit is part of the fact). */
static void put_key(sb_t *b, const char *k)
{
    for (; *k; k++) {
        char c[2] = { *k == '_' ? ' ' : *k, 0 };
        sb_esc(b, c);
    }
}

static void put_scalar(sb_t *b, const json_t *v)
{
    if (json_is_string(v))
        sb_esc(b, json_string_value(v));
    else if (json_is_integer(v))
        sb_printf(b, "%lld", (long long)json_integer_value(v));
    else if (json_is_real(v))
        sb_printf(b, "%.6g", json_real_value(v));
    else if (json_is_true(v))
        sb_put(b, "yes");
    else if (json_is_false(v))
        sb_put(b, "no");
    else if (json_is_null(v))
        sb_put(b, "\xe2\x80\x94");
}

/* An array of objects with scalar fields renders as a table with the
 * first element's keys as its columns (the dose points, the lens
 * passes); any other array is its values in a row. */
static int table_shaped(const json_t *a)
{
    if (!json_is_array(a) || json_array_size(a) == 0)
        return 0;
    size_t i;
    json_t *e;
    json_array_foreach(a, i, e) {
        if (!json_is_object(e))
            return 0;
        const char *k;
        json_t *v;
        json_object_foreach((json_t *)e, k, v)
            if (json_is_object(v) || json_is_array(v))
                return 0;
    }
    return 1;
}

static void put_value(sb_t *b, const json_t *v, int depth);

static void put_array(sb_t *b, const json_t *a, int depth)
{
    if (table_shaped(a)) {
        sb_put(b, "<table class=\"inner\"><tr>");
        const char *k;
        json_t *x;
        json_object_foreach(json_array_get(a, 0), k, x) {
            sb_put(b, "<th>");
            put_key(b, k);
            sb_put(b, "</th>");
        }
        sb_put(b, "</tr>");
        size_t i;
        json_t *e;
        json_array_foreach(a, i, e) {
            sb_put(b, "<tr>");
            json_object_foreach(json_array_get(a, 0), k, x) {
                sb_put(b, "<td>");
                put_scalar(b, json_object_get(e, k));
                sb_put(b, "</td>");
            }
            sb_put(b, "</tr>");
        }
        sb_put(b, "</table>");
        return;
    }
    size_t i;
    json_t *e;
    json_array_foreach(a, i, e) {
        if (i)
            sb_put(b, ", ");
        put_value(b, e, depth + 1);
    }
}

static void put_value(sb_t *b, const json_t *v, int depth)
{
    if (json_is_array(v))
        put_array(b, v, depth);
    else if (json_is_object(v)) {
        if (depth >= 2) {
            char *t = json_dumps(v, JSON_COMPACT);
            sb_esc(b, t ? t : "");
            free(t);
            return;
        }
        const char *k;
        json_t *x;
        int first = 1;
        json_object_foreach((json_t *)v, k, x) {
            if (!first)
                sb_put(b, "; ");
            first = 0;
            put_key(b, k);
            sb_put(b, " ");
            put_value(b, x, depth + 1);
        }
    } else
        put_scalar(b, v);
}

/* The facts of a result as rows, the sentence and the chart points
 * having been shown already. */
static void put_facts(sb_t *b, const json_t *r)
{
    const char *k;
    json_t *v;
    int any = 0;
    json_object_foreach((json_t *)r, k, v)
        if (strcmp(k, "summary"))
            any = 1;
    if (!any)
        return;
    sb_put(b, "<table class=\"facts\">");
    json_object_foreach((json_t *)r, k, v) {
        if (!strcmp(k, "summary"))
            continue;
        sb_put(b, "<tr><th>");
        put_key(b, k);
        sb_put(b, "</th><td>");
        put_value(b, v, 0);
        sb_put(b, "</td></tr>");
    }
    sb_put(b, "</table>");
}

static void put_row(sb_t *b, const char *k, const char *v)
{
    if (!v || !*v)
        return;
    sb_put(b, "<tr><th>");
    sb_esc(b, k);
    sb_put(b, "</th><td>");
    sb_esc(b, v);
    sb_put(b, "</td></tr>");
}

/* The title of an id from a {id, title} array, or the id itself. */
static const char *title_of(const json_t *list, const char *id)
{
    size_t i;
    json_t *e;
    json_array_foreach(list, i, e)
        if (!strcmp(sget(e, "id"), id))
            return sget(e, "title");
    return id;
}

/* -------------------------------------------------------------- page */

static const char css[] =
    "body{font:14px/1.5 -apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;"
    "color:#1c1e26;background:#fff;margin:0;padding:28px 32px;max-width:900px}"
    "h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:26px 0 8px;"
    "border-bottom:1px solid #d8d9e0;padding-bottom:4px}h3{font-size:15px;margin:18px 0 4px}"
    ".sub{color:#5a5d6b;margin:0 0 18px}.mono{font-family:ui-monospace,Menlo,Consolas,monospace}"
    "table.facts{border-collapse:collapse;margin:6px 0 10px}table.facts th{text-align:left;"
    "font-weight:500;color:#5a5d6b;padding:2px 14px 2px 0;vertical-align:top;white-space:nowrap}"
    "table.facts td{padding:2px 0;vertical-align:top}"
    "table.inner{border-collapse:collapse;font-size:12.5px}table.inner th,table.inner td{"
    "border:1px solid #d8d9e0;padding:1px 6px;text-align:right}table.inner th{font-weight:500;"
    "color:#5a5d6b}"
    ".summary{margin:4px 0 8px;font-size:15px}.written{margin:4px 0 8px}"
    ".written b{display:block;color:#5a5d6b;font-weight:500}.was{color:#5a5d6b}"
    ".meta{color:#5a5d6b;font-size:12.5px}.flag{color:#a33}"
    ".foot{margin-top:30px;color:#5a5d6b;font-size:12px;border-top:1px solid #d8d9e0;padding-top:8px}"
    "@media print{body{padding:0}h2{break-after:avoid}h3{break-after:avoid}"
    "section{break-inside:avoid}}";

char *record_html(const json_t *rec, const json_t *docs, const json_t *wizards,
                  const char *version)
{
    sb_t b = { 0 };
    const char *sid = sget(rec, "sheet_id");
    const json_t *machine = json_object_get(rec, "machine");
    const json_t *head = json_object_get(machine, "head");

    sb_put(&b, "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
               "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>Commissioning record ");
    sb_esc(&b, sid);
    sb_put(&b, "</title><style>");
    sb_put(&b, css);
    sb_put(&b, "</style></head><body>\n<h1>ForgeFIRM commissioning record</h1>\n<p class=\"sub\">");
    if (*sid) {
        sb_put(&b, "Sheet id <span class=\"mono\">");
        sb_esc(&b, sid);
        sb_put(&b, "</span>");
    }
    if (version && *version) {
        sb_put(&b, *sid ? " &middot; firmware " : "Firmware ");
        sb_esc(&b, version);
    }
    sb_put(&b, "</p>\n<table class=\"facts\">");
    put_row(&b, "Created", sget(rec, "created"));
    put_row(&b, "Completed", sget(rec, "completed"));
    put_row(&b, "Model", sget(machine, "model"));
    if (json_is_boolean(json_object_get(machine, "tec")))
        put_row(&b, "Thermoelectric cooler", json_is_true(json_object_get(machine, "tec")) ? "fitted" : "none");
    put_row(&b, "Camera", sget(machine, "camera"));
    put_row(&b, "Head", sget(head, "hw_id"));
    put_row(&b, "Head id", sget(head, "serial_hash"));
    put_row(&b, "Head version", sget(head, "version"));
    put_row(&b, "Firmware at the setup", sget(machine, "firmware"));
    put_row(&b, "Build", sget(machine, "build_kind"));
    sb_put(&b, "</table>\n");

    /* The acknowledgment */
    sb_put(&b, "<h2>Operator acknowledgment</h2>\n<table class=\"facts\">");
    const json_t *acc = json_object_get(rec, "advisories");
    size_t i;
    json_t *d;
    json_array_foreach(docs, i, d) {
        const json_t *a = json_object_get(acc, sget(d, "id"));
        sb_put(&b, "<tr><th>");
        sb_esc(&b, sget(d, "title"));
        sb_put(&b, "</th><td>");
        if (a) {
            sb_esc(&b, sget(a, "accepted"));
            sb_put(&b, " <span class=\"meta\">(");
            sb_esc(&b, !strcmp(sget(a, "method"), "typed") ? "typed" : "checked");
            sb_put(&b, ", hash <span class=\"mono\">");
            const char *h = sget(a, "hash");
            char shorth[17];
            snprintf(shorth, sizeof(shorth), "%.16s", h);
            sb_esc(&b, shorth);
            sb_put(&b, "</span>)</span>");
        } else
            sb_put(&b, "not accepted");
        sb_put(&b, "</td></tr>");
    }
    const char *pressed = sget(json_object_get(rec, "acceptance"), "pressed_at");
    put_row(&b, "The button press", *pressed ? pressed : "not yet");
    sb_put(&b, "</table>\n");

    /* The account */
    const json_t *account = json_object_get(rec, "account");
    if (json_is_object(account)) {
        sb_put(&b, "<h2>Account</h2>\n<table class=\"facts\">");
        put_row(&b, "Name", sget(account, "name"));
        put_row(&b, "Created", sget(account, "created"));
        sb_put(&b, "</table>\n");
    }

    /* The steps, in catalog order */
    const json_t *wz = json_object_get(rec, "wizards");
    const json_t *flags = json_object_get(rec, "flags");
    sb_put(&b, "<h2>The steps</h2>\n");
    int any = 0;
    json_t *w;
    json_array_foreach(wizards, i, w) {
        const char *id = sget(w, "id");
        const json_t *e = json_object_get(wz, id);
        if (!json_is_object(e))
            continue;
        any = 1;
        sb_put(&b, "<section><h3>");
        sb_esc(&b, sget(w, "title"));
        sb_printf(&b, " <span class=\"meta\">v%lld, ",
                  (long long)json_integer_value(json_object_get(e, "version")));
        sb_esc(&b, sget(e, "completed"));
        sb_put(&b, "</span></h3>\n");
        const json_t *flag = json_object_get(flags, id);
        if (json_is_object(flag)) {
            sb_put(&b, "<p class=\"flag\">Asked for again (");
            sb_esc(&b, sget(flag, "level"));
            if (*sget(flag, "reason")) {
                sb_put(&b, "): ");
                sb_esc(&b, sget(flag, "reason"));
            } else
                sb_put(&b, ")");
            sb_put(&b, "</p>\n");
        }
        const json_t *r = json_object_get(e, "result");
        if (*sget(r, "summary")) {
            sb_put(&b, "<p class=\"summary\">");
            sb_esc(&b, sget(r, "summary"));
            sb_put(&b, "</p>\n");
        }
        const json_t *ap = json_object_get(e, "applied");
        if (json_is_object(ap) && json_object_size(ap)) {
            sb_put(&b, "<div class=\"written\"><b>Written to the settings</b>");
            const char *k;
            json_t *v;
            json_object_foreach((json_t *)ap, k, v) {
                sb_put(&b, "<div><span class=\"mono\">");
                sb_esc(&b, k);
                sb_put(&b, "</span> = ");
                put_scalar(&b, json_object_get(v, "to"));
                const json_t *from = json_object_get(v, "from");
                if (from && !json_is_null(from) &&
                    !(json_is_string(from) && !*json_string_value(from)) &&
                    !json_equal((json_t *)from, json_object_get(v, "to"))) {
                    sb_put(&b, " <span class=\"was\">(was ");
                    put_scalar(&b, from);
                    sb_put(&b, ")</span>");
                }
                sb_put(&b, "</div>");
            }
            sb_put(&b, "</div>\n");
        }
        if (json_is_object(r))
            put_facts(&b, r);
        sb_put(&b, "</section>\n");
    }
    if (!any)
        sb_put(&b, "<p class=\"meta\">No step has completed yet.</p>\n");

    /* Flags on wizards that never ran (a change reported before the run) */
    int flagged_unrun = 0;
    const char *k;
    json_t *f;
    json_object_foreach((json_t *)flags, k, f) {
        if (json_is_object(json_object_get(wz, k)))
            continue;
        if (!flagged_unrun) {
            sb_put(&b, "<h2>Asked for</h2>\n<table class=\"facts\">");
            flagged_unrun = 1;
        }
        sb_put(&b, "<tr><th>");
        sb_esc(&b, title_of(wizards, k));
        sb_put(&b, "</th><td>");
        sb_esc(&b, sget(f, "level"));
        if (*sget(f, "reason")) {
            sb_put(&b, ": ");
            sb_esc(&b, sget(f, "reason"));
        }
        sb_put(&b, "</td></tr>");
    }
    if (flagged_unrun)
        sb_put(&b, "</table>\n");

    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M UTC", &tm);
    sb_put(&b, "<p class=\"foot\">Made ");
    sb_esc(&b, ts);
    sb_put(&b, ". Times are UTC. The record carries no serial number, no network name, and "
               "no credential; the sheet id and the head id are salted hashes that only this "
               "machine can reproduce.</p>\n</body></html>\n");
    if (b.oom) {
        free(b.s);
        return NULL;
    }
    return b.s;
}
