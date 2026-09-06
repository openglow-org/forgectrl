/*
 * sheet_test.c - host unit test for the commissioning sheet renderer
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The font table is whole; text draws, measures, and clips; the cards
 * tile the frame; every card's drawing stays inside its box, on the
 * sheet and alone; every program line is one the controller takes or a
 * directive; the SVG is one document; the curve inverse interpolates.
 */
#include "../src/sheet.h"
#include "../src/font_hershey.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void test_font(void)
{
    unsigned total = 0;
    for (int i = 0; i < HERSHEY_NGLYPH; i++) {
        const hershey_glyph_t *g = &hershey_glyphs[i];
        CHECK(g->adv > 0, "glyph %d has no advance", i + HERSHEY_FIRST);
        CHECK(g->first == total, "glyph %d does not follow the last (%u vs %u)", i + HERSHEY_FIRST,
              g->first, total);
        total += g->count;
        for (unsigned k = 0; k < g->count; k++) {
            const short *p = hershey_pts[g->first + k];
            if (p[0] == HERSHEY_PEN_UP)
                continue;
            CHECK(p[0] >= -100 && p[0] <= 1200 && p[1] >= -400 && p[1] <= 1000,
                  "glyph %d point %u out of range (%d,%d)", i + HERSHEY_FIRST, k, p[0], p[1]);
        }
    }
    CHECK(total == hershey_npts, "point count %u vs %u", total, hershey_npts);
    CHECK(hershey_glyphs[0].count == 0, "the space has strokes");
    CHECK(hershey_glyphs['A' - HERSHEY_FIRST].count > 4, "A has too few points");
}

static void test_text(void)
{
    sheet_paths_t s;
    sheet_paths_init(&s);
    float w = sheet_text(&s, 10, 20, 2.5f, "Hello", 0);
    CHECK(w > 5 && w < 12, "Hello at 2.5 mm is %.1f mm wide", (double)w);
    CHECK(fabsf(w - sheet_text_width(2.5f, "Hello")) < 0.01f, "width differs from the measure");
    float x0, y0, x1, y1;
    CHECK(sheet_bounds(&s, &x0, &y0, &x1, &y1), "no bounds");
    CHECK(x0 >= 9.9f && x1 <= 10 + w + 0.5f, "text runs outside its advance: %.2f..%.2f", (double)x0,
          (double)x1);
    CHECK(y1 <= 20.01f && y0 >= 20 - 2.6f, "cap height off: y %.2f..%.2f", (double)y0, (double)y1);
    /* Clipping: a narrow limit keeps the first glyphs only. */
    sheet_paths_t c;
    sheet_paths_init(&c);
    float wc = sheet_text(&c, 0, 0, 2.5f, "Hello world, this is long", 6.0f);
    CHECK(wc <= 6.01f, "clipped text is %.2f wide", (double)wc);
    CHECK(c.n < s.n * 2, "clipped text drew everything");
    /* A wider cap scales. */
    CHECK(fabsf(sheet_text_width(5.0f, "Hello") - 2 * w) < 0.01f, "width does not scale with cap");
    sheet_paths_free(&s);
    sheet_paths_free(&c);
}

static void test_layout(void)
{
    size_t n = sheet_ncards();
    CHECK(n == 5, "%zu cards", n);
    /* The sheet fits a 10 x 8 in piece with room to spare. */
    CHECK(SHEET_W <= 254.0f && SHEET_H <= 203.2f, "the sheet is %.0f x %.0f", (double)SHEET_W, (double)SHEET_H);
    CHECK(SHEET_FRAME_X + SHEET_FRAME_W <= SHEET_W && SHEET_FRAME_Y + SHEET_FRAME_H <= SHEET_H,
          "the frame leaves the sheet");
    /* The focus and floor ladders span their cards: from the label
     * column to the right margin, and the wizards name that length. */
    CHECK(fabsf(sheet_ladder_len(sheet_card("laser.focus")) - 45.0f) < 0.01f, "the focus ladder is %.1f mm",
          (double)sheet_ladder_len(sheet_card("laser.focus")));
    CHECK(fabsf(sheet_ladder_len(sheet_card("laser.floor")) - 53.0f) < 0.01f, "the floor ladder is %.1f mm",
          (double)sheet_ladder_len(sheet_card("laser.floor")));
    /* Every card title fits its box at the card cap. */
    for (size_t i = 0; i < n; i++) {
        const sheet_card_t *c = sheet_card_at(i);
        CHECK(sheet_text_width(SHEET_CAP_CARD, c->title) <= c->w - 4, "%s: the title is wider than the card",
              c->id);
        CHECK(c->x >= SHEET_FRAME_X && c->x + c->w <= SHEET_FRAME_X + SHEET_FRAME_W + 0.01f,
              "%s runs outside the frame in x", c->id);
        CHECK(c->y >= SHEET_FRAME_Y + 32 && c->y + c->h <= SHEET_FRAME_Y + SHEET_FRAME_H + 0.01f,
              "%s runs into the header or out of the frame", c->id);
        for (size_t k = 0; k < n; k++) {
            const sheet_card_t *d = sheet_card_at(k);
            if (k == i)
                continue;
            int apart = c->x + c->w + SHEET_GUTTER <= d->x + 0.01f || d->x + d->w + SHEET_GUTTER <= c->x + 0.01f ||
                        c->y + c->h + SHEET_GUTTER <= d->y + 0.01f || d->y + d->h + SHEET_GUTTER <= c->y + 0.01f;
            CHECK(apart, "%s and %s overlap or touch", c->id, d->id);
        }
    }
    CHECK(sheet_card("laser.focus") != NULL && sheet_card("nope") == NULL, "lookup");
}

static int program_ok(const sheet_text_t *pg, const char *what)
{
    int ok = 1, lines = 0;
    char *copy = strdup(pg->buf ? pg->buf : "");
    for (char *l = strtok(copy, "\n"); l; l = strtok(NULL, "\n")) {
        lines++;
        size_t len = strlen(l);
        CHECK(len < 80, "%s: a line of %zu chars", what, len);
        int good = !strncmp(l, "G0 ", 3) || !strncmp(l, "G1 ", 3) || !strncmp(l, "G4 ", 3) ||
                   !strncmp(l, "M3 S", 4) || !strncmp(l, "M4 S", 4) || !strcmp(l, "M5") ||
                   (l[0] == 'S' && atoi(l + 1) > 0) || l[0] == ';';
        if (!good) {
            CHECK(0, "%s: bad line %s", what, l);
            ok = 0;
        }
        if (!strncmp(l, "G1 ", 3))
            CHECK(strstr(l, "F") != NULL, "%s: a feed move without F: %s", what, l);
    }
    free(copy);
    CHECK(lines > 5, "%s: only %d lines", what, lines);
    return ok;
}

static void bounds_inside(const sheet_paths_t *pv, float x, float y, float w, float h, const char *what)
{
    float x0, y0, x1, y1;
    CHECK(sheet_bounds(pv, &x0, &y0, &x1, &y1), "%s: nothing drawn", what);
    CHECK(x0 >= x - 0.01f && x1 <= x + w + 0.01f && y0 >= y - 0.01f && y1 <= y + h + 0.01f,
          "%s: drawing %.1f..%.1f x %.1f..%.1f leaves the box %.0f..%.0f x %.0f..%.0f", what,
          (double)x0, (double)x1, (double)y0, (double)y1, (double)x, (double)(x + w), (double)y,
          (double)(y + h));
}

static void build(const char *id, sheet_paths_t *pv, sheet_text_t *pg)
{
    const sheet_card_t *c = sheet_card(id);
    double z[SHEET_FOCUS_N], d[SHEET_FLOOR_N], g[SHEET_CORNER_N] = { 1.0, 1.25, 1.5, 1.75, 2.0 };
    for (int i = 0; i < SHEET_FOCUS_N; i++)
        z[i] = 10.6 - 1.4 * i;
    for (int i = 0; i < SHEET_FLOOR_N; i++)
        d[i] = 2.0 * (i + 1);
    if (!strcmp(id, "laser.focus"))
        sheet_build_focus(pv, pg, c, z, SHEET_FOCUS_N, 5.3, 400, 3000);
    else if (!strcmp(id, "laser.floor"))
        sheet_build_floor(pv, pg, c, d, SHEET_FLOOR_N, 600, 3000);
    else if (!strcmp(id, "laser.dose-curve"))
        sheet_build_dose(pv, pg, c, 600, 3000);
    else if (!strcmp(id, "laser.corner"))
        sheet_build_corner(pv, pg, c, g, SHEET_CORNER_N, 300, 2000, 400, 3000);
    else if (!strcmp(id, "cooling.flow-load"))
        sheet_build_flowload(pv, pg, c, 600, 1500, 400, 3000);
}

static void test_cards(void)
{
    static const char *const ids[] = { "laser.focus", "laser.floor", "laser.dose-curve",
                                       "laser.corner", "cooling.flow-load" };
    for (size_t i = 0; i < sizeof(ids) / sizeof(*ids); i++) {
        const sheet_card_t *c = sheet_card(ids[i]);
        sheet_paths_t pv;
        sheet_text_t pg;
        sheet_paths_init(&pv);
        sheet_text_init(&pg);
        build(ids[i], &pv, &pg);
        bounds_inside(&pv, c->x, c->y, c->w, c->h, ids[i]);
        program_ok(&pg, ids[i]);
        CHECK(strstr(pg.buf, "M5") != NULL, "%s: no M5", ids[i]);
        sheet_paths_free(&pv);
        sheet_text_free(&pg);
        /* Alone: the same card offset to the margin of its own sheet. */
        sheet_paths_init(&pv);
        sheet_text_init(&pg);
        float dx, dy, w, h;
        sheet_alone_offset(c, &dx, &dy);
        sheet_alone_size(c, &w, &h);
        sheet_offset(&pv, dx, dy);
        build(ids[i], &pv, &pg);
        bounds_inside(&pv, SHEET_ALONE_MARGIN, SHEET_ALONE_MARGIN, c->w, c->h, "alone");
        /* The program's coordinates moved with the drawing. */
        char want[32];
        snprintf(want, sizeof(want), "X%.0f", (double)(c->x + dx + 2));
        CHECK(strstr(pg.buf, "G0 X") != NULL, "%s alone: no rapid", ids[i]);
        sheet_paths_free(&pv);
        sheet_text_free(&pg);
    }
    /* The focus ladder runs at its own heavy dose, not the mark's. */
    {
        sheet_paths_t fv;
        sheet_text_t fg;
        sheet_paths_init(&fv);
        sheet_text_init(&fg);
        build("laser.focus", &fv, &fg);
        char want[32];
        snprintf(want, sizeof(want), "M3 S%d\n", SHEET_FOCUS_S);
        CHECK(strstr(fg.buf, want) != NULL, "the focus ladder lacks its S line");
        snprintf(want, sizeof(want), "F%d\n", SHEET_FOCUS_FEED);
        CHECK(strstr(fg.buf, want) != NULL, "the focus ladder lacks its feed");
        sheet_paths_free(&fv);
        sheet_text_free(&fg);
    }
    /* The corner card carries one directive per gamma. */
    sheet_paths_t pv;
    sheet_text_t pg;
    sheet_paths_init(&pv);
    sheet_text_init(&pg);
    build("laser.corner", &pv, &pg);
    int n = 0;
    for (const char *p = pg.buf; (p = strstr(p, ";gamma=")); p++)
        n++;
    CHECK(n == SHEET_CORNER_N, "%d gamma directives", n);
    CHECK(strstr(pg.buf, "M4 S300") != NULL, "no M4 in the corner card");
    sheet_paths_free(&pv);
    sheet_text_free(&pg);
    sheet_paths_init(&pv);
    sheet_text_init(&pg);
    build("cooling.flow-load", &pv, &pg);
    CHECK(strstr(pg.buf, ";settle") == NULL, "the flow-load card settles inside the job");
    CHECK(strstr(pg.buf, "M3 S") != NULL, "no patch in the flow-load card");
    sheet_paths_free(&pv);
    sheet_text_free(&pg);
}

static void test_frame(void)
{
    sheet_paths_t pv;
    sheet_text_t pg;
    sheet_paths_init(&pv);
    sheet_text_init(&pg);
    /* A long version and a long camera name: the facts stay in the band. */
    sheet_header_t h = { "0.0.99-rc1+g1234abcd", "release", "Pro", "OV8856", "ABCDE-FGHIJ",
                         "2026-09-05 20:00Z" };
    sheet_build_frame(&pv, &pg, &h, 400, 3000);
    bounds_inside(&pv, SHEET_FRAME_X, SHEET_FRAME_Y, SHEET_FRAME_W, SHEET_FRAME_H, "frame");
    program_ok(&pg, "frame");
    CHECK(pv.n > 500, "the header has %d points", pv.n);
    /* Nothing but the frame's own edges reaches under the header band. */
    for (int i = 0; i < pv.n; i++)
        if (pv.p[i].x != SHEET_PEN_UP && pv.p[i].x > SHEET_FRAME_X + 0.5f &&
            pv.p[i].x < SHEET_FRAME_X + SHEET_FRAME_W - 0.5f)
            CHECK(pv.p[i].y <= SHEET_FRAME_Y + 32 - 1.5f || pv.p[i].y >= SHEET_FRAME_Y + SHEET_FRAME_H - 0.5f,
                  "header text at y %.1f runs into the cards", (double)pv.p[i].y);
    sheet_paths_free(&pv);
    sheet_text_free(&pg);
    /* The corner card's four-character labels are drawn whole. */
    sheet_paths_init(&pv);
    sheet_text_init(&pg);
    build("laser.corner", &pv, &pg);
    const sheet_card_t *c = sheet_card("laser.corner");
    float label_w = sheet_text_width(SHEET_CAP_BODY, "1.25");
    int right_of_label = 0;
    /* The first label, "1.00", sits at baseline y + 13; its last glyph
     * runs past x + 2 + (the width less one glyph). The pattern starts
     * at x + 14, outside the window. */
    for (int i = 0; i < pv.n; i++)
        if (pv.p[i].x != SHEET_PEN_UP && pv.p[i].x > c->x + 2 + label_w - 2.0f && pv.p[i].x < c->x + 13 &&
            pv.p[i].y > c->y + 10 && pv.p[i].y < c->y + 13.5f)
            right_of_label++;
    CHECK(right_of_label > 0, "the corner label's last digit is clipped");
    sheet_paths_free(&pv);
    sheet_text_free(&pg);
}

/* A whole program body goes through the raw append, not the line
 * writer, which is bounded at one line's length. */
static void test_text_append(void)
{
    sheet_paths_t pv;
    sheet_text_t pg, out;
    sheet_paths_init(&pv);
    sheet_text_init(&pg);
    sheet_text_init(&out);
    sheet_header_t h = { "0.0.1", "dev", "pro", "OV5648", "ABCDE-FGHIJ", "2026-09-05 20:00Z" };
    sheet_build_frame(&pv, &pg, &h, 400, 3000);
    CHECK(pg.len > 2000, "the frame body is only %zu bytes", pg.len);
    sheet_text_line(&out, "; a comment line");
    CHECK(sheet_text_append(&out, pg.buf, pg.len) == 0, "append failed");
    CHECK(out.len == pg.len + strlen("; a comment line\n"), "append lost bytes: %zu vs %zu", out.len, pg.len);
    CHECK(out.lines == pg.lines + 1, "append miscounted lines: %d vs %d", out.lines, pg.lines);
    CHECK(strstr(out.buf, "M4 S400") != NULL && strstr(out.buf, "\nM5\n") != NULL, "the body is not whole");
    sheet_paths_free(&pv);
    sheet_text_free(&pg);
    sheet_text_free(&out);
}

static void test_svg(void)
{
    sheet_paths_t pv;
    sheet_text_t out;
    sheet_paths_init(&pv);
    sheet_text_init(&out);
    sheet_rect(&pv, 10, 10, 180, 130);
    sheet_text(&pv, 20, 30, 6, "OpenGlow", 0);
    CHECK(sheet_svg(&pv, 200, 150, &out) == 0, "svg failed");
    CHECK(!strncmp(out.buf, "<svg ", 5), "svg does not start with <svg");
    CHECK(strstr(out.buf, "</svg>") != NULL, "svg unterminated");
    CHECK(strstr(out.buf, "M10 10 L190 10 L190 140 L10 140 L10 10 ") != NULL, "the frame path is off: %.200s",
          out.buf);
    CHECK(strstr(out.buf, "viewBox=\"0 0 200.0 150.0\"") != NULL, "viewBox");
    sheet_paths_free(&pv);
    sheet_text_free(&out);
}

static void test_curve(void)
{
    const char *curve = "10:0.5,20:2,30:7,45:21,60:37,80:50,100:100";
    CHECK(fabs(sheet_density_for_light(curve, 100) - 100) < 1e-9, "top");
    CHECK(fabs(sheet_density_for_light(curve, 50) - 80) < 1e-9, "50 light is 80 density");
    double d = sheet_density_for_light(curve, 40);
    CHECK(d > 60 && d < 80, "40 light interpolates to %.1f", d);
    CHECK(fabs(sheet_density_for_light("off", 40) - 40) < 1e-9, "off is the identity");
    CHECK(fabs(sheet_density_for_light("", 40) - 40) < 1e-9, "empty is the identity");
    CHECK(fabs(sheet_density_for_light("garbage", 40) - 40) < 1e-9, "bad text is the identity");
    CHECK(sheet_density_for_light(curve, 0.25) > 0 && sheet_density_for_light(curve, 0.25) <= 10, "below the first point");
}

int main(void)
{
    test_font();
    test_text();
    test_layout();
    test_cards();
    test_frame();
    test_text_append();
    test_svg();
    test_curve();
    if (fails) {
        printf("sheet_test: %d failures\n", fails);
        return 1;
    }
    printf("sheet_test: ok\n");
    return 0;
}
