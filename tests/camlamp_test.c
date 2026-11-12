/*
 * camlamp_test.c - host unit test for a camera lamp write and its result
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A lamp write reaches the attribute when the stream is flushed, at fclose:
 * a write the attribute refuses (the kernel fails a head register write
 * that never holds) must come back as that error, never as a write that
 * went through. /dev/full refuses every write at the flush.
 */
#define _GNU_SOURCE
#include "../src/camlamp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    char path[] = "/tmp/camlamp_test.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(fd);

    printf("A. a write that lands is 0, and the attribute holds the level\n");
    char buf[16] = "";
    int e = camlamp_write(path, 132);
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(buf, sizeof(buf), f))
            buf[0] = '\0';
        fclose(f);
    }
    CHECK(e == 0 && !strcmp(buf, "132"), "0, and it reads 132");
    unlink(path);

    printf("B. a write refused at the flush comes back as its error\n");
    CHECK(camlamp_write("/dev/full", 0) == ENOSPC, "/dev/full: ENOSPC");

    printf("C. a lamp with no attribute is ENOENT\n");
    CHECK(camlamp_write("/nonexistent-camlamp/white_led", 0) == ENOENT, "ENOENT");

    if (failures) {
        printf("camlamp_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("camlamp_test: all passed\n");
    return 0;
}
