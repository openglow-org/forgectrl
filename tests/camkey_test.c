/*
 * camkey_test.c - host test: the camera key
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "../src/camkey.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); failures++; } } while (0)

int main(void)
{
    char dir[] = "/tmp/camkey-test-XXXXXX";
    if (!mkdtemp(dir)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    setenv("FORGECTRL_DATA_DIR", dir, 1);

    camkey_init();
    char first[CAMKEY_HEX + 1];
    snprintf(first, sizeof(first), "%s", camkey_get());
    CHECK(strlen(first) == CAMKEY_HEX && strspn(first, "0123456789abcdef") == CAMKEY_HEX,
          "a key of 32 hex characters is made at first start");
    CHECK(camkey_valid(first), "the key validates");
    CHECK(!camkey_valid("0000"), "a short key fails");
    CHECK(!camkey_valid(NULL), "no key fails");
    char other[CAMKEY_HEX + 1];
    memcpy(other, first, sizeof(other));
    other[5] = other[5] == 'a' ? 'b' : 'a';
    CHECK(!camkey_valid(other), "a one-character difference fails");

    camkey_init();
    CHECK(!strcmp(camkey_get(), first), "a reload finds the same key");

    CHECK(camkey_rotate() == 0, "rotation makes a new key");
    CHECK(strcmp(camkey_get(), first) != 0, "the new key differs");
    CHECK(!camkey_valid(first), "the old key stops working");
    CHECK(camkey_valid(camkey_get()), "the new key validates");

    /* A rotation that cannot write its key: the key in force stays, in
     * memory and on disk alike. The temporary name is a directory. */
    char tmp[300], kept[CAMKEY_HEX + 1];
    snprintf(kept, sizeof(kept), "%s", camkey_get());
    snprintf(tmp, sizeof(tmp), "%s/camera.key.tmp", dir);
    mkdir(tmp, 0700);
    CHECK(camkey_rotate() == -1, "a rotation that cannot write fails");
    CHECK(!strcmp(camkey_get(), kept) && camkey_valid(kept), "and the key in force stays valid");
    rmdir(tmp);
    camkey_init();
    CHECK(!strcmp(camkey_get(), kept), "the file holds the key in force");

    char path[300];
    snprintf(path, sizeof(path), "%s/camera.key", dir);
    unlink(path);
    rmdir(dir);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
