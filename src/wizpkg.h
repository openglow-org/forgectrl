/*
 * wizpkg.h - a package's own check on the Setup page
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A package that holds `wizard` adds a check of its own to the Setup page,
 * as pkg:<id>. Its service runs the check and keeps its result; this runs
 * it on the wizard runner (wizdark.c), with the prompt, the log, and the
 * result a built-in check has, by asking the service one step at a time
 * through the extension host (forgeext wizard): start, then an answer for
 * each prompt, and abort when the operator aborts. A step is
 *
 *   {"log": ["..."], "phase": "...", "progress": 0-100,
 *    "prompt": {"kind": "continue|confirm|number|choice", "id": "...", "text": "...",
 *               "options": ["..."]}}
 *
 * or, the last one, {"result": {"ok": true|false, "summary": "..."}}. A
 * confirm is answered "yes" or "no". The service's GET /wizard says
 * {"title": "...", "done": true|false} for the Setup page's list.
 *
 * The check holds no machine lease and writes nothing of the machine's:
 * whatever it does to the machine, it does through the package's own
 * capabilities, and its result is the package's own. It can gate only the
 * package's own work, never the machine's setup gate.
 */
#ifndef FORGECTRL_WIZPKG_H
#define FORGECTRL_WIZPKG_H

#include <jansson.h>
#include <stddef.h>

#define WIZPKG_PREFIX     "pkg:"
#define WIZPKG_STEPS_MAX  64        /* a check that takes more has lost its way */
#define WIZPKG_CACHE_S    10        /* the Setup page's list, asked again at most this often */
#define WIZPKG_LOG_MAX    8
#define WIZPKG_OPTS_MAX   8

typedef struct {
    char log[WIZPKG_LOG_MAX][160];
    int nlog;
    char phase[96];
    int progress;                   /* -1 when the step names none */
    int has_prompt;
    char kind[16], pid[32], text[400];
    char opts[WIZPKG_OPTS_MAX][64];
    int nopt;
    int has_result, ok;
    char summary[200];
} wizpkg_step_t;

/* One step of the service's, held to its form: 0, or -1 with the words. */
int wizpkg_parse(json_t *body, wizpkg_step_t *s, char *why, size_t wlen);

/* The wizard runner's entry for pkg:<id>. */
void wizpkg_run(void);

/* 1 when id is pkg:<id> of an installed package that adds a check. */
int wizpkg_known(const char *id);

/* The Setup page's list: [{id, title, done}], from the host and each
 * package's own answer, asked at most every WIZPKG_CACHE_S. A new
 * reference. */
json_t *wizpkg_catalog_json(void);

#endif
