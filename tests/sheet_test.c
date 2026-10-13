/*
 * sheet_test.c - host unit test for the setup burns' renderer
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Text draws, measures, and clips; each card sits the margin from the
 * head's home and its scrap fits a 4 x 3 in piece; every card's drawing
 * stays inside its box; every program line is one the controller takes
 * or a directive; the SVG is one document.
 */
#include "../src/sheet.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

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
    CHECK(n == 2, "%zu cards", n);
    /* The focus and floor ladders span their cards: from the label
     * column to the right margin, and the wizards name that length. */
    CHECK(fabsf(sheet_ladder_len(sheet_card("laser.focus")) - 45.0f) < 0.01f, "the focus ladder is %.1f mm",
          (double)sheet_ladder_len(sheet_card("laser.focus")));
    CHECK(fabsf(sheet_ladder_len(sheet_card("laser.floor")) - 53.0f) < 0.01f, "the floor ladder is %.1f mm",
          (double)sheet_ladder_len(sheet_card("laser.floor")));
    for (size_t i = 0; i < n; i++) {
        const sheet_card_t *c = sheet_card_at(i);
        /* Every card title fits its box at the card cap. */
        CHECK(sheet_text_width(SHEET_CAP_CARD, c->title) <= c->w - 4, "%s: the title is wider than the card",
              c->id);
        /* Each card's box sits the margin from the head's home, and its
         * scrap is the box with the margin on every side. */
        CHECK(c->x == SHEET_MARGIN && c->y == SHEET_MARGIN, "%s: the box is at %.0f, %.0f", c->id,
              (double)c->x, (double)c->y);
        float w, h;
        sheet_scrap_size(c, &w, &h);
        CHECK(w == c->w + 2 * SHEET_MARGIN && h == c->h + 2 * SHEET_MARGIN, "%s: the scrap is %.0f x %.0f",
              c->id, (double)w, (double)h);
        /* A scrap fits a 4 x 3 in piece. */
        CHECK(w <= 101.6f && h <= 76.2f, "%s: the scrap %.0f x %.0f is larger than 4 x 3 in", c->id,
              (double)w, (double)h);
    }
    CHECK(sheet_card("laser.focus") != NULL && sheet_card("laser.floor") != NULL &&
          sheet_card("laser.dose-curve") == NULL && sheet_card("nope") == NULL, "lookup");
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
    double z[SHEET_FOCUS_N], d[SHEET_FLOOR_N];
    for (int i = 0; i < SHEET_FOCUS_N; i++)
        z[i] = 10.6 - 1.4 * i;
    for (int i = 0; i < SHEET_FLOOR_N; i++)
        d[i] = 2.0 * (i + 1);
    if (!strcmp(id, "laser.focus"))
        sheet_build_focus(pv, pg, c, z, SHEET_FOCUS_N, 5.3, 400, 3000);
    else if (!strcmp(id, "laser.floor"))
        sheet_build_floor(pv, pg, c, d, SHEET_FLOOR_N, 600, 3000);
}

static void test_cards(void)
{
    static const char *const ids[] = { "laser.focus", "laser.floor" };
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
        CHECK(strstr(pg.buf, "G0 X") != NULL, "%s: no rapid", ids[i]);
        /* Nothing the program draws comes nearer home than the margin. */
        float x0, y0, x1, y1;
        sheet_bounds(&pv, &x0, &y0, &x1, &y1);
        CHECK(x0 >= SHEET_MARGIN - 0.01f && y0 >= SHEET_MARGIN - 0.01f, "%s: drawing at %.1f, %.1f", ids[i],
              (double)x0, (double)y0);
        sheet_paths_free(&pv);
        sheet_text_free(&pg);
    }
    /* The focus ladder runs at its own heavy dose, not the mark's. */
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
    /* The floor rungs run with the floor and the dose curve off. */
    sheet_paths_init(&fv);
    sheet_text_init(&fg);
    build("laser.floor", &fv, &fg);
    CHECK(strstr(fg.buf, ";keys=floor-off\n") != NULL, "the floor card lacks its keys directive");
    sheet_paths_free(&fv);
    sheet_text_free(&fg);
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
    CHECK(strstr(out.buf, "<path") != NULL, "svg carries no path");
    sheet_paths_free(&pv);
    sheet_text_free(&out);
}

int main(void)
{
    test_text();
    test_layout();
    test_cards();
    test_svg();
    if (fails) {
        printf("sheet_test: %d failures\n", fails);
        return 1;
    }
    printf("sheet_test: ok\n");
    return 0;
}
