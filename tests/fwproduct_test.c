/*
 * fwproduct_test.c - host test for the product gate on the firmware paths
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Cases: the product is read out of `fwup -m` text as fwup prints it,
 * quoted or bare, and only from a line that starts with the key (fwup
 * prints the product first, so the first such line is the real one); each
 * firmware product passes under its own key and unsigned, and under the
 * other's key it does not; an extension package is refused in every
 * class, in words that say what it is; and an archive with no product,
 * another product, or a class that does not exist is refused. Then the
 * gate as the firmware paths call it, over a stand-in fwup on PATH that
 * prints what the real one prints for a ForgeFIRM release, a factory
 * archive, and an extension package: the release passes under the release
 * key (the text reaches the parser with its quotes and line ends, which a
 * JSON-sanitized capture would not have), the others are judged by their
 * own product, and an archive fwup cannot read is refused.
 */
#include "../src/fwproduct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int refused_with(const char *product, int cls, const char *words)
{
    const char *why = fwproduct_refusal(product, cls);
    return why && strstr(why, words);
}

/* What `fwup -m` prints, taken from the real archives. */
static const char RELEASE_M[] =
    "meta-product=\"ForgeFIRM firmware\"\n"
    "meta-description=\"ForgeFIRM firmware for Glowforge brand CNC lasers\"\n"
    "meta-version=v0.0.6\nmeta-author=OpenGlow\nmeta-platform=glowforge\nmeta-architecture=arm\n";
static const char FACTORY_M[] =
    "meta-product=\"Glowforge firmware\"\nmeta-version=\"1.7.0-38\"\nmeta-author=\"Glowforge, Inc.\"\n"
    "meta-platform=glowforge\n";
static const char EXTENSION_M[] =
    "meta-product=\"ForgeFIRM extension\"\nmeta-description=org.example.notify\nmeta-version=1.0.0\n"
    "meta-platform=\"forgefirm-ext\"\n";

static char dir[64] = "/tmp/fwproduct-test.XXXXXX";

/* The stand-in: `fwup -m -i <file>` prints <file> and exits 0, or exits 1
 * when the file is not there, the way fwup fails on what it cannot read. */
static void stand_in(void)
{
    char path[128], env[256];
    snprintf(path, sizeof(path), "%s/fwup", dir);
    FILE *f = fopen(path, "w");
    fputs("#!/bin/sh\n[ \"$1\" = -m ] && [ \"$2\" = -i ] && [ -f \"$3\" ] && exec cat \"$3\"\nexit 1\n", f);
    fclose(f);
    chmod(path, 0755);
    snprintf(env, sizeof(env), "%s:%s", dir, getenv("PATH") ? getenv("PATH") : "/bin:/usr/bin");
    setenv("PATH", env, 1);
}

static const char *archive(const char *name, const char *text)
{
    static char path[128];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
    return path;
}

int main(void)
{
    char p[64];

    CHECK(fwproduct_parse("meta-product=\"ForgeFIRM firmware\"\nmeta-version=v0.0.6\n", p, sizeof(p)) == 0
          && strcmp(p, "ForgeFIRM firmware") == 0, "the release's own text gave \"%s\"", p);
    CHECK(fwproduct_parse("meta-version=\"1.7.0-38\"\nmeta-product=\"Glowforge firmware\"\r\n", p, sizeof(p)) == 0
          && strcmp(p, "Glowforge firmware") == 0, "the factory's text gave \"%s\"", p);
    CHECK(fwproduct_parse("meta-product=bare\n", p, sizeof(p)) == 0 && strcmp(p, "bare") == 0, "a bare value gave \"%s\"", p);
    CHECK(fwproduct_parse("meta-description=\"meta-product=ForgeFIRM firmware\"\nmeta-version=1\n", p, sizeof(p)) != 0
          && p[0] == '\0', "a product inside another line's value was taken: \"%s\"", p);
    CHECK(fwproduct_parse("meta-description=\"x\nmeta-product=\"ForgeFIRM extension\"\n", p, sizeof(p)) == 0
          && strcmp(p, "ForgeFIRM extension") == 0, "the first line that starts with the key: \"%s\"", p);
    CHECK(fwproduct_parse("", p, sizeof(p)) != 0 && fwproduct_parse("meta-version=1\n", p, sizeof(p)) != 0, "no product line");
    char tiny[8];
    CHECK(fwproduct_parse("meta-product=\"ForgeFIRM firmware\"\n", tiny, sizeof(tiny)) != 0 && tiny[0] == '\0',
          "a value that does not fit was cut, not refused");

    CHECK(fwproduct_refusal(FWPRODUCT_FORGEFIRM, FWCLASS_RELEASE) == NULL, "ForgeFIRM firmware under the release key");
    CHECK(fwproduct_refusal(FWPRODUCT_FORGEFIRM, FWCLASS_NONE) == NULL, "ForgeFIRM firmware unsigned");
    CHECK(fwproduct_refusal(FWPRODUCT_FACTORY, FWCLASS_FACTORY) == NULL, "factory firmware under a factory key");
    CHECK(fwproduct_refusal(FWPRODUCT_FACTORY, FWCLASS_NONE) == NULL, "factory firmware unsigned");
    CHECK(refused_with(FWPRODUCT_FACTORY, FWCLASS_RELEASE, "is not ForgeFIRM firmware"), "factory firmware under the release key");
    CHECK(refused_with(FWPRODUCT_FORGEFIRM, FWCLASS_FACTORY, "is not factory firmware"), "ForgeFIRM firmware under a factory key");
    for (int cls = -1; cls <= 3; cls++)
        CHECK(fwproduct_refusal(FWPRODUCT_EXTENSION, cls) != NULL, "an extension package passed in class %d", cls);
    CHECK(refused_with(FWPRODUCT_EXTENSION, FWCLASS_RELEASE, "an extension package, not firmware")
          && refused_with(FWPRODUCT_EXTENSION, FWCLASS_NONE, "an extension package, not firmware"),
          "an extension package is named as one");
    CHECK(refused_with("", FWCLASS_NONE, "not firmware") && refused_with(NULL, FWCLASS_RELEASE, "not ForgeFIRM firmware")
          && refused_with("forgefirm firmware", FWCLASS_NONE, "not firmware")
          && refused_with("ForgeFIRM firmware ", FWCLASS_NONE, "not firmware"), "no product, or one that is nearly right");
    CHECK(refused_with(FWPRODUCT_FORGEFIRM, 7, "not firmware") && refused_with(FWPRODUCT_FORGEFIRM, -1, "not firmware"),
          "a class that does not exist");

    if (!mkdtemp(dir))
        return 2;
    stand_in();
    const char *why;
    CHECK((why = fwproduct_gate(archive("release.fw", RELEASE_M), FWCLASS_RELEASE)) == NULL,
          "a ForgeFIRM release under the release key was refused: %s", why);
    CHECK((why = fwproduct_gate(archive("release.fw", RELEASE_M), FWCLASS_NONE)) == NULL,
          "a ForgeFIRM build, unsigned, was refused: %s", why);
    CHECK((why = fwproduct_gate(archive("factory.fw", FACTORY_M), FWCLASS_FACTORY)) == NULL,
          "a factory archive under a factory key was refused: %s", why);
    why = fwproduct_gate(archive("factory.fw", FACTORY_M), FWCLASS_RELEASE);
    CHECK(why && strstr(why, "is not ForgeFIRM firmware"), "a factory archive under the release key -> %s", why ? why : "passed");
    for (int cls = FWCLASS_NONE; cls <= FWCLASS_RELEASE; cls++) {
        why = fwproduct_gate(archive("package.ffx", EXTENSION_M), cls);
        CHECK(why && strstr(why, "an extension package, not firmware"), "an extension package in class %d -> %s", cls,
              why ? why : "passed");
    }
    char gone[96];
    snprintf(gone, sizeof(gone), "%s/not-there.fw", dir);
    why = fwproduct_gate(gone, FWCLASS_NONE);
    CHECK(why && strstr(why, "not firmware"), "an archive fwup cannot read -> %s", why ? why : "passed");
    why = fwproduct_gate(archive("empty.fw", ""), FWCLASS_RELEASE);
    CHECK(why != NULL, "an archive with no metadata passed");
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0)
        fails++;

    printf("%s: fwproduct_test, %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
