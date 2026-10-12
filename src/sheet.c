/*
 * sheet.c - the setup burns: paths, text, layout, and programs
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See sheet.h. Everything here is arithmetic over a point list and a
 * text buffer; the host test draws both cards and checks that each
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

/* Each card stands alone at the corner: its box SHEET_MARGIN from the
 * head's home in X and Y, the size of its content (twelve lines at 3 mm
 * under the title, with the label column) plus 2 mm of margin. */
static const sheet_card_t cards[] = {
    { "laser.focus", "Focus",       SHEET_MARGIN, SHEET_MARGIN, 58, 48 },
    { "laser.floor", "Laser floor", SHEET_MARGIN, SHEET_MARGIN, 66, 48 },
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

void sheet_scrap_size(const sheet_card_t *c, float *w, float *h)
{
    *w = c->w + 2 * SHEET_MARGIN;
    *h = c->h + 2 * SHEET_MARGIN;
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
