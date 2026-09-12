/*
 * relcheck.h - forgectrl: the published-release check
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef RELCHECK_H
#define RELCHECK_H

#include <stddef.h>

/* The latest published release, from the GitHub REST API: the newest
 * release that is neither a draft nor a prerelease. An unauthenticated
 * read, allowed 60 times an hour per address. It touches no download
 * counter. */
#define RELCHECK_API_URL \
    "https://api.github.com/repos/openglow-org/forgefirm/releases/latest"
/* The firmware file every release carries under this fixed name. */
#define RELCHECK_ASSET "forgefirm.fw"
/* The firmware file of a release, by its tag. This is the URL GitHub
 * counts as a download, so only the download itself requests it. */
#define RELCHECK_DOWNLOAD_FMT \
    "https://github.com/openglow-org/forgefirm/releases/download/%s/" \
    RELCHECK_ASSET

/* Ceilings: the API reply a fetch may hand over, and the release notes
 * kept from it (cut at a character boundary past the ceiling). */
#define RELCHECK_BODY_MAX  (256 * 1024)
#define RELCHECK_NOTES_MAX (24 * 1024)

/* Fetch a URL: writes the response body (NUL-terminated, cut to len)
 * and the HTTP status. Returns 0 when the server answered, negative
 * when nothing answered (no network, no name, a timeout). The daemon
 * supplies curl; a test supplies a scripted server. */
typedef int (*relcheck_fetch_fn)(const char *url, char *body, size_t len,
                                 int *http);

struct relcheck_result {
    int  available;             /* a release with the firmware file */
    char version[48];           /* its tag, tag characters only */
    char published[32];         /* its published_at, as the API gives it */
    long bytes;                 /* the firmware file's size */
    char notes[RELCHECK_NOTES_MAX]; /* its release notes, markdown */
    char detail[160];           /* why not available, otherwise "" */
    int  http;                  /* the status the API answered with */
};

/* Ask the API for the latest release. Returns 0 with r filled (available
 * or not, detail says why not), -1 when the server could not be
 * reached (r cleared). Nothing read from the network is trusted: the
 * tag is copied out under a character whitelist, everything else is
 * carried as data for the JSON reply. */
int relcheck(struct relcheck_result *r, relcheck_fetch_fn fetch);

/* True when the tag names a release newer than the installed version.
 * A version is v<major>.<minor>.<patch>, an optional -suffix marking a
 * prerelease of that number. An installed version that is not one (a
 * development build's stamp, an empty string) is older than every
 * release. */
int relcheck_is_newer(const char *tag, const char *current);

/* True when s is made of tag characters only (letters, digits, . - _ +)
 * and is not empty: safe to quote into a reply and into a command. */
int relcheck_tag_ok(const char *s);

#endif
