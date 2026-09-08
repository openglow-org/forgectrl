/*
 * recordhtml.h - the commissioning record as a printable page (see recordhtml.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_RECORDHTML_H
#define FORGECTRL_RECORDHTML_H
#include <jansson.h>

/* Render the record as one self-contained HTML page: no script, no
 * external resource, print styles inline. `docs` and `wizards` are
 * arrays of {id, title} in display order (the advisory documents and
 * the wizard catalog); a wizard the record does not hold is left out.
 * `version` is the firmware version line. Returns a malloc'd string
 * the caller frees, or NULL. Pure: nothing here reads the machine. */
char *record_html(const json_t *rec, const json_t *docs, const json_t *wizards,
                  const char *version);
#endif
