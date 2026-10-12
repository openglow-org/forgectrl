/*
 * wizlive.h - the setup burns (see wizlive.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_WIZLIVE_H
#define FORGECTRL_WIZLIVE_H

#include "sheet.h"

/* Is `id` a burn this build knows? */
int wizlive_known(const char *id);

/* The preview of a burn as an SVG document: its card on the scrap it
 * needs in the back-left corner. */
int wizlive_preview(const char *id, sheet_text_t *out);

/* The program body the burn streams, with a comment on the frame the
 * wizard puts ahead of it; for inspection. */
int wizlive_program(const char *id, sheet_text_t *out);

#endif
