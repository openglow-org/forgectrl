/*
 * relcheck_test.c - host unit test for the published-release check
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The release check resolves the published version from the redirect
 * chain behind the fixed-name asset URL. The chain has three hops: the
 * fixed-name URL redirects to the tagged download URL, the tagged URL
 * redirects to the asset store, and the store answers 200 from a URL
 * that names an object id and nothing else. The tag lives on the first
 * hop only. A check that follows the chain and reads the tag from the
 * effective URL therefore finds none exactly when a release with the
 * file is published, the state it exists to detect, and reports
 * "release server error (HTTP 200)" against a good release. That is the
 * regression this test holds shut.
 *
 * The test drives the real relcheck() with a model of curl against a
 * scripted server. The model honors -I with and without -L and the -w
 * tokens a checker may use, so it answers a request the way the real
 * server does, whichever way the checker asks.
 */
#include "../src/relcheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* ------------------------------------------------- the scripted server */

#define TAGGED_URL "https://github.com/openglow-org/forgefirm/releases/download/"
/* The asset store, as the real chain names it: an object id, a signed
 * query, the file name only inside a content-disposition parameter. */
#define STORE_URL \
    "https://release-assets.githubusercontent.com/github-production-release-asset/" \
    "255357527/16fdbb89-0b13-48b3-9c01-5684472926f3?sp=r&sv=2018-11-09&sr=b" \
    "&rscd=attachment%3B+filename%3Dforgefirm.fw" \
    "&response-content-disposition=attachment%3B%20filename%3Dforgefirm.fw" \
    "&response-content-type=application%2Foctet-stream"

static const char *srv_tag;     /* the published release's tag; NULL = none */
static int srv_asset;           /* that release carries forgefirm.fw */
static int srv_first_code;      /* nonzero: the fixed-name URL answers this */
static int srv_offline;         /* nothing answers */
static int srv_requests;        /* requests the checker made */

static void server(const char *tag, int asset)
{
    srv_tag = tag;
    srv_asset = asset;
    srv_first_code = 0;
    srv_offline = 0;
    srv_requests = 0;
}

struct hop {
    int  code;
    char redirect[512];         /* the Location answered, "" when none */
};

static void answer(const char *url, struct hop *h)
{
    h->redirect[0] = '\0';
    if (strcmp(url, RELCHECK_LATEST_URL) == 0) {
        if (srv_first_code) {
            h->code = srv_first_code;
            return;
        }
        if (!srv_tag) {
            h->code = 404;
            return;
        }
        h->code = 302;
        snprintf(h->redirect, sizeof(h->redirect),
                 TAGGED_URL "%s/forgefirm.fw", srv_tag);
        return;
    }
    if (strncmp(url, TAGGED_URL, strlen(TAGGED_URL)) == 0) {
        if (!srv_asset) {
            h->code = 404;
            return;
        }
        h->code = 302;
        snprintf(h->redirect, sizeof(h->redirect), "%s", STORE_URL);
        return;
    }
    h->code = 200;                      /* the asset store */
}

/* ------------------------------------------------------ the curl model */

/* True when the command line carries the short flag c in a bundle
 * (-sIL) or alone (-L). */
static int has_flag(const char *cmd, char c)
{
    const char *p = cmd;
    while (*p) {
        while (*p == ' ')
            p++;
        const char *t = p;
        while (*p && *p != ' ')
            p++;
        if (t[0] == '-' && t[1] != '-' && memchr(t + 1, c, (size_t)(p - t - 1)))
            return 1;
    }
    return 0;
}

/* curl, as far as a checker can tell: -I with or without -L (or
 * --location), one https URL, and a -w format quoted with single quotes
 * that may use %{http_code}, %{redirect_url} and %{url_effective}. The
 * output is the format rendered for the last response, exactly as curl
 * renders it: the redirect target of the last response when it was a
 * redirect and empty otherwise, the URL the last response came from as
 * the effective URL. */
static int fake_curl(char *out, size_t outlen, const char *cmd)
{
    srv_requests++;
    out[0] = '\0';
    if (srv_offline)
        return 7;                       /* curl: failed to connect */

    int follow = has_flag(cmd, 'L') || strstr(cmd, "--location");
    const char *u = strstr(cmd, "https://");
    if (!u) {
        printf("  (model: no URL in the command)\n");
        return 3;
    }
    char cur[512];
    size_t n = strcspn(u, " ");
    snprintf(cur, sizeof(cur), "%.*s", (int)n, u);

    struct hop h;
    answer(cur, &h);
    while (follow && h.redirect[0]) {
        snprintf(cur, sizeof(cur), "%s", h.redirect);
        answer(cur, &h);
    }

    const char *fmt = strstr(cmd, "-w '");
    if (!fmt)
        return 0;
    fmt += 4;
    size_t o = 0;
    while (*fmt && *fmt != '\'' && o + 1 < outlen) {
        const char *sub = NULL;
        char code[8];
        if (strncmp(fmt, "%{http_code}", 12) == 0) {
            snprintf(code, sizeof(code), "%03d", h.code);
            sub = code;
            fmt += 12;
        } else if (strncmp(fmt, "%{redirect_url}", 15) == 0) {
            sub = h.redirect;
            fmt += 15;
        } else if (strncmp(fmt, "%{url_effective}", 16) == 0) {
            sub = cur;
            fmt += 16;
        }
        if (sub) {
            o += (size_t)snprintf(out + o, outlen - o, "%s", sub);
            if (o >= outlen)
                o = outlen - 1;
        } else {
            out[o++] = *fmt++;
        }
    }
    out[o] = '\0';
    return 0;
}

/* ---------------------------------------------------------- the cases */

static int well_formed(const char *body)
{
    size_t n = strlen(body);
    return n > 2 && body[0] == '{' && body[n - 1] == '}';
}

static void t_published_with_file(void)
{
    printf("a published release with the file, the installed one older\n");
    server("v0.0.4", 1);
    char body[256] = "";
    int rc = relcheck(body, sizeof(body), "v0.0.3", fake_curl);
    printf("  body: %s\n", body);
    CHECK(rc == 0, "the check answers");
    CHECK(well_formed(body), "the body is one JSON object");
    CHECK(strstr(body, "\"available\":true") != NULL, "available");
    CHECK(strstr(body, "\"version\":\"v0.0.4\"") != NULL,
          "the tag comes from the first hop");
    CHECK(strstr(body, "\"current\":\"v0.0.3\"") != NULL,
          "the installed version is reported");
    CHECK(strstr(body, "\"new\":true") != NULL, "newer than installed");
    CHECK(srv_requests == 2, "two requests: the tag hop, then the chain");
}

static void t_up_to_date(void)
{
    printf("the installed version is the published one\n");
    server("v0.0.3", 1);
    char body[256] = "";
    int rc = relcheck(body, sizeof(body), "v0.0.3", fake_curl);
    printf("  body: %s\n", body);
    CHECK(rc == 0 && well_formed(body), "the check answers");
    CHECK(strstr(body, "\"available\":true") != NULL, "available");
    CHECK(strstr(body, "\"version\":\"v0.0.3\"") != NULL, "the tag");
    CHECK(strstr(body, "\"new\":false") != NULL, "not new");
}

static void t_no_release(void)
{
    printf("no published release\n");
    server(NULL, 0);
    char body[256] = "";
    int rc = relcheck(body, sizeof(body), "v0.0.3", fake_curl);
    printf("  body: %s\n", body);
    CHECK(rc == 0 && well_formed(body), "the check answers");
    CHECK(strstr(body, "\"available\":false") != NULL, "not available");
    CHECK(strstr(body, "\"detail\":\"no published release found (HTTP 404)\"")
          != NULL, "named as no release");
    CHECK(strstr(body, "\"current\":\"v0.0.3\"") != NULL,
          "the installed version is still reported");
    CHECK(srv_requests == 1, "no chain request without a tag");
}

static void t_release_without_file(void)
{
    printf("a published release that carries no firmware file\n");
    server("v0.0.4", 0);
    char body[256] = "";
    int rc = relcheck(body, sizeof(body), "v0.0.3", fake_curl);
    printf("  body: %s\n", body);
    CHECK(rc == 0 && well_formed(body), "the check answers");
    CHECK(strstr(body, "\"available\":false") != NULL, "not available");
    CHECK(strstr(body, "\"detail\":\"release v0.0.4 carries no firmware file "
                       "(HTTP 404)\"") != NULL,
          "named as a release without the file");
    CHECK(srv_requests == 2, "the chain was asked");
}

static void t_server_error(void)
{
    printf("the release server fails\n");
    server("v0.0.4", 1);
    srv_first_code = 503;
    char body[256] = "";
    int rc = relcheck(body, sizeof(body), "v0.0.3", fake_curl);
    printf("  body: %s\n", body);
    CHECK(rc == 0 && well_formed(body), "the check answers");
    CHECK(strstr(body, "\"available\":false") != NULL, "not available");
    CHECK(strstr(body, "\"detail\":\"release server error (HTTP 503)\"")
          != NULL, "named as a server error");
    CHECK(srv_requests == 1, "no chain request after a failed first hop");
}

static void t_offline(void)
{
    printf("offline\n");
    server("v0.0.4", 1);
    srv_offline = 1;
    char body[256] = "";
    int rc = relcheck(body, sizeof(body), "v0.0.3", fake_curl);
    CHECK(rc == -1, "the check reports unreachable");
    CHECK(body[0] == '\0', "the body is left alone");
}

static void t_odd_tag(void)
{
    printf("a redirect whose tag segment is not a tag\n");
    server("v0.0.4\"x", 1);
    char body[256] = "";
    int rc = relcheck(body, sizeof(body), "v0.0.3", fake_curl);
    printf("  body: %s\n", body);
    CHECK(rc == 0 && well_formed(body), "the check answers");
    CHECK(strstr(body, "\"available\":false") != NULL, "not available");
    CHECK(strstr(body, "v0.0.4") == NULL, "nothing of the segment is quoted");
    CHECK(strstr(body, "(HTTP 302)") != NULL, "the first hop's status is reported");
    CHECK(srv_requests == 1, "no chain request without a tag");
}

static void t_current_unknown(void)
{
    printf("the installed version is unknown\n");
    server("v0.0.4", 1);
    char body[256] = "";
    int rc = relcheck(body, sizeof(body), "", fake_curl);
    printf("  body: %s\n", body);
    CHECK(rc == 0 && well_formed(body), "the check answers");
    CHECK(strstr(body, "\"current\":\"\"") != NULL, "an empty current");
    CHECK(strstr(body, "\"new\":true") != NULL, "anything published is new");
}

int main(void)
{
    t_published_with_file();
    t_up_to_date();
    t_no_release();
    t_release_without_file();
    t_server_error();
    t_offline();
    t_odd_tag();
    t_current_unknown();
    printf("%s: %d failure%s\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
