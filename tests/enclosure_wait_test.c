/*
 * enclosure_wait_test.c - host unit test for the motion gate's enclosure read
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The motion-liveness probe moves the gantry, so the supervisor's gate
 * runs it only with the lid and the interlock closed, and waits for them
 * otherwise (no controller, /mode "waiting", the button amber). What is
 * provable on a host: the EV_SW word maps to the right verdict and the
 * right words for the operator, and every way the switch read can fail
 * (a missing device, a path that is not an input device) lands on
 * "open", never on "closed".
 *
 * The test drives liveness_enclosure() from liveness.c through the
 * GF_SWITCH_DEV seam.
 */
#define _GNU_SOURCE
#include "../src/liveness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* EV_SW bits: 3 = doors (set = closed), 5 = interlock (set = open). */
#define DOORS_CLOSED   (1u << 3)
#define INTERLOCK_OPEN (1u << 5)

int main(void)
{
    char why[96];

    printf("the classification of the switch word\n");
    memset(why, 'x', sizeof(why));
    CHECK(liveness_enclosure_classify(DOORS_CLOSED, why, sizeof(why)) == 0 && why[0] == '\0',
          "doors closed, interlock satisfied: closed, no words");
    CHECK(liveness_enclosure_classify(0, why, sizeof(why)) == 1 && !strcmp(why, "the lid is open"),
          "doors open: the lid is open");
    CHECK(liveness_enclosure_classify(DOORS_CLOSED | INTERLOCK_OPEN, why, sizeof(why)) == 1
          && !strcmp(why, "the interlock is open"),
          "doors closed, interlock open: the interlock is open");
    CHECK(liveness_enclosure_classify(INTERLOCK_OPEN, why, sizeof(why)) == 1
          && !strcmp(why, "the lid and the interlock are open"),
          "both open: named together");
    CHECK(liveness_enclosure_classify(0, why, 1) == 1 && why[0] == '\0',
          "a one-byte why buffer is terminated");

    printf("the read fails open\n");
    setenv("GF_SWITCH_DEV", "/nonexistent/input/event0", 1);
    CHECK(liveness_enclosure(why, sizeof(why)) == -1 && strstr(why, "cannot be read"),
          "a missing switch device reads as open, with the reason");
    setenv("GF_SWITCH_DEV", "/dev/null", 1);
    CHECK(liveness_enclosure(why, sizeof(why)) == -1 && strstr(why, "cannot be read"),
          "a path that is not an input device reads as open");

    printf(failures ? "enclosure_wait_test: %d FAILED\n" : "enclosure_wait_test: all ok\n", failures);
    return failures ? 1 : 0;
}
