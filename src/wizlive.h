/*
 * wizlive.h - the sheet wizards (see wizlive.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_WIZLIVE_H
#define FORGECTRL_WIZLIVE_H

#include "sheet.h"

/* Is `id` a sheet wizard this build knows? */
int wizlive_known(const char *id);

/* The preview of a wizard's burn as an SVG document (the frame and
 * header, or a card on the sheet or alone as the placement says; the
 * whole sheet with every card for the placement), from the record's
 * facts. */
int wizlive_preview(const char *id, sheet_text_t *out);

/* The program body the wizard streams, with a comment on the frame the
 * wizard puts ahead of it; for inspection. */
int wizlive_program(const char *id, sheet_text_t *out);

#endif
