/*
 * advisories.h - the advisory documents embedded in the daemon
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The four documents the first-run wizard shows (docs/advisories, one file per document)
 * are compiled into the binary by src/ui/embed_docs.cmake, so the
 * machine carries exactly the texts this build was made with. Each
 * document's SHA-256 is the version the record stores: a changed text
 * has a new hash and must be accepted again.
 */
#ifndef FORGECTRL_ADVISORIES_H
#define FORGECTRL_ADVISORIES_H
#include <stddef.h>

typedef struct {
    const char *id;         /* "safety-and-risk", the file name stem */
    const char *title;
    const char *consent;    /* "typed" or "check" */
    const char *phrase;     /* the typed phrase, or NULL */
    const unsigned char *text;
    size_t len;
    char hash[65];          /* filled by advisories_init() */
} advisory_t;

/* Compute the hashes. Call once at startup. */
void advisories_init(void);
const advisory_t *advisories_list(size_t *n);
const advisory_t *advisories_find(const char *id);
#endif
