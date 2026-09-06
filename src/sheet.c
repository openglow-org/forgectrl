/*
 * sheet.c - the commissioning sheet: paths, text, layout, and programs
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * See sheet.h. Everything here is arithmetic over a point list and a
 * text buffer; the host test draws every card and checks that each
 * stays inside its box and that the program parses.
 */
#define _GNU_SOURCE
#include "sheet.h"
#include "font_hershey.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The glyphs carry a left bearing in the table; text starts at its
 * pen position when that much is taken off every x. */
#define HERSHEY_LEFT 94

/* ------------------------------------------------------------ paths */

void sheet_paths_init(sheet_paths_t *s)
{
    memset(s, 0, sizeof(*s));
}

void sheet_paths_free(sheet_paths_t *s)
{
    free(s->p);
    memset(s, 0, sizeof(*s));
}

void sheet_offset(sheet_paths_t *s, float dx, float dy)
{
    s->dx = dx;
    s->dy = dy;
}

static void push(sheet_paths_t *s, float x, float y)
{
    if (s->oom)
        return;
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 256;
        sheet_pt_t *p = realloc(s->p, (size_t)cap * sizeof(*p));
        if (!p) {
            s->oom = 1;
            return;
        }
        s->p = p;
        s->cap = cap;
    }
    s->p[s->n].x = x;
    s->p[s->n].y = y;
    s->n++;
}

void sheet_move(sheet_paths_t *s, float x, float y)
{
    if (s->n && s->p[s->n - 1].x != SHEET_PEN_UP)
        push(s, SHEET_PEN_UP, SHEET_PEN_UP);
    push(s, x + s->dx, y + s->dy);
}

void sheet_to(sheet_paths_t *s, float x, float y)
{
    push(s, x + s->dx, y + s->dy);
}

void sheet_line(sheet_paths_t *s, float x0, float y0, float x1, float y1)
{
    sheet_move(s, x0, y0);
    sheet_to(s, x1, y1);
}

void sheet_rect(sheet_paths_t *s, float x, float y, float w, float h)
{
    sheet_move(s, x, y);
    sheet_to(s, x + w, y);
    sheet_to(s, x + w, y + h);
    sheet_to(s, x, y + h);
    sheet_to(s, x, y);
}

void sheet_circle(sheet_paths_t *s, float cx, float cy, float r)
{
    int n = r < 1.0f ? 12 : r < 4.0f ? 24 : 48;
    for (int i = 0; i <= n; i++) {
        float a = (float)(2.0 * M_PI * i / n);
        float x = cx + r * cosf(a), y = cy + r * sinf(a);
        if (i == 0)
            sheet_move(s, x, y);
        else
            sheet_to(s, x, y);
    }
}

void sheet_cross(sheet_paths_t *s, float cx, float cy, float arm)
{
    sheet_line(s, cx - arm, cy, cx + arm, cy);
    sheet_line(s, cx, cy - arm, cx, cy + arm);
}

static const hershey_glyph_t *glyph(unsigned char c)
{
    if (c < HERSHEY_FIRST || c >= HERSHEY_FIRST + HERSHEY_NGLYPH)
        c = '?';
    return &hershey_glyphs[c - HERSHEY_FIRST];
}

float sheet_text_width(float cap, const char *text)
{
    float scale = cap / HERSHEY_CAP, w = 0;
    for (const unsigned char *c = (const unsigned char *)text; *c; c++)
        w += glyph(*c)->adv * scale;
    return w;
}

float sheet_text(sheet_paths_t *s, float x, float y, float cap, const char *text, float max_w)
{
    float scale = cap / HERSHEY_CAP, pen = 0;
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
        const hershey_glyph_t *g = glyph(*c);
        if (max_w > 0 && pen + g->adv * scale > max_w + 0.01f)
            break;
        int up = 1;
        for (unsigned i = 0; i < g->count; i++) {
            const short *pt = hershey_pts[g->first + i];
            if (pt[0] == HERSHEY_PEN_UP) {
                up = 1;
                continue;
            }
            float px = x + pen + (pt[0] - HERSHEY_LEFT) * scale;
            float py = y - pt[1] * scale;
            if (up)
                sheet_move(s, px, py);
            else
                sheet_to(s, px, py);
            up = 0;
        }
        pen += g->adv * scale;
    }
    return pen;
}

/* The mark as the panel draws it: nine segments and a circle in a
 * box 63.5 wide and 15 tall (the panel SVG's units). */
void sheet_mark(sheet_paths_t *s, float x, float y, float h)
{
    static const float seg[9][4] = {
        { 34, 8, 90, 8 }, { 34, 0.5f, 34, 15.5f }, { 26.5f, 8, 41.5f, 8 },
        { 28.7f, 2.7f, 39.3f, 13.3f }, { 39.3f, 2.7f, 28.7f, 13.3f },
        { 31.1f, 1.1f, 36.9f, 14.9f }, { 36.9f, 1.1f, 31.1f, 14.9f },
        { 27.1f, 5.1f, 40.9f, 10.9f }, { 40.9f, 5.1f, 27.1f, 10.9f },
    };
    float k = h / 15.0f;
    for (int i = 0; i < 9; i++)
        sheet_line(s, x + (seg[i][0] - 26.5f) * k, y + (seg[i][1] - 0.5f) * k,
                   x + (seg[i][2] - 26.5f) * k, y + (seg[i][3] - 0.5f) * k);
    sheet_circle(s, x + (34 - 26.5f) * k, y + 7.5f * k, 2.0f * k);
}

int sheet_bounds(const sheet_paths_t *s, float *x0, float *y0, float *x1, float *y1)
{
    int any = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->p[i].x == SHEET_PEN_UP)
            continue;
        if (!any) {
            *x0 = *x1 = s->p[i].x;
            *y0 = *y1 = s->p[i].y;
            any = 1;
            continue;
        }
        if (s->p[i].x < *x0) *x0 = s->p[i].x;
        if (s->p[i].x > *x1) *x1 = s->p[i].x;
        if (s->p[i].y < *y0) *y0 = s->p[i].y;
        if (s->p[i].y > *y1) *y1 = s->p[i].y;
    }
    return any;
}

/* ------------------------------------------------------------ layout */

/* The header band is 32 tall under the frame's top edge; two rows of
 * cards follow with the gutter between. Row 1 holds the three ladders
 * (twelve lines at 3 mm, or five patterns at 7 mm, under the title);
 * row 2 the dose ladder (seven 100 mm rungs) and the flow patch
 * (60 x 15). Each card is its content plus the title and 2 mm of
 * margin; the rows fill the frame's width. */
#define ROW1_Y   46.0f
#define ROW2_Y   98.0f

static const sheet_card_t cards[] = {
    { "laser.focus",       "Focus",           10, ROW1_Y,  58, 48 },
    { "laser.floor",       "Laser floor",     72, ROW1_Y,  66, 48 },
    { "laser.corner",      "Corner rolloff", 142, ROW1_Y,  48, 48 },
    { "laser.dose-curve",  "Dose curve",      10, ROW2_Y, 112, 38 },
    { "cooling.flow-load", "Flow under load",126, ROW2_Y,  64, 38 },
};

size_t sheet_ncards(void)
{
    return sizeof(cards) / sizeof(*cards);
}

const sheet_card_t *sheet_card_at(size_t i)
{
    return i < sheet_ncards() ? &cards[i] : NULL;
}

const sheet_card_t *sheet_card(const char *id)
{
    for (size_t i = 0; i < sheet_ncards(); i++)
        if (!strcmp(cards[i].id, id))
            return &cards[i];
    return NULL;
}

void sheet_alone_offset(const sheet_card_t *c, float *dx, float *dy)
{
    *dx = SHEET_ALONE_MARGIN - c->x;
    *dy = SHEET_ALONE_MARGIN - c->y;
}

void sheet_alone_size(const sheet_card_t *c, float *w, float *h)
{
    *w = c->w + 2 * SHEET_ALONE_MARGIN;
    *h = c->h + 2 * SHEET_ALONE_MARGIN;
}

void sheet_card_box(sheet_paths_t *s, const sheet_card_t *c)
{
    sheet_rect(s, c->x, c->y, c->w, c->h);
    sheet_text(s, c->x + 2, c->y + 2 + SHEET_CAP_CARD, SHEET_CAP_CARD, c->title, c->w - 4);
}

#define LADDER_X0 10.0f     /* the ladder starts past the label column */
#define LADDER_X1  3.0f     /* and ends this far from the box's right edge */
float sheet_ladder_len(const sheet_card_t *c)
{
    return c->w - LADDER_X0 - LADDER_X1;
}

/* The header band: the mark with the site under it on the left, the
 * titles and the facts (the build on one line, the sheet and its start
 * on the next) to the right of it, clipped at the frame's edge. */
void sheet_frame(sheet_paths_t *s, const sheet_header_t *h)
{
    const float text_x = 58, text_w = SHEET_FRAME_X + SHEET_FRAME_W - 2 - text_x;
    sheet_rect(s, SHEET_FRAME_X, SHEET_FRAME_Y, SHEET_FRAME_W, SHEET_FRAME_H);
    sheet_mark(s, 12, 12, 10);
    sheet_text(s, 12, 37.5f, SHEET_CAP_BODY, "forgefirm.org", text_x - 14);
    sheet_text(s, text_x, 20, SHEET_CAP_TITLE, "OpenGlow ForgeFIRM", text_w);
    sheet_text(s, text_x, 27, SHEET_CAP_CARD, "Hardware Commissioning", text_w);
    char facts[160];
    snprintf(facts, sizeof(facts), "%s %s - %s - %s", h->version ? h->version : "",
             h->build ? h->build : "", h->model ? h->model : "", h->camera ? h->camera : "");
    sheet_text(s, text_x, 33, SHEET_CAP_BODY, facts, text_w);
    snprintf(facts, sizeof(facts), "sheet %s - started %s", h->sheet_id ? h->sheet_id : "",
             h->started ? h->started : "");
    sheet_text(s, text_x, 37.5f, SHEET_CAP_BODY, facts, text_w);
}

/* ------------------------------------------------------------ text out */

void sheet_text_init(sheet_text_t *t)
{
    memset(t, 0, sizeof(*t));
}

void sheet_text_free(sheet_text_t *t)
{
    free(t->buf);
    memset(t, 0, sizeof(*t));
}

static int text_reserve(sheet_text_t *t, size_t more)
{
    if (t->oom)
        return -1;
    if (t->len + more + 1 <= t->cap)
        return 0;
    size_t cap = t->cap ? t->cap : 4096;
    while (cap < t->len + more + 1)
        cap *= 2;
    char *b = realloc(t->buf, cap);
    if (!b) {
        t->oom = 1;
        return -1;
    }
    t->buf = b;
    t->cap = cap;
    return 0;
}

static int text_append(sheet_text_t *t, const char *s, size_t n)
{
    if (text_reserve(t, n) != 0)
        return -1;
    memcpy(t->buf + t->len, s, n);
    t->len += n;
    t->buf[t->len] = '\0';
    return 0;
}

int sheet_text_append(sheet_text_t *t, const char *s, size_t n)
{
    if (text_append(t, s, n) != 0)
        return -1;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n')
            t->lines++;
    return 0;
}

int sheet_text_line(sheet_text_t *t, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(line) - 1)
        return -1;
    line[n++] = '\n';
    if (text_append(t, line, (size_t)n) != 0)
        return -1;
    t->lines++;
    return 0;
}

static void fmt_num(char *buf, size_t len, float v)
{
    /* Three decimals, trailing zeros trimmed: short lines for the RX ring. */
    snprintf(buf, len, "%.3f", (double)v);
    char *dot = strchr(buf, '.');
    if (dot) {
        char *e = buf + strlen(buf) - 1;
        while (e > dot && *e == '0')
            *e-- = '\0';
        if (e == dot)
            *e = '\0';
    }
    if (!strcmp(buf, "-0"))
        strcpy(buf, "0");
}

void sheet_prog_paths(sheet_text_t *t, const sheet_paths_t *s, int from, int feed)
{
    int up = 1;
    char xs[24], ys[24];
    for (int i = from < 0 ? 0 : from; i < s->n; i++) {
        if (s->p[i].x == SHEET_PEN_UP) {
            up = 1;
            continue;
        }
        fmt_num(xs, sizeof(xs), s->p[i].x);
        fmt_num(ys, sizeof(ys), s->p[i].y);
        if (up)
            sheet_text_line(t, "G0 X%s Y%s", xs, ys);
        else
            sheet_text_line(t, "G1 X%s Y%s F%d", xs, ys, feed);
        up = 0;
    }
}

int sheet_svg(const sheet_paths_t *s, float w, float h, sheet_text_t *out)
{
    char line[96];
    snprintf(line, sizeof(line),
             "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %.1f %.1f\" "
             "width=\"%.0fmm\" height=\"%.0fmm\">\n", (double)w, (double)h, (double)w, (double)h);
    text_append(out, line, strlen(line));
    snprintf(line, sizeof(line),
             "<rect width=\"%.1f\" height=\"%.1f\" fill=\"#e7d3ac\" stroke=\"#9c8a66\" "
             "stroke-width=\"0.4\"/>\n", (double)w, (double)h);
    text_append(out, line, strlen(line));
    static const char path_open[] = "<path fill=\"none\" stroke=\"#3a2a12\" stroke-width=\"0.3\" "
                                    "stroke-linecap=\"round\" stroke-linejoin=\"round\" d=\"";
    text_append(out, path_open, sizeof(path_open) - 1);
    int up = 1;
    for (int i = 0; i < s->n; i++) {
        if (s->p[i].x == SHEET_PEN_UP) {
            up = 1;
            continue;
        }
        char xs[24], ys[24];
        fmt_num(xs, sizeof(xs), s->p[i].x);
        fmt_num(ys, sizeof(ys), s->p[i].y);
        snprintf(line, sizeof(line), "%s%s %s ", up ? "M" : "L", xs, ys);
        text_append(out, line, strlen(line));
        up = 0;
    }
    text_append(out, "\"/>\n</svg>\n", strlen("\"/>\n</svg>\n"));
    return out->oom ? -1 : 0;
}

double sheet_density_for_light(const char *curve, double light)
{
    double d[16], l[16];
    int n = 0;
    if (!curve || !*curve || !strcmp(curve, "off"))
        return light;
    const char *p = curve;
    while (*p && n < 16) {
        char *end;
        double dv = strtod(p, &end);
        if (end == p || *end != ':')
            return light;
        p = end + 1;
        double lv = strtod(p, &end);
        if (end == p)
            return light;
        d[n] = dv;
        l[n] = lv;
        n++;
        p = end;
        while (*p == ',' || *p == ' ')
            p++;
    }
    if (n < 2)
        return light;
    if (light <= l[0])
        return d[0] * (light / (l[0] > 0 ? l[0] : 1.0));
    for (int i = 1; i < n; i++) {
        if (light <= l[i]) {
            double span = l[i] - l[i - 1];
            double f = span > 0 ? (light - l[i - 1]) / span : 0.0;
            return d[i - 1] + f * (d[i] - d[i - 1]);
        }
    }
    return d[n - 1];
}

/* ------------------------------------------------------------ cards */

static void card_start(sheet_paths_t *pv, sheet_text_t *pg, const sheet_card_t *c, int text_s)
{
    sheet_card_box(pv, c);
    sheet_text_line(pg, "M4 S%d", text_s);
}

/* Everything drawn since `from` at the text dose. */
static void burn_since(sheet_paths_t *pv, sheet_text_t *pg, int from, int feed)
{
    sheet_prog_paths(pg, pv, from, feed);
}

static void num_label(sheet_paths_t *pv, const sheet_card_t *c, float y, const char *fmt, double v)
{
    char t[24];
    snprintf(t, sizeof(t), fmt, v);
    /* Four characters at the body cap ("1.25") need 8.1 mm. */
    sheet_text(pv, c->x + 2, y, SHEET_CAP_BODY, t, 10);
}

void sheet_build_frame(sheet_paths_t *pv, sheet_text_t *pg, const sheet_header_t *h,
                       int text_s, int feed)
{
    int from = pv->n;
    sheet_frame(pv, h);
    sheet_text_line(pg, "M4 S%d", text_s);
    burn_since(pv, pg, from, feed);
    sheet_text_line(pg, "M5");
}

void sheet_build_focus(sheet_paths_t *pv, sheet_text_t *pg, const sheet_card_t *c,
                       const double *z, int n, double z_text, int text_s, int feed)
{
    int from = pv->n;
    card_start(pv, pg, c, text_s);
    /* The lines are numbered from the top; the lens heights behind them
     * are the wizard's, reported with the pick. */
    for (int i = 0; i < n && i < SHEET_FOCUS_N; i++)
        num_label(pv, c, c->y + 11.0f + i * 3.0f, "%.0f", (double)(i + 1));
    sheet_text_line(pg, "G0 Z%.2f", z_text);
    burn_since(pv, pg, from, feed);
    /* The ladder: each line at its own Z, 20 mm at the focus dose, at
     * constant power (the text above rides M4). */
    sheet_text_line(pg, "M3 S%d", SHEET_FOCUS_S);
    for (int i = 0; i < n && i < SHEET_FOCUS_N; i++) {
        float y = c->y + 10.0f + i * 3.0f, x0 = c->x + LADDER_X0, x1 = x0 + sheet_ladder_len(c);
        sheet_line(pv, x0, y, x1, y);
        char ys[24], xa[24], xb[24];
        fmt_num(ys, sizeof(ys), y + pv->dy);
        fmt_num(xa, sizeof(xa), x0 + pv->dx);
        fmt_num(xb, sizeof(xb), x1 + pv->dx);
        sheet_text_line(pg, "G0 X%s Y%s", xa, ys);
        sheet_text_line(pg, "G0 Z%.2f", z[i]);
        sheet_text_line(pg, "G1 X%s F%d", xb, SHEET_FOCUS_FEED);
    }
    sheet_text_line(pg, "M5");
}

void sheet_build_floor(sheet_paths_t *pv, sheet_text_t *pg, const sheet_card_t *c,
                       const double *density, int n, int text_s, int feed)
{
    int from = pv->n;
    card_start(pv, pg, c, text_s);
    for (int i = 0; i < n && i < SHEET_FLOOR_N; i++)
        num_label(pv, c, c->y + 11.0f + i * 3.0f, "%.0f", density[i]);
    burn_since(pv, pg, from, feed);
    /* The text above burned under the machine's keys; the rungs need
     * the floor and the dose curve off (the wizard's directive). */
    sheet_text_line(pg, ";keys=floor-off");
    sheet_text_line(pg, "M3 S%d", (int)(density[0] * 10.0 + 0.5));
    for (int i = 0; i < n && i < SHEET_FLOOR_N; i++) {
        float y = c->y + 10.0f + i * 3.0f, x0 = c->x + LADDER_X0, x1 = x0 + sheet_ladder_len(c);
        sheet_line(pv, x0, y, x1, y);
        char ys[24], xa[24], xb[24];
        fmt_num(ys, sizeof(ys), y + pv->dy);
        fmt_num(xa, sizeof(xa), x0 + pv->dx);
        fmt_num(xb, sizeof(xb), x1 + pv->dx);
        sheet_text_line(pg, "G0 X%s Y%s", xa, ys);
        sheet_text_line(pg, "S%d", (int)(density[i] * 10.0 + 0.5));
        sheet_text_line(pg, "G1 X%s F%d", xb, SHEET_LADDER_FEED);
    }
    sheet_text_line(pg, "M5");
}

const double sheet_dose_density[SHEET_DOSE_N] = { 10, 20, 30, 45, 60, 80, 100 };

void sheet_build_dose(sheet_paths_t *pv, sheet_text_t *pg, const sheet_card_t *c,
                      int text_s, int feed)
{
    int from = pv->n;
    card_start(pv, pg, c, text_s);
    /* The rungs are labeled with their density, over the card's height. */
    for (int i = 0; i < SHEET_DOSE_N; i++)
        num_label(pv, c, c->y + 11.0f + i * SHEET_DOSE_PITCH, "%.0f", sheet_dose_density[i]);
    burn_since(pv, pg, from, feed);
    sheet_text_line(pg, ";keys=floor-off");
    sheet_text_line(pg, ";record");
    /* The recorder's ladder, one 100 mm line per rung, a dark dwell
     * between rungs so the fitter can tell them apart, at constant
     * power; the witnesses are recorded from here, not from the text. */
    sheet_text_line(pg, "M3 S%d", (int)(sheet_dose_density[0] * 10.0));
    for (int i = 0; i < SHEET_DOSE_N; i++) {
        float y = c->y + 10.0f + i * SHEET_DOSE_PITCH, x0 = c->x + 10, x1 = c->x + 110;
        sheet_line(pv, x0, y, x1, y);
        char ys[24], xa[24], xb[24];
        fmt_num(ys, sizeof(ys), y + pv->dy);
        fmt_num(xa, sizeof(xa), x0 + pv->dx);
        fmt_num(xb, sizeof(xb), x1 + pv->dx);
        sheet_text_line(pg, "G0 X%s Y%s", xa, ys);
        sheet_text_line(pg, "G4 P2");
        sheet_text_line(pg, "S%d", (int)(sheet_dose_density[i] * 10.0));
        sheet_text_line(pg, "G1 X%s F%d", xb, SHEET_LADDER_FEED);
    }
    sheet_text_line(pg, "M5");
}

/* The corner-heavy pattern: a long segment that reaches the programmed
 * feed, teeth, a square, small teeth, and a square-root sign as the
 * finish (a short stroke down, a long stroke up, the bar), so the right
 * end holds one acute and one obtuse corner without a move over its
 * own track. 24.5 mm long, 2 mm below the line and 2 above. */
static const float corner_pattern[][2] = {
    { 10, 0 },
    { 1, 0.6f }, { 1, -0.6f }, { 1, 0.6f }, { 1, -0.6f }, { 1, 0.6f }, { 1, -0.6f },
    { 2, 0 }, { 0, 2 }, { -2, 0 }, { 0, -2 },
    { 0.5f, 0.4f }, { 0.5f, -0.4f }, { 0.5f, 0.4f }, { 0.5f, -0.4f },
    { 1, 1.5f }, { 1.5f, -3.5f }, { 4, 0 },
};

void sheet_build_corner(sheet_paths_t *pv, sheet_text_t *pg, const sheet_card_t *c,
                        const double *gamma, int n, int s_pattern, int pattern_feed,
                        int text_s, int feed)
{
    int from = pv->n;
    card_start(pv, pg, c, text_s);
    for (int i = 0; i < n && i < SHEET_CORNER_N; i++)
        num_label(pv, c, c->y + 13.0f + i * 7.0f, "%.2f", gamma[i]);
    burn_since(pv, pg, from, feed);
    sheet_text_line(pg, "M5");
    for (int i = 0; i < n && i < SHEET_CORNER_N; i++) {
        float x = c->x + 14, y = c->y + 12.0f + i * 7.0f;
        sheet_text_line(pg, ";gamma=%.2f", gamma[i]);
        char xs[24], ys[24];
        fmt_num(xs, sizeof(xs), x + pv->dx);
        fmt_num(ys, sizeof(ys), y + pv->dy);
        sheet_text_line(pg, "G0 X%s Y%s", xs, ys);
        sheet_text_line(pg, "M4 S%d", s_pattern);
        sheet_move(pv, x, y);
        for (size_t k = 0; k < sizeof(corner_pattern) / sizeof(*corner_pattern); k++) {
            x += corner_pattern[k][0];
            y += corner_pattern[k][1];
            sheet_to(pv, x, y);
            fmt_num(xs, sizeof(xs), x + pv->dx);
            fmt_num(ys, sizeof(ys), y + pv->dy);
            sheet_text_line(pg, "G1 X%s Y%s F%d", xs, ys, pattern_feed);
        }
        sheet_text_line(pg, "M5");
    }
}

#define PATCH_W 60.0f
#define PATCH_H 15.0f
#define PATCH_PITCH 0.5f

void sheet_build_flowload(sheet_paths_t *pv, sheet_text_t *pg, const sheet_card_t *c,
                          int s_patch, int patch_feed, int text_s, int feed)
{
    /* The patch sits centered in the room under the title. */
    float x0 = c->x + 2, y0 = c->y + 8 + (c->h - 8 - PATCH_H) / 2;
    int from = pv->n;
    card_start(pv, pg, c, text_s);
    burn_since(pv, pg, from, feed);
    /* The patch follows the text with the tube never off for long, so
     * the armed window stays open: the loop settled before the press. */
    char xs[24], ys[24];
    fmt_num(xs, sizeof(xs), x0 + pv->dx);
    fmt_num(ys, sizeof(ys), y0 + pv->dy);
    sheet_text_line(pg, "G0 X%s Y%s", xs, ys);
    sheet_text_line(pg, "M3 S%d", s_patch);
    int rows = (int)(PATCH_H / PATCH_PITCH + 0.5f);
    sheet_move(pv, x0, y0);
    for (int r = 0; r <= rows; r++) {
        float y = y0 + r * PATCH_PITCH;
        float xe = (r & 1) ? x0 : x0 + PATCH_W;
        if (r) {
            sheet_to(pv, (r & 1) ? x0 + PATCH_W : x0, y);
            fmt_num(ys, sizeof(ys), y + pv->dy);
            sheet_text_line(pg, "G1 Y%s F%d", ys, patch_feed);
        }
        sheet_to(pv, xe, y);
        fmt_num(xs, sizeof(xs), xe + pv->dx);
        sheet_text_line(pg, "G1 X%s F%d", xs, patch_feed);
    }
    sheet_text_line(pg, "M5");
}
