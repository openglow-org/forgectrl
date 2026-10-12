/*
 * sheet.h - the setup burns: paths, text, layout, and programs
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Pure geometry, no hardware: a path list in millimeters on a scrap
 * (x to the right, y toward the front of the machine, the origin at the
 * head's home, against the gantry stops), text drawn as strokes from
 * the Hershey table, the layout of the two cards, and the same list
 * rendered two ways: SVG for the page's preview and G-code moves for
 * the streamer. The card builders make the preview and the program of
 * each burn together, so what the page shows is what the machine burns.
 *
 * A program is body lines only: the wizard puts the modal frame and the
 * origin (G21, G90, G92) ahead of it and streams it through jobstream.
 * Text and boxes burn under M4, power following speed: a glyph is many
 * short segments the head never reaches its feed in, and at constant
 * power its crawl through every curve over-burns. The ladders burn at
 * constant power (M3).
 * Lines that begin with ';' are directives for the wizard's generator
 * (";keys=floor-off" turns the floor and the dose curve off and sends
 * M102); the streamer never sees them.
 */
#ifndef FORGECTRL_SHEET_H
#define FORGECTRL_SHEET_H

#include <stddef.h>

/* A card's box sits this far from the head's home in X and Y, and the
 * scrap it needs reaches this far past the box on every side. */
#define SHEET_MARGIN    10.0f

#define SHEET_CAP_CARD   4.0f
#define SHEET_CAP_BODY   2.5f

/* The mark dose the boxes and the labels burn at. */
#define SHEET_MARK_S     400
#define SHEET_MARK_FEED  3000
#define SHEET_LADDER_FEED 600
/* The focus ladder burns heavy, so every line marks whatever its
 * defocus and the focused one reads as the narrowest dark line; at the
 * mark dose a defocused beam barely marks and its trace looks thin. */
#define SHEET_FOCUS_S    800
#define SHEET_FOCUS_FEED 1500

/* ------------------------------------------------------------ paths */

typedef struct {
    float x, y;
} sheet_pt_t;

/* A point with x = SHEET_PEN_UP ends one polyline; the next point
 * starts another. */
#define SHEET_PEN_UP (-1e9f)

typedef struct {
    sheet_pt_t *p;
    int n, cap;
    int oom;            /* an allocation failed: the list is incomplete */
    float dx, dy;       /* the offset added to every point appended */
} sheet_paths_t;

void sheet_paths_init(sheet_paths_t *s);
void sheet_paths_free(sheet_paths_t *s);
void sheet_offset(sheet_paths_t *s, float dx, float dy);
void sheet_move(sheet_paths_t *s, float x, float y);    /* pen up, then to */
void sheet_to(sheet_paths_t *s, float x, float y);      /* line to */
void sheet_line(sheet_paths_t *s, float x0, float y0, float x1, float y1);
void sheet_rect(sheet_paths_t *s, float x, float y, float w, float h);
/* Text with its baseline at y and its cap height `cap`; glyphs past
 * max_w (0 = no limit) are left out. Returns the width drawn. */
float sheet_text(sheet_paths_t *s, float x, float y, float cap, const char *text, float max_w);
float sheet_text_width(float cap, const char *text);
/* The extent of every point; 0 when the list is empty. */
int sheet_bounds(const sheet_paths_t *s, float *x0, float *y0, float *x1, float *y1);

/* ------------------------------------------------------------ layout */

typedef struct {
    const char *id;         /* the wizard id that burns the card's pattern */
    const char *title;
    float x, y, w, h;       /* the box, from the head's home */
} sheet_card_t;

size_t sheet_ncards(void);
const sheet_card_t *sheet_card_at(size_t i);
const sheet_card_t *sheet_card(const char *id);

/* The scrap a card needs in the back-left corner: its box and the
 * margin on every side. */
void sheet_scrap_size(const sheet_card_t *c, float *w, float *h);

/* The box and the title of a card. */
void sheet_card_box(sheet_paths_t *s, const sheet_card_t *c);
/* A ladder line's length on a card: from the label column to the box's
 * right margin (the focus and floor ladders span their cards). */
float sheet_ladder_len(const sheet_card_t *c);

/* ------------------------------------------------------------ text out */

typedef struct {
    char *buf;
    size_t len, cap;
    int lines;
    int oom;
} sheet_text_t;

void sheet_text_init(sheet_text_t *t);
void sheet_text_free(sheet_text_t *t);
int sheet_text_line(sheet_text_t *t, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* Append `n` bytes as they are (a whole program body, many lines). */
int sheet_text_append(sheet_text_t *t, const char *s, size_t n);
/* Append the moves for points [from, n): a rapid to each polyline's
 * first point and feed moves along it. */
void sheet_prog_paths(sheet_text_t *t, const sheet_paths_t *s, int from, int feed);
/* The preview: an SVG document of the paths on a w x h sheet. */
int sheet_svg(const sheet_paths_t *s, float w, float h, sheet_text_t *out);

/* ------------------------------------------------------------ cards */

/* Each builder draws the card's preview into `pv` and appends the
 * card's program body to `pg`. `text_s` is the S the text and boxes
 * use under the job's laser keys (the mark S, or its density under an
 * override), `feed` the mark feed. */

/* The focus ladder: n lines, one per Z, each sheet_ladder_len() long at the focus dose
 * (SHEET_FOCUS_S, SHEET_FOCUS_FEED), spread over the lens's measured
 * travel. The box and the labels are burned at z_text at the mark dose. */
#define SHEET_FOCUS_N 12
void sheet_build_focus(sheet_paths_t *pv, sheet_text_t *pg, const sheet_card_t *c,
                       const double *z, int n, double z_text, int text_s, int feed);
/* The floor ladder: n rungs at the given densities (percent), each
 * sheet_ladder_len() long at the ladder feed, under a floor of 0 and the curve off. */
#define SHEET_FLOOR_N 12
void sheet_build_floor(sheet_paths_t *pv, sheet_text_t *pg, const sheet_card_t *c,
                       const double *density, int n, int text_s, int feed);
#endif
