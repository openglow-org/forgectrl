/*
 * advisories.h - the advisory documents embedded in the daemon
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The four documents the first-run wizard shows (docs/advisories, one file per document)
 * are compiled into the binary by src/ui/embed_docs.cmake, so the
 * machine carries exactly the texts this build was made with. Each
 * document's SHA-256 is the version the record stores: a changed text
 * has a new hash and must be accepted again.
 *
 * A fifth document is on demand: the Extensions advisory is no part of
 * first-run setup, so an owner who never turns extensions on is never
 * asked about it. It is served the same way, and it is accepted where
 * its feature is turned on (POST /settings ext_enabled=1, with its
 * current hash and the typed phrase).
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
    int on_demand;          /* not a first-run document: accepted where its feature is turned on */
} advisory_t;

/* Compute the hashes. Call once at startup. */
void advisories_init(void);
/* The first-run documents, in the order the wizard shows them. */
const advisory_t *advisories_list(size_t *n);
/* Any document, the on-demand ones included. */
const advisory_t *advisories_find(const char *id);
#endif
