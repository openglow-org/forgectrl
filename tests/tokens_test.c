/*
 * tokens_test.c - host test for the scoped-token store
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Cases: what a token can be named and granted, and what it cannot; the
 * token's shape, and a store that holds its hash and never the token; the
 * judgment (a held capability, one not held, a route no token reaches,
 * the ".any" form, a token that is not this machine's in every shape of
 * wrong); a use is remembered in memory and reaches the file at a flush,
 * not at the use; a reload keeps every token; a revoke ends one and only
 * that one; sixteen at most, one name once; a damaged line is dropped and
 * its neighbors kept.
 */
#include "../src/tokens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static char root[128], path[192];

static const char *file_text(void)
{
    static char body[8192];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(body, 1, sizeof(body) - 1, f) : 0;
    if (f)
        fclose(f);
    body[n] = '\0';
    return body;
}

static const char *json(void)
{
    static char body[8192];
    if (tokens_json(body, sizeof(body)) != 0)
        snprintf(body, sizeof(body), "(does not fit)");
    return body;
}

static void test_create_rules(void)
{
    char tok[TOKENS_TEXT_LEN + 1], id[TOKENS_ID_HEX + 1], err[160];
    const char *bad_names[] = { "", " lead", "trail ", "semi;colon", "co:lon", "quo\"te",
                                "0123456789012345678901234567890123456789x" };
    for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++)
        CHECK(tokens_create(bad_names[i], "events", tok, id, err, sizeof(err)) == -1 && !tok[0],
              "the name \"%s\" was taken", bad_names[i]);
    const char *bad_caps[] = { "", " , ", "settings.write", "machine.read,panel", "motion", "motion.jog.x",
                               "camera.any", "role:homing" };
    for (size_t i = 0; i < sizeof(bad_caps) / sizeof(bad_caps[0]); i++)
        CHECK(tokens_create("rules", bad_caps[i], tok, id, err, sizeof(err)) == -1 && !tok[0],
              "the capabilities \"%s\" were granted", bad_caps[i]);
    CHECK(strstr(err, "role:homing") && strstr(err, "not a capability"), "the refusal does not name the word: %s", err);
    CHECK(!tokens_cap_known("settings.write") && !tokens_cap_known(NULL) && tokens_cap_known("motion.job"),
          "the closed list");
    CHECK(strstr(json(), "\"tokens\":[]"), "a refused create left a token: %s", json());
}

static void test_shape_and_check(void)
{
    char hub[TOKENS_TEXT_LEN + 1], pend[TOKENS_TEXT_LEN + 1], id[TOKENS_ID_HEX + 1], id2[TOKENS_ID_HEX + 1];
    char seen[TOKENS_ID_HEX + 1], err[160] = "";
    CHECK(tokens_create("Home Assistant", "machine.read, events,camera.lid", hub, id, err, sizeof(err)) == 0,
          "create: %s", err);
    CHECK(strlen(hub) == TOKENS_TEXT_LEN && !strncmp(hub, "fft_", 4) && strlen(id) == TOKENS_ID_HEX,
          "the token's shape: \"%s\" id \"%s\"", hub, id);
    CHECK(tokens_create("pendant", "motion.jog", pend, id2, err, sizeof(err)) == 0, "create: %s", err);
    CHECK(strcmp(hub, pend) && strcmp(id, id2), "two tokens alike");

    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "the store's mode is %o", st.st_mode & 0777);
    CHECK(!strstr(file_text(), hub + 4) && !strstr(file_text(), pend + 4), "the store holds a token");
    CHECK(strstr(file_text(), id) && strstr(file_text(), "Home Assistant"), "the store: %s", file_text());
    CHECK(!strstr(json(), hub + 4) && strstr(json(), "\"caps\":[\"machine.read\",\"events\",\"camera.lid\"]") &&
          strstr(json(), "\"name\":\"pendant\""), "the list: %s", json());

    CHECK(tokens_check(hub, "machine.read", seen) == 1 && !strcmp(seen, id), "a held capability");
    CHECK(tokens_check(hub, "motion.jog", seen) == 0 && !strcmp(seen, id), "a capability not held");
    CHECK(tokens_check(hub, NULL, seen) == 0 && !strcmp(seen, id), "a route no token reaches");
    CHECK(tokens_check(hub, "camera.lid", seen) == 1 && tokens_check(hub, "camera.head", seen) == 0, "one camera");
    CHECK(tokens_check(hub, "camera.any", seen) == 1 && tokens_check(pend, "camera.any", seen) == 0, "camera.any");
    CHECK(tokens_check(pend, "motion.jog", seen) == 1 && tokens_check(pend, "motion.job", seen) == 0,
          "motion.jog is not motion.job");
    CHECK(tokens_check(pend, "motion.any", seen) == 1, "motion.any");

    char wrong[TOKENS_TEXT_LEN + 1];
    snprintf(wrong, sizeof(wrong), "%s", hub);
    wrong[20] = wrong[20] == '0' ? '1' : '0';
    CHECK(tokens_check(wrong, "machine.read", seen) == -1 && !seen[0], "one digit off passed");
    const char *not_ours[] = { NULL, "", "fft_", "0123456789abcdef0123456789abcdef", hub + 4,
                               "fft_0123456789ABCDEF0123456789ABCDEF", "fft_0123456789abcdef0123456789abcdef0" };
    for (size_t i = 0; i < sizeof(not_ours) / sizeof(not_ours[0]); i++)
        CHECK(tokens_check(not_ours[i], "machine.read", seen) == -1, "\"%s\" passed", not_ours[i] ? not_ours[i] : "(null)");
    CHECK(tokens_looks_scoped(wrong) && !tokens_looks_scoped(hub + 4) && !tokens_looks_scoped(NULL), "the prefix");

    /* A use is in memory; the file learns of it at a flush. */
    CHECK(!strstr(json(), "\"last_used\":0"), "a use was not remembered: %s", json());
    CHECK(strstr(file_text(), ":0:machine.read"), "the use reached the file at once: %s", file_text());
    tokens_flush();
    CHECK(!strstr(file_text(), ":0:machine.read"), "the flush wrote no use: %s", file_text());

    /* A reload keeps both, and the times. */
    tokens_init();
    CHECK(tokens_check(hub, "events", seen) == 1 && tokens_check(pend, "motion.jog", seen) == 1, "after a reload");
    /* A token that may ride in a URL: every capability a camera's. */
    char cam[TOKENS_TEXT_LEN + 1], id3[TOKENS_ID_HEX + 1];
    CHECK(tokens_create("viewer", "camera.lid,camera.head", cam, id3, err, sizeof(err)) == 0, "create: %s", err);
    CHECK(tokens_holds_only(cam, "camera.") == 1, "a camera-only token");
    CHECK(tokens_holds_only(hub, "camera.") == 0, "a token with a camera and more");
    CHECK(tokens_holds_only(pend, "camera.") == 0, "a token with no camera");
    CHECK(tokens_holds_only("fft_00000000000000000000000000000000", "camera.") == 0 &&
          tokens_holds_only(NULL, "camera.") == 0, "no token at all");
    CHECK(tokens_revoke(id3, err, sizeof(err)) == 0, "revoke: %s", err);

    CHECK(tokens_revoke("00000000", err, sizeof(err)) == -1 && strstr(err, "no such"), "a revoke of nothing: %s", err);
    CHECK(tokens_revoke(id, err, sizeof(err)) == 0, "revoke: %s", err);
    CHECK(tokens_check(hub, "machine.read", seen) == -1, "a revoked token passed");
    CHECK(tokens_check(pend, "motion.jog", seen) == 1, "the revoke took the other token too");
    CHECK(!strstr(file_text(), id) && !strstr(json(), "Home Assistant"), "the revoked token is still listed");
    tokens_init();
    CHECK(tokens_check(hub, "machine.read", seen) == -1, "a revoked token came back at the reload");
    CHECK(tokens_revoke(id2, err, sizeof(err)) == 0, "revoke: %s", err);
}

static void test_limits(void)
{
    char tok[TOKENS_TEXT_LEN + 1], id[TOKENS_ID_HEX + 1], first[TOKENS_ID_HEX + 1] = "", err[160] = "", name[32];
    for (int i = 0; i < TOKENS_MAX; i++) {
        snprintf(name, sizeof(name), "client %d", i);
        CHECK(tokens_create(name, "events", tok, id, err, sizeof(err)) == 0, "create %d: %s", i, err);
        if (i == 0)
            snprintf(first, sizeof(first), "%s", id);
    }
    CHECK(tokens_create("one too many", "events", tok, id, err, sizeof(err)) == -1 && strstr(err, "at most"),
          "a seventeenth token: %s", err);
    CHECK(tokens_revoke(first, err, sizeof(err)) == 0, "revoke: %s", err);
    CHECK(tokens_create("client 5", "events", tok, id, err, sizeof(err)) == -1 && strstr(err, "exists already"),
          "one name twice: %s", err);
    CHECK(tokens_create("sixteenth", "events", tok, id, err, sizeof(err)) == 0, "a place freed by a revoke: %s", err);
    char big[8192];
    CHECK(tokens_json(big, sizeof(big)) == 0, "a full list does not fit");
    CHECK(tokens_json(big, 200) == -1, "a list cut short was served");
}

static void test_damaged_store(void)
{
    char seen[TOKENS_ID_HEX + 1], tok[TOKENS_TEXT_LEN + 1], id[TOKENS_ID_HEX + 1], err[160];
    unlink(path);
    tokens_init();
    CHECK(tokens_create("kept", "machine.read", tok, id, err, sizeof(err)) == 0, "create: %s", err);
    FILE *f = fopen(path, "a");
    fputs("not a token\n", f);
    fputs("deadbeef:00:0:0:machine.read:short hash\n", f);
    fprintf(f, "deadbeef:%064d:0:0:settings.write:bad cap\n", 0);
    fprintf(f, "cafef00d:%064d:7:0:events:whole\n", 0);
    fclose(f);
    tokens_init();
    CHECK(tokens_check(tok, "machine.read", seen) == 1, "the good token fell with the bad lines");
    CHECK(strstr(json(), "\"name\":\"whole\"") && !strstr(json(), "bad cap") && !strstr(json(), "short hash"),
          "the list after a damaged store: %s", json());
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/tokens_test.%d", (int)getpid());
    mkdir(root, 0755);
    setenv("FORGECTRL_DATA_DIR", root, 1);
    snprintf(path, sizeof(path), "%s/tokens", root);
    tokens_init();

    test_create_rules();
    test_shape_and_check();
    test_limits();
    test_damaged_store();

    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) { }
    printf(fails ? "tokens_test: %d FAILED\n" : "tokens_test: all passed\n", fails);
    return fails ? 1 : 0;
}
