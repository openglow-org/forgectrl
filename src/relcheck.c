/*
 * relcheck.c - forgectrl: the published-release check
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Resolves the latest published ForgeFIRM release without the GitHub
 * API (no token, no rate limit, no JSON). The fixed-name asset URL
 * redirects to .../releases/download/<tag>/forgefirm.fw, and that URL
 * redirects on to the asset store, whose URL names an object id and
 * nothing else. The tag is visible on the FIRST hop only, so the check
 * sends two HEAD requests:
 *
 *   1. The fixed-name URL, redirects not followed. The Location the
 *      server answers with carries the tag. A 404 here means no
 *      published release at all.
 *   2. The same URL, the whole chain followed. A 200 at the end means
 *      the file is there. A release with no forgefirm.fw asset still
 *      gets the first redirect; the tagged hop then answers 404.
 *
 * Nothing read from the network goes back to the shell: both commands
 * are fixed strings, and the tag is copied out under a character
 * whitelist before it is quoted into the reply.
 */
#include "relcheck.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The first hop: its status and its redirect target, redirects not
 * followed. */
#define FIRST_CMD \
    "curl -sI -o /dev/null -w '%{http_code} %{redirect_url}' " \
    "--max-time 20 " RELCHECK_LATEST_URL " 2>/dev/null"
/* The whole chain: the final status only. */
#define CHAIN_CMD \
    "curl -sIL -o /dev/null -w '%{http_code}' " \
    "--max-time 20 " RELCHECK_LATEST_URL " 2>/dev/null"

#define DOWNLOAD_SEG "/releases/download/"

/* The release tag the first hop's report names: the path segment after
 * /releases/download/ in its redirect target. Empty when the report
 * names none, or when the segment holds anything but tag characters. */
static void report_tag(const char *report, char *tag, size_t len)
{
    tag[0] = '\0';
    const char *m = strstr(report, DOWNLOAD_SEG);
    if (!m)
        return;
    m += strlen(DOWNLOAD_SEG);
    size_t o = 0;
    for (; *m && *m != '/'; m++) {
        unsigned char c = (unsigned char)*m;
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_' || c == '+')
            || o + 1 >= len) {
            tag[0] = '\0';
            return;
        }
        tag[o++] = (char)c;
    }
    tag[o] = '\0';
}

int relcheck(char *body, size_t len, const char *current,
             relcheck_run_fn run)
{
    char first[512];
    if (run(first, sizeof(first), FIRST_CMD) != 0)
        return -1;
    int http = atoi(first);
    char tag[48];
    report_tag(first, tag, sizeof(tag));

    char why[96];
    const char *detail = "release server error";
    if (tag[0]) {
        char chain[16];
        if (run(chain, sizeof(chain), CHAIN_CMD) != 0)
            return -1;
        http = atoi(chain);
        if (http == 200) {
            snprintf(body, len,
                     "{\"available\":true,\"version\":\"%s\","
                     "\"current\":\"%s\",\"new\":%s}",
                     tag, current, strcmp(tag, current) ? "true" : "false");
            return 0;
        }
        if (http == 404) {
            snprintf(why, sizeof(why),
                     "release %s carries no firmware file", tag);
            detail = why;
        }
    } else if (http == 404 || http == 0) {
        /* The expected state before the first release, distinct from
         * a transport or proxy error. */
        detail = "no published release found";
    }
    snprintf(body, len,
             "{\"available\":false,\"current\":\"%s\","
             "\"detail\":\"%s (HTTP %d)\"}", current, detail, http);
    return 0;
}
