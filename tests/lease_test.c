/*
 * lease_test.c - host test for the machine lease
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * One holder at a time, and the refusal names it; one owner let in under
 * a holder, and only under the holder it names; release by the innermost
 * owner alone; a sender hold refused while a client is connected, and no
 * other kind; the controls locked by every holder but a log export; the
 * /status document with no holder, a holder, a nested one, and what is
 * observed outside the lease; the words for each kind of owner; and
 * sixteen threads asking at once, of which one wins.
 */
#include "../src/lease.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int client, released;
static int fake_client(void) { return client; }
static int fake_released(void) { return released; }

static pthread_barrier_t gate;
static int winners;
static pthread_mutex_t wmu = PTHREAD_MUTEX_INITIALIZER;

static void *racer(void *arg)
{
    char owner[32], why[96];
    snprintf(owner, sizeof(owner), "diag:racer-%ld", (long)(size_t)arg);
    pthread_barrier_wait(&gate);
    if (lease_take(owner, LEASE_HARDWARE, NULL, why, sizeof(why)) == 0) {
        pthread_mutex_lock(&wmu);
        winners++;
        pthread_mutex_unlock(&wmu);
    }
    return NULL;
}

int main(void)
{
    char why[128], who[LEASE_OWNER_MAX], doc[512];

    lease_sender_connected = fake_client;
    lease_motors_released = fake_released;

    /* Nobody. */
    CHECK(lease_refusal(why, sizeof(why)) == 0, "a refusal with no holder");
    CHECK(lease_holder(who, sizeof(who)) == 0, "a holder with no holder");
    CHECK(lease_json(doc, sizeof(doc)) > 0 &&
          !strcmp(doc, "\"lease\":{\"holder\":null,\"observed\":{\"sender\":false,\"motors_released\":false}}"),
          "the empty document: %s", doc);

    /* One holder, and the refusal names it. */
    CHECK(lease_take("diag:flow-verify", LEASE_HARDWARE, NULL, why, sizeof(why)) == 0, "the first take: %s", why);
    CHECK(lease_take("update:apply", LEASE_SYSTEM, NULL, why, sizeof(why)) != 0 &&
          !strcmp(why, "a diagnostic (flow-verify) holds the machine"), "a second owner: '%s'", why);
    CHECK(lease_take("diag:flow-verify", LEASE_HARDWARE, NULL, why, sizeof(why)) != 0, "the holder taking it twice");
    CHECK(lease_refusal(why, sizeof(why)) == 1 && strstr(why, "flow-verify"), "the route's refusal: '%s'", why);
    CHECK(lease_holder(who, sizeof(who)) == 1 && !strcmp(who, "diag:flow-verify"), "the holder: '%s'", who);
    lease_release("update:apply");
    CHECK(lease_holder(who, sizeof(who)) == 1, "somebody else's release took the lease away");
    lease_release("diag:flow-verify");
    CHECK(lease_holder(who, sizeof(who)) == 0, "the holder's release did not");

    /* Under a holder: only under the one named, one deep, inner first out. */
    CHECK(lease_take("diag:flow-calibrate", LEASE_HARDWARE, "wizard:cooling.flow", why, sizeof(why)) != 0 &&
          strstr(why, "does not hold"), "under a holder that is not there: '%s'", why);
    CHECK(lease_take("wizard:cooling.flow", LEASE_HARDWARE, NULL, why, sizeof(why)) == 0, "the wizard: %s", why);
    CHECK(lease_take("diag:flow-calibrate", LEASE_HARDWARE, "wizard:motion", why, sizeof(why)) != 0 &&
          strstr(why, "a setup wizard (cooling.flow) holds"), "under the wrong holder: '%s'", why);
    CHECK(lease_take("diag:flow-calibrate", LEASE_HARDWARE, NULL, why, sizeof(why)) != 0, "inside without saying so");
    CHECK(lease_take("diag:flow-calibrate", LEASE_HARDWARE, "wizard:cooling.flow", why, sizeof(why)) == 0,
          "the diagnostic under its wizard: %s", why);
    CHECK(lease_take("recorder", LEASE_SENDER, "diag:flow-calibrate", why, sizeof(why)) != 0 &&
          strstr(why, "a diagnostic (flow-calibrate) holds"), "a third level: '%s'", why);
    CHECK(lease_json(doc, sizeof(doc)) > 0 && strstr(doc, "\"owner\":\"diag:flow-calibrate\"") &&
          strstr(doc, "\"kind\":\"hardware\"") && strstr(doc, "\"under\":\"wizard:cooling.flow\"") &&
          strstr(doc, "\"words\":\"a diagnostic (flow-calibrate)\""), "the nested document: %s", doc);
    lease_release("wizard:cooling.flow");
    CHECK(lease_holder(who, sizeof(who)) == 1 && !strcmp(who, "diag:flow-calibrate"),
          "the outer owner released from under the inner one: '%s'", who);
    lease_release("diag:flow-calibrate");
    CHECK(lease_holder(who, sizeof(who)) == 1 && !strcmp(who, "wizard:cooling.flow"), "after the inner release: '%s'", who);
    CHECK(lease_json(doc, sizeof(doc)) > 0 && !strstr(doc, "\"under\""), "the document after it: %s", doc);
    lease_release("wizard:cooling.flow");
    CHECK(lease_holder(who, sizeof(who)) == 0, "not empty at the end of the nesting");

    /* A sender hold and a connected client. */
    client = 1;
    CHECK(lease_take("recorder", LEASE_SENDER, NULL, why, sizeof(why)) != 0 && strstr(why, "a sender is connected"),
          "a sender hold beside a client: '%s'", why);
    CHECK(lease_take("diag:flow-verify", LEASE_HARDWARE, NULL, why, sizeof(why)) == 0,
          "a hardware hold was refused for the client: %s", why);
    released = 1;
    CHECK(lease_json(doc, sizeof(doc)) > 0 && strstr(doc, "\"observed\":{\"sender\":true,\"motors_released\":true}"),
          "what is observed: %s", doc);
    lease_release("diag:flow-verify");
    client = released = 0;
    CHECK(lease_take("recorder", LEASE_SENDER, NULL, why, sizeof(why)) == 0, "the sender hold with no client: %s", why);
    CHECK(lease_json(doc, sizeof(doc)) > 0 && strstr(doc, "\"kind\":\"sender\""), "its kind: %s", doc);
    lease_release("recorder");

    /* What locks the controls: every holder but a log export. */
    CHECK(lease_refusal_locks(why, sizeof(why)) == 0, "the controls are locked with no holder");
    CHECK(lease_take("update:download", LEASE_SYSTEM, NULL, why, sizeof(why)) == 0, "the update job: %s", why);
    CHECK(lease_refusal_locks(why, sizeof(why)) == 1 && !strcmp(why, "an update job (download) holds the machine"),
          "an update job does not lock the controls: '%s'", why);
    CHECK(lease_json(doc, sizeof(doc)) > 0 && strstr(doc, "\"kind\":\"system\""), "its kind: %s", doc);
    lease_release("update:download");
    CHECK(lease_take("logs.export", LEASE_EXPORT, NULL, why, sizeof(why)) == 0, "the export: %s", why);
    CHECK(lease_refusal_locks(why, sizeof(why)) == 0, "a log export locks the controls");
    CHECK(lease_refusal(why, sizeof(why)) == 1 && !strcmp(why, "a log export holds the machine"),
          "but it holds the machine against everything else: '%s'", why);
    CHECK(lease_json(doc, sizeof(doc)) > 0 && strstr(doc, "\"kind\":\"export\""), "its kind: %s", doc);
    lease_release("logs.export");

    /* An extension that keeps the sender out locks the controls, and its
     * own requests, and a job run under it, are its own. */
    CHECK(lease_take("ext:org.openglow.alignment", LEASE_EXTENSION, NULL, why, sizeof(why)) == 0,
          "the extension's hold: %s", why);
    CHECK(lease_json(doc, sizeof(doc)) > 0 && strstr(doc, "\"kind\":\"extension\""), "its kind: %s", doc);
    CHECK(lease_refusal_locks(why, sizeof(why)) == 1 &&
          !strcmp(why, "an extension (org.openglow.alignment) holds the machine"),
          "it locks the controls: '%s'", why);
    CHECK(lease_refusal_locks_for("ext:org.openglow.alignment", why, sizeof(why)) == 0,
          "but not its own requests");
    CHECK(lease_refusal_locks_for("ext:org.example.other", why, sizeof(why)) == 1,
          "and every other extension's");
    CHECK(lease_refusal_locks_for(NULL, why, sizeof(why)) == 1, "and every caller that names nobody");
    CHECK(lease_take("job:org.openglow.alignment", LEASE_SENDER, "ext:org.openglow.alignment", why, sizeof(why)) == 0,
          "its job runs under it: %s", why);
    CHECK(lease_refusal_locks_for("ext:org.openglow.alignment", why, sizeof(why)) == 0,
          "and it is still its own under the job");
    lease_release("job:org.openglow.alignment");
    lease_release("ext:org.openglow.alignment");
    lease_take("logs.export", LEASE_EXPORT, NULL, why, sizeof(why));
    CHECK(lease_refusal_locks_for("ext:org.example.other", why, sizeof(why)) == 0,
          "a log export locks nobody's controls");
    lease_release("logs.export");

    /* Names that cannot be held. */
    CHECK(lease_take("", LEASE_SYSTEM, NULL, why, sizeof(why)) != 0, "an empty owner");
    CHECK(lease_take("a\"b", LEASE_SYSTEM, NULL, why, sizeof(why)) != 0, "an owner with a quote in it");
    CHECK(lease_take("ext:01234567890123456789012345678901234567890123456789012345678901234567", LEASE_SYSTEM,
                     NULL, why, sizeof(why)) != 0, "an owner too long to hold");
    CHECK(lease_take("ext:org.example.a-package-id-of-sixty-three-characters-at-the-bound", LEASE_EXTENSION,
                     NULL, why, sizeof(why)) == 0, "an extension's owner with the longest package id: %s", why);
    lease_release("ext:org.example.a-package-id-of-sixty-three-characters-at-the-bound");
    CHECK(lease_holder(who, sizeof(who)) == 0, "a refused name left a holder: '%s'", who);

    /* The words. */
    static const char *const pairs[][2] = {
        { "diag:aa-offset-calibrate", "a diagnostic (aa-offset-calibrate)" },
        { "wizard:motion", "a setup wizard (motion)" },
        { "update:download", "an update job (download)" },
        { "ext:alignment", "an extension (alignment)" },
        { "recorder", "the dose-curve recorder" },
        { "logs.export", "a log export" },
        { "something", "something" },
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        lease_words(pairs[i][0], why, sizeof(why));
        CHECK(!strcmp(why, pairs[i][1]), "the words for %s: '%s'", pairs[i][0], why);
    }

    /* Sixteen at once, fifty times: one winner each time. */
    for (int round = 0; round < 50; round++) {
        pthread_t th[16];
        winners = 0;
        pthread_barrier_init(&gate, NULL, 16);
        for (size_t i = 0; i < 16; i++)
            pthread_create(&th[i], NULL, racer, (void *)i);
        for (size_t i = 0; i < 16; i++)
            pthread_join(th[i], NULL);
        pthread_barrier_destroy(&gate);
        CHECK(winners == 1, "round %d: %d winners", round, winners);
        CHECK(lease_holder(who, sizeof(who)) == 1, "round %d: nobody holds it", round);
        lease_release(who);
    }

    printf(fails ? "lease_test: %d FAILED\n" : "lease_test: all passed\n", fails);
    return fails ? 1 : 0;
}
