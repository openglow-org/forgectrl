/*
 * relcheck_test.c - host unit test for the published-release check
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The check reads the latest release from the GitHub REST API and
 * never requests the firmware file's own URL, because GitHub counts
 * every request of that URL as a download. The test drives the real
 * relcheck() with a scripted server: it answers the API URL the way
 * GitHub does (the release object, or 404 with no release, or an error
 * with a message) and fails any other URL, so a check that strays to
 * the download link fails here.
 *
 * The version order is tested on its own: a release is newer than a
 * lower number, than a prerelease of its own number, and than any
 * installed version that is not a release version (a development
 * build's stamp), and it is never newer than itself or a higher one.
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

static const char *srv_tag;     /* the published release's tag; NULL = none */
static int srv_asset;           /* that release carries forgefirm.fw */
static const char *srv_notes;   /* its notes (JSON-encoded string body) */
static int srv_code;            /* nonzero: the API answers this instead */
static const char *srv_message; /* with this message */
static const char *srv_raw;     /* nonzero: the API answers this text */
static int srv_offline;         /* nothing answers */
static int srv_requests;        /* requests the checker made */
static int srv_strayed;         /* requests to anything but the API */

static void server(const char *tag, int asset)
{
    srv_tag = tag;
    srv_asset = asset;
    srv_notes = "\"## What is new\\n\\n- The lens frame\\n- A \\\"quoted\\\" note\\n\"";
    srv_code = 0;
    srv_message = NULL;
    srv_raw = NULL;
    srv_offline = 0;
    srv_requests = 0;
    srv_strayed = 0;
}

/* The API, as far as a checker can tell: the release object with its
 * assets (forgefirm.fw among them when the release carries it, with a
 * sibling asset either way), the notes as the release body. */
static int fake_fetch(const char *url, char *body, size_t len, int *http)
{
    srv_requests++;
    body[0] = '\0';
    *http = 0;
    if (srv_offline)
        return -1;
    if (strcmp(url, RELCHECK_API_URL) != 0) {
        srv_strayed++;
        *http = 200;
        snprintf(body, len, "%s", "not the api");
        return 0;
    }
    if (srv_raw) {
        *http = 200;
        snprintf(body, len, "%s", srv_raw);
        return 0;
    }
    if (srv_code) {
        *http = srv_code;
        if (srv_message)
            snprintf(body, len, "{\"message\":\"%s\",\"documentation_url\":"
                                "\"https://docs.github.com/rest\"}",
                     srv_message);
        return 0;
    }
    if (!srv_tag) {
        *http = 404;
        snprintf(body, len, "{\"message\":\"Not Found\"}");
        return 0;
    }
    *http = 200;
    snprintf(body, len,
             "{\"url\":\"https://api.github.com/repos/openglow-org/forgefirm/"
             "releases/1\",\"tag_name\":\"%s\",\"name\":\"ForgeFIRM %s\","
             "\"draft\":false,\"prerelease\":false,"
             "\"published_at\":\"2026-09-11T20:30:15Z\","
             "\"assets\":[{\"name\":\"sha256sums.txt\",\"size\":412,"
             "\"browser_download_url\":\"https://github.com/openglow-org/"
             "forgefirm/releases/download/%s/sha256sums.txt\"}%s],"
             "\"body\":%s}",
             srv_tag, srv_tag, srv_tag,
             srv_asset ? ",{\"name\":\"forgefirm.fw\",\"size\":87654321,"
                         "\"browser_download_url\":\"https://github.com/"
                         "openglow-org/forgefirm/releases/download/v0.0.4/"
                         "forgefirm.fw\"}"
                       : "",
             srv_notes);
    return 0;
}

/* ---------------------------------------------------------- the cases */

static void t_published_with_file(void)
{
    printf("a published release with the file\n");
    server("v0.0.4", 1);
    struct relcheck_result r;
    int rc = relcheck(&r, fake_fetch);
    printf("  version %s published %s bytes %ld detail '%s'\n",
           r.version, r.published, r.bytes, r.detail);
    CHECK(rc == 0, "the check answers");
    CHECK(r.available, "available");
    CHECK(strcmp(r.version, "v0.0.4") == 0, "the tag");
    CHECK(strcmp(r.published, "2026-09-11T20:30:15Z") == 0, "the date");
    CHECK(r.bytes == 87654321, "the firmware file's size");
    CHECK(strstr(r.notes, "- A \"quoted\" note") != NULL,
          "the notes, decoded");
    CHECK(r.detail[0] == '\0', "no detail");
    CHECK(srv_requests == 1 && srv_strayed == 0,
          "one request, to the API only");
}

static void t_no_release(void)
{
    printf("no published release\n");
    server(NULL, 0);
    struct relcheck_result r;
    int rc = relcheck(&r, fake_fetch);
    printf("  detail '%s'\n", r.detail);
    CHECK(rc == 0, "the check answers");
    CHECK(!r.available, "not available");
    CHECK(strcmp(r.detail, "no published release found (HTTP 404)") == 0,
          "named as no release");
    CHECK(r.version[0] == '\0', "no version");
}

static void t_release_without_file(void)
{
    printf("a published release that carries no firmware file\n");
    server("v0.0.4", 0);
    struct relcheck_result r;
    int rc = relcheck(&r, fake_fetch);
    printf("  detail '%s'\n", r.detail);
    CHECK(rc == 0, "the check answers");
    CHECK(!r.available, "not available");
    CHECK(strcmp(r.detail, "release v0.0.4 carries no firmware file") == 0,
          "named as a release without the file");
    CHECK(srv_strayed == 0, "the file's URL was not requested");
}

static void t_server_error(void)
{
    printf("the release server fails\n");
    server("v0.0.4", 1);
    srv_code = 503;
    struct relcheck_result r;
    int rc = relcheck(&r, fake_fetch);
    printf("  detail '%s'\n", r.detail);
    CHECK(rc == 0, "the check answers");
    CHECK(!r.available, "not available");
    CHECK(strcmp(r.detail, "release server error (HTTP 503)") == 0,
          "named as a server error");
}

static void t_rate_limited(void)
{
    printf("the API rate limit\n");
    server("v0.0.4", 1);
    srv_code = 403;
    srv_message = "API rate limit exceeded for 203.0.113.7.";
    struct relcheck_result r;
    int rc = relcheck(&r, fake_fetch);
    printf("  detail '%s'\n", r.detail);
    CHECK(rc == 0, "the check answers");
    CHECK(!r.available, "not available");
    CHECK(strcmp(r.detail, "release server error (HTTP 403): API rate "
                           "limit exceeded for 203.0.113.7.") == 0,
          "the API's message is carried");
}

static void t_offline(void)
{
    printf("offline\n");
    server("v0.0.4", 1);
    srv_offline = 1;
    struct relcheck_result r;
    int rc = relcheck(&r, fake_fetch);
    CHECK(rc == -1, "the check reports unreachable");
    CHECK(!r.available && r.version[0] == '\0', "the result is cleared");
}

static void t_odd_tag(void)
{
    printf("a tag that is not a tag\n");
    server("v0.0.4\\\"; rm -rf /", 1);
    struct relcheck_result r;
    int rc = relcheck(&r, fake_fetch);
    printf("  detail '%s'\n", r.detail);
    CHECK(rc == 0, "the check answers");
    CHECK(!r.available, "not available");
    CHECK(r.version[0] == '\0', "nothing of the tag is kept");
    CHECK(strstr(r.detail, "not a tag") != NULL, "named as not a tag");
}

static void t_not_json(void)
{
    printf("a reply that is not the API's\n");
    server("v0.0.4", 1);
    srv_raw = "<html>a captive portal</html>";
    struct relcheck_result r;
    int rc = relcheck(&r, fake_fetch);
    printf("  detail '%s'\n", r.detail);
    CHECK(rc == 0, "the check answers");
    CHECK(!r.available, "not available");
    CHECK(strcmp(r.detail, "release server answered badly (HTTP 200)") == 0,
          "named as a bad answer");
}

static void t_long_notes(void)
{
    printf("notes past the ceiling, cut at a character boundary\n");
    server("v0.0.4", 1);
    /* Notes of two-byte characters, longer than the ceiling: the cut
     * must land between characters, so the kept text stays UTF-8. */
    static char notes[RELCHECK_NOTES_MAX * 2 + 8];
    size_t o = 0;
    notes[o++] = '"';
    while (o + 4 < sizeof(notes) - 2) {
        notes[o++] = (char)0xC3;        /* U+00E9 in UTF-8 */
        notes[o++] = (char)0xA9;
    }
    notes[o++] = '"';
    notes[o] = '\0';
    srv_notes = notes;
    struct relcheck_result r;
    int rc = relcheck(&r, fake_fetch);
    size_t n = strlen(r.notes);
    CHECK(rc == 0 && r.available, "the check answers");
    CHECK(n < RELCHECK_NOTES_MAX, "the notes are cut to the ceiling");
    CHECK(n % 2 == 0 && ((unsigned char)r.notes[n - 1]) == 0xA9,
          "the cut lands on a character boundary");
}

static void t_version_order(void)
{
    printf("the version order\n");
    CHECK(relcheck_is_newer("v0.0.4", "v0.0.3"), "a higher patch is newer");
    CHECK(relcheck_is_newer("v0.1.0", "v0.0.99"), "a higher minor is newer");
    CHECK(relcheck_is_newer("v1.0.0", "v0.9.9"), "a higher major is newer");
    CHECK(!relcheck_is_newer("v0.0.3", "v0.0.3"), "the same is not newer");
    CHECK(!relcheck_is_newer("v0.0.3", "v0.0.4"), "a lower one is not newer");
    CHECK(!relcheck_is_newer("v0.0.10", "v0.1.0"), "numbers, not text");
    CHECK(relcheck_is_newer("v0.0.10", "v0.0.9"), "ten is past nine");
    CHECK(relcheck_is_newer("v0.0.4", "20260911203113 (dev)"),
          "a release is newer than a development build");
    CHECK(relcheck_is_newer("v0.0.1", "20260911203113 (dev)"),
          "any release is newer than a development build");
    CHECK(relcheck_is_newer("v0.0.4", ""), "a release is newer than unknown");
    CHECK(relcheck_is_newer("v0.0.4", NULL), "a release is newer than none");
    CHECK(relcheck_is_newer("v0.0.4", "v0.0.4-rc1"),
          "a release is newer than its prerelease");
    CHECK(!relcheck_is_newer("v0.0.4-rc1", "v0.0.4"),
          "a prerelease is not newer than its release");
    CHECK(relcheck_is_newer("v0.0.4-rc2", "v0.0.4-rc1"),
          "prereleases order among themselves");
    CHECK(relcheck_is_newer("0.0.4", "v0.0.3"), "the v is optional");
    CHECK(!relcheck_is_newer("nightly", "v0.0.3"),
          "a tag that is not a version is not newer than a release");
    CHECK(!relcheck_is_newer("v0.0", "v0.0.3"), "two numbers are not a version");
}

int main(void)
{
    t_published_with_file();
    t_no_release();
    t_release_without_file();
    t_server_error();
    t_rate_limited();
    t_offline();
    t_odd_tag();
    t_not_json();
    t_long_notes();
    t_version_order();
    printf("%s: %d failure%s\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
