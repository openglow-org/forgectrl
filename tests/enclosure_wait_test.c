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
 * "open", never on "closed". The switch word from a file, the host
 * start's seam GF_SWITCH_WORD_FILE, is read by an absolute path only:
 * the word the file holds classifies as the device's would, and a
 * relative path is a failed read, so "open".
 *
 * The test drives liveness_enclosure() from liveness.c through the
 * GF_SWITCH_DEV and GF_SWITCH_WORD_FILE seams.
 */
#define _GNU_SOURCE
#include "../src/liveness.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    setenv("FORGECTRL_HOST_TEST", "1", 1);     /* the test hooks' gate (testhook.h) */
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

    printf("the switch word from a file (GF_SWITCH_WORD_FILE)\n");
    char wf[] = "/tmp/enclosure-wait-XXXXXX";
    int fd = mkstemp(wf);
    CHECK(fd >= 0 && write(fd, "8\n", 2) == 2, "a word file can be made");
    close(fd);
    setenv("GF_SWITCH_WORD_FILE", wf, 1);
    CHECK(liveness_enclosure(why, sizeof(why)) == 0, "8 (doors closed, loop closed): closed, the device unread");
    fd = open(wf, O_WRONLY | O_TRUNC); (void)!write(fd, "0x28\n", 5); close(fd);
    CHECK(liveness_enclosure(why, sizeof(why)) == 1 && !strcmp(why, "the interlock is open"),
          "0x28: the interlock is open");
    fd = open(wf, O_WRONLY | O_TRUNC); (void)!write(fd, "0\n", 2); close(fd);
    CHECK(liveness_enclosure(why, sizeof(why)) == 1 && !strcmp(why, "the lid is open"), "0: the lid is open");
    fd = open(wf, O_WRONLY | O_TRUNC); (void)!write(fd, "eight\n", 6); close(fd);
    CHECK(liveness_enclosure(why, sizeof(why)) == -1 && strstr(why, "cannot be read"),
          "a word that is no integer reads as open");
    FILE *rel = fopen("enclosure-wait-word", "w");
    if (rel) {
        fputs("8\n", rel);
        fclose(rel);
    }
    setenv("GF_SWITCH_WORD_FILE", "enclosure-wait-word", 1);
    CHECK(liveness_enclosure(why, sizeof(why)) == -1 && strstr(why, "cannot be read"),
          "the same word by a relative path is refused by its form: open");
    unlink("enclosure-wait-word");
    unlink(wf);
    unsetenv("GF_SWITCH_WORD_FILE");

    printf(failures ? "enclosure_wait_test: %d FAILED\n" : "enclosure_wait_test: all ok\n", failures);
    return failures ? 1 : 0;
}
