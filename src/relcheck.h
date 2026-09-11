/*
 * relcheck.h - forgectrl: the published-release check
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef RELCHECK_H
#define RELCHECK_H

#include <stddef.h>

/* The fixed-name asset of the latest published release. The check reads
 * the version from where this URL redirects; the download follows it to
 * the file. */
#define RELCHECK_LATEST_URL \
    "https://github.com/openglow-org/forgefirm/releases/latest/download/forgefirm.fw"

/* Runs a shell command, stores its output (NUL-terminated, cut to
 * outlen, quotes and newlines removed) and returns the exit status,
 * negative when the command could not run at all. The daemon supplies
 * its own; a test supplies a model of curl. */
typedef int (*relcheck_run_fn)(char *out, size_t outlen, const char *cmd);

/* Resolve the latest published release and write the /update/check
 * reply into body: {"available":true,"version":..,"current":..,"new":..}
 * when a release with a firmware file is published, and
 * {"available":false,"current":..,"detail":".. (HTTP n)"} otherwise.
 * current is the installed version ("" when unknown), already safe to
 * quote. Returns 0 with the body written, -1 when the release server
 * could not be reached (body untouched). */
int relcheck(char *body, size_t len, const char *current,
             relcheck_run_fn run);

#endif
