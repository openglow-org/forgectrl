/*
 * holds_test.c - host test: an extension package's holds, as the engine reads them
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * holds_read() over a directory of hold files: what stands, what is
 * dropped, what a file that cannot be read means, and what of a package's
 * text reaches the verdict.
 */
#include "../src/holds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static char dir[] = "/tmp/holds-test-XXXXXX";

static void put(const char *name, const char *text)
{
    char p[300];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void hold(const char *id, int required, int raised, const char *reason, double ts)
{
    char name[128], text[512];
    snprintf(name, sizeof(name), "%s.json", id);
    snprintf(text, sizeof(text), "{\"id\": \"%s\", \"required\": %s, \"raised\": %s, \"reason\": \"%s\", \"ts_mono\": %.3f}\n",
             id, required ? "true" : "false", raised ? "true" : "false", reason, ts);
    put(name, text);
}

static void clear_dir(void)
{
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -f %s/* %s/.[a-z]*", dir, dir);
    if (system(cmd) != 0)
        printf("FAIL: cannot empty %s\n", dir), fails++;
}

int main(void)
{
    holds_t h;
    const double now = 5000.0;
    if (!mkdtemp(dir)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }

    holds_read("/nonexistent/holds", "/nonexistent/required", now, &h);
    CHECK(!h.standing && h.files == 0, "a directory that does not exist holds something");
    holds_read(dir, NULL, now, &h);
    CHECK(!h.standing && h.files == 0 && !h.reason[0], "an empty directory holds something");

    /* Fresh: what the file says. */
    hold("org.example.badge", 1, 0, "", now - 0.4);
    hold("org.example.filter", 0, 0, "", now - 0.4);
    holds_read(dir, NULL, now, &h);
    CHECK(!h.standing && h.files == 2 && h.raised == 0, "two clear holds stand: %s", h.reason);
    hold("org.example.filter", 0, 1, "filter life under 5 percent", now - 0.4);
    holds_read(dir, NULL, now, &h);
    CHECK(h.standing && h.raised == 1 && !strcmp(h.reason, "org.example.filter: filter life under 5 percent"),
          "a raised advisory hold: standing %d, reason %s", h.standing, h.reason);
    hold("org.example.badge", 1, 1, "no badge presented", now - 1.9);
    holds_read(dir, NULL, now, &h);
    CHECK(h.standing && h.raised == 2 && !strcmp(h.reason, "org.example.badge: no badge presented"),
          "two raised holds: the first by name gives the reason: %s", h.reason);
    hold("org.example.badge", 1, 1, "", now);
    holds_read(dir, NULL, now, &h);
    CHECK(!strcmp(h.reason, "org.example.badge: hold"), "a hold with no words: %s", h.reason);

    /* Stale: the host is gone. Required stands whatever it said; advisory is dropped whatever it said. */
    clear_dir();
    hold("org.example.badge", 1, 0, "", now - 2.1);
    hold("org.example.filter", 0, 1, "filter life under 5 percent", now - 60);
    holds_read(dir, NULL, now, &h);
    CHECK(h.standing && h.stale_required == 1 && h.stale_advisory == 1 && h.raised == 0 &&
          !strcmp(h.reason, "org.example.badge: the extension host is not answering"),
          "stale: standing %d, required %d, advisory %d, reason %s", h.standing, h.stale_required, h.stale_advisory, h.reason);
    clear_dir();
    hold("org.example.filter", 0, 1, "filter life under 5 percent", now - 2.1);
    holds_read(dir, NULL, now, &h);
    CHECK(!h.standing && h.stale_advisory == 1, "a stale advisory hold alone stands");
    hold("org.example.filter", 1, 0, "", now + 30);
    holds_read(dir, NULL, now, &h);
    CHECK(h.standing && h.stale_required == 1, "a required hold stamped in the future is taken as fresh");

    /* What cannot be read stands. */
    clear_dir();
    put("org.example.broken.json", "{\"id\": \"org.example.broken\", \"required\": tru");
    holds_read(dir, NULL, now, &h);
    CHECK(h.standing && h.unreadable == 1 && !strcmp(h.reason, "hold file org.example.broken.json cannot be read"),
          "a cut-off file: %d %s", h.standing, h.reason);
    clear_dir();
    put("org.example.other.json", "{\"id\": \"org.example.badge\", \"required\": false, \"raised\": false, \"reason\": \"\", \"ts_mono\": 5000}");
    holds_read(dir, NULL, now, &h);
    CHECK(h.standing && h.unreadable == 1, "a file that speaks for another package is read");
    clear_dir();
    put("org.example.badge.json", "{\"id\": \"org.example.badge\", \"required\": 1, \"raised\": false, \"reason\": \"\", \"ts_mono\": 5000}");
    holds_read(dir, NULL, now, &h);
    CHECK(h.standing && h.unreadable == 1, "required as a number is read");
    clear_dir();
    put("org.example.badge.json", "");
    holds_read(dir, NULL, now, &h);
    CHECK(h.standing && h.unreadable == 1, "an empty file is read");

    /* What is not a hold file is not looked at. */
    clear_dir();
    put("org.example.badge.json.new", "not JSON");
    put(".org.example.badge.json", "not JSON");
    put("README", "not JSON");
    holds_read(dir, NULL, now, &h);
    CHECK(!h.standing && h.files == 0, "a file that is not <id>.json was read");

    /* A package's words go into a JSON string the controllers parse. */
    clear_dir();
    put("org.example.badge.json", "{\"id\": \"org.example.badge\", \"required\": true, \"raised\": true, "
        "\"reason\": \"a \\\"quote\\\", a \\\\ slash,\\na line, \\u00e9\", \"ts_mono\": 5000}");
    holds_read(dir, NULL, now, &h);
    CHECK(h.standing && !strchr(h.reason, '"') && !strchr(h.reason, '\\') && !strchr(h.reason, '\n'),
          "the reason kept a character that ends a JSON string: %s", h.reason);
    for (const char *c = h.reason; *c; c++)
        CHECK((unsigned char)*c >= 0x20 && (unsigned char)*c <= 0x7e, "the reason kept byte 0x%02x", (unsigned char)*c);
    char longwords[400];
    memset(longwords, 'x', sizeof(longwords) - 1);
    longwords[sizeof(longwords) - 1] = '\0';
    hold("org.example.badge", 1, 1, longwords, now);
    holds_read(dir, NULL, now, &h);
    CHECK(h.standing && strlen(h.reason) < HOLDS_REASON_MAX, "long words overran the reason");

    /* More files than there are package accounts. */
    clear_dir();
    for (int i = 0; i < HOLDS_MAX + 1; i++) {
        char id[64];
        snprintf(id, sizeof(id), "org.example.p%02d", i);
        hold(id, 0, 0, "", now);
    }
    holds_read(dir, NULL, now, &h);
    CHECK(h.standing && h.files == HOLDS_MAX && strstr(h.reason, "more hold files"), "too many files: %d %s", h.files, h.reason);

    /* A required hold nobody has spoken for: the tmpfs files are gone with a reboot, and a host that never
     * came up wrote none. The marker on /data is a name and nothing else. */
    clear_dir();
    char req[] = "/tmp/holds-required-XXXXXX", marker[300];
    if (!mkdtemp(req)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    holds_read(dir, req, now, &h);
    CHECK(!h.standing, "an empty required directory holds something");
    snprintf(marker, sizeof(marker), "%s/org.example.badge", req);
    fclose(fopen(marker, "w"));
    holds_read(dir, req, now, &h);
    CHECK(h.standing && h.stale_required == 1 && !strcmp(h.reason, "org.example.badge: the extension host is not answering"),
          "a required hold with no file: standing %d, reason %s", h.standing, h.reason);
    holds_read("/nonexistent/holds", req, now, &h);
    CHECK(h.standing && h.stale_required == 1, "a required hold with no holds directory at all does not stand");
    hold("org.example.badge", 1, 0, "", now);
    holds_read(dir, req, now, &h);
    CHECK(!h.standing && h.stale_required == 0, "the host speaks for it, clear: it stands anyway: %s", h.reason);
    hold("org.example.badge", 1, 0, "", now - 10);
    holds_read(dir, req, now, &h);
    CHECK(h.standing && h.stale_required == 1, "stale and marked: counted %d times", h.stale_required);
    unlink(marker);
    snprintf(marker, sizeof(marker), "%s/Not An Id", req);
    fclose(fopen(marker, "w"));
    snprintf(marker, sizeof(marker), "%s/.hidden", req);
    fclose(fopen(marker, "w"));
    hold("org.example.badge", 1, 0, "", now);
    holds_read(dir, req, now, &h);
    CHECK(!h.standing, "a name that is no package id was taken for a required hold: %s", h.reason);
    snprintf(marker, sizeof(marker), "rm -rf %s", req);
    if (system(marker) != 0)
        fails++;

    clear_dir();
    rmdir(dir);
    printf(fails ? "holds_test: %d FAILED\n" : "holds_test: all passed\n", fails);
    return fails ? 1 : 0;
}
