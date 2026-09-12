/*
 * relcheck.c - forgectrl: the published-release check
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Resolves the latest published ForgeFIRM release from the GitHub REST
 * API, one unauthenticated GET. The reply names the release's tag, its
 * notes, and its assets with their sizes, so the check learns whether
 * the firmware file is there without touching the file's URL: GitHub
 * counts every request of that URL as a download, and the counter is
 * meant to count installs.
 *
 * The tag is the only value that leaves this module as anything but
 * data: it names the file the download requests. It is copied out
 * under a character whitelist. The notes are carried whole, cut at a
 * character boundary past their ceiling, and go into the reply through
 * a JSON encoder, never into a command.
 */
#include "relcheck.h"

#include <ctype.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int relcheck_tag_ok(const char *s)
{
    if (!s || !*s)
        return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_' || c == '+'))
            return 0;
    }
    return 1;
}

/* Copy src into dst, cut to len, never splitting a UTF-8 sequence: a
 * cut that lands inside one backs up to the sequence's start. */
static void copy_text(char *dst, size_t len, const char *src)
{
    size_t n = strlen(src);
    if (n >= len) {
        n = len - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *str_or(json_t *v, const char *dflt)
{
    return json_is_string(v) ? json_string_value(v) : dflt;
}

int relcheck(struct relcheck_result *r, relcheck_fetch_fn fetch)
{
    memset(r, 0, sizeof(*r));
    char *body = malloc(RELCHECK_BODY_MAX);
    if (!body)
        return -1;
    int http = 0;
    if (fetch(RELCHECK_API_URL, body, RELCHECK_BODY_MAX, &http) != 0) {
        free(body);
        return -1;
    }
    r->http = http;

    if (http == 404) {
        /* The expected state before the first release, distinct from a
         * transport or proxy error. */
        snprintf(r->detail, sizeof(r->detail),
                 "no published release found (HTTP %d)", http);
        free(body);
        return 0;
    }

    json_error_t err;
    json_t *root = json_loads(body, 0, &err);
    free(body);
    if (http != 200) {
        /* The API says why in "message" (the rate limit names itself
         * there); it is data, quoted into the reply by the encoder. */
        const char *msg = root ? str_or(json_object_get(root, "message"), "")
                               : "";
        char why[96];
        copy_text(why, sizeof(why), msg);
        snprintf(r->detail, sizeof(r->detail),
                 "release server error (HTTP %d)%s%s", http,
                 why[0] ? ": " : "", why);
        if (root)
            json_decref(root);
        return 0;
    }
    if (!root || !json_is_object(root)) {
        snprintf(r->detail, sizeof(r->detail),
                 "release server answered badly (HTTP %d)", http);
        if (root)
            json_decref(root);
        return 0;
    }

    const char *tag = str_or(json_object_get(root, "tag_name"), "");
    if (!relcheck_tag_ok(tag) || strlen(tag) >= sizeof(r->version)) {
        snprintf(r->detail, sizeof(r->detail),
                 "release server error (HTTP %d): the release tag is not "
                 "a tag", http);
        json_decref(root);
        return 0;
    }
    snprintf(r->version, sizeof(r->version), "%s", tag);
    copy_text(r->published, sizeof(r->published),
              str_or(json_object_get(root, "published_at"), ""));
    copy_text(r->notes, sizeof(r->notes),
              str_or(json_object_get(root, "body"), ""));

    int have = 0;
    json_t *assets = json_object_get(root, "assets");
    size_t i;
    json_t *a;
    json_array_foreach(assets, i, a) {
        if (strcmp(str_or(json_object_get(a, "name"), ""), RELCHECK_ASSET))
            continue;
        json_t *sz = json_object_get(a, "size");
        r->bytes = json_is_integer(sz) ? (long)json_integer_value(sz) : 0;
        have = 1;
        break;
    }
    json_decref(root);
    if (!have) {
        snprintf(r->detail, sizeof(r->detail),
                 "release %s carries no firmware file", r->version);
        return 0;
    }
    r->available = 1;
    return 0;
}

/* v<major>.<minor>.<patch>[-suffix]: the three numbers and the suffix
 * (empty when none). 0 when s is not a version. */
static int parse_version(const char *s, long n[3], const char **suffix)
{
    if (*s == 'v' || *s == 'V')
        s++;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s))
            return 0;
        char *end;
        n[i] = strtol(s, &end, 10);
        s = end;
        if (i < 2) {
            if (*s != '.')
                return 0;
            s++;
        }
    }
    if (*s == '\0') {
        *suffix = "";
        return 1;
    }
    if (*s != '-')
        return 0;
    *suffix = s + 1;
    return 1;
}

int relcheck_is_newer(const char *tag, const char *current)
{
    long t[3], c[3];
    const char *ts, *cs;
    if (!parse_version(tag, t, &ts))
        return 0;
    if (!current || !parse_version(current, c, &cs))
        return 1;
    for (int i = 0; i < 3; i++)
        if (t[i] != c[i])
            return t[i] > c[i];
    /* The same number: a release outranks its prereleases, and among
     * prereleases the suffixes order as text. */
    if (!*ts)
        return *cs != '\0';
    if (!*cs)
        return 0;
    return strcmp(ts, cs) > 0;
}
