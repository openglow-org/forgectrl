/*
 * senderout_test.c - host test for an extension that keeps the Grbl sender out
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The daemon's side of keeping the sender out (senderout.h), over a
 * scripted controller port and the real lease: a claim is the controller's
 * "sender out" and the machine lease as ext:<id>, granted to one package
 * at a time and refused in words when the controller says the machine or
 * the sender is busy (the lease then left as it was); the package's end,
 * and the operator's, are "sender in" with the lease given up; a claim the
 * host stops keeping fresh ends by itself with a notice, and moves
 * nothing; the operator's end refuses the package until it says in; the
 * controller is brought back in line after a restart either way; and a
 * daemon that starts takes up a claim the host still keeps.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../src/grblport.h"
#include "../src/lease.h"
#include "../src/senderout.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
                              else { printf("  ok: " __VA_ARGS__); printf("\n"); } } while (0)

/* --- the controller, scripted ------------------------------------------ */
static int running = 1;
static char next_reply[64] = "ok";      /* the answer to "sender ..." */
static int unreachable;
static int kept_out;                    /* what the controller says in its state */
static char sent[16][32];
static int nsent;

int super_grbl_running(void) { return running; }

int grblport_request(grblport_set_t set, grblport_op_t op, const char *arg, char *reply, size_t len)
{
    if (unreachable)
        return GRBLPORT_UNREACHABLE;
    if (op == GRBLPORT_STATE) {
        snprintf(reply, len, "{\"state\":\"Idle\",\"sender_out\":%s}", kept_out ? "true" : "false");
        return GRBLPORT_OK;
    }
    if (op != GRBLPORT_SENDER || set != GRBLPORT_SET_DAEMON)
        return GRBLPORT_FORBIDDEN;
    if (nsent < 16)
        snprintf(sent[nsent++], sizeof(sent[0]), "sender %s", arg);
    snprintf(reply, len, "%s", next_reply);
    if (!strcmp(next_reply, "ok"))
        kept_out = !strcmp(arg, "out");
    return GRBLPORT_OK;
}

static const char *last_sent(void) { return nsent ? sent[nsent - 1] : ""; }

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static char dir[128];

/* The host's claim file for a package: written age seconds ago, or gone. */
static void keep(const char *id, double age, int out)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/sender-out/%s.json", dir, id);
    FILE *f = fopen(path, "w");
    fprintf(f, "{\"id\":\"%s\",\"required\":false,\"raised\":%s,\"reason\":\"keeps the Grbl sender out\","
               "\"ts_mono\":%.3f}\n", id, out ? "true" : "false", mono() - age);
    fclose(f);
}

static void unkeep(const char *id)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/sender-out/%s.json", dir, id);
    unlink(path);
}

static void nap(double s)
{
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    nanosleep(&ts, NULL);
}

static int holds(const char *owner)
{
    char h[LEASE_OWNER_MAX] = "";
    return lease_holder(h, sizeof(h)) && !strcmp(h, owner);
}

int main(void)
{
    char why[256], doc[512], id[80];
    const char *A = "org.openglow.alignment", *B = "org.example.other";

    snprintf(dir, sizeof(dir), "/tmp/senderout-test.XXXXXX");
    if (!mkdtemp(dir))
        return 2;
    char sub[256];
    snprintf(sub, sizeof(sub), "%s/sender-out", dir);
    mkdir(sub, 0755);
    setenv("GF_RUN_DIR", dir, 1);
    senderout_init();

    printf("A package id:\n");
    CHECK(senderout_id_ok(A) && senderout_id_ok("a") && senderout_id_ok("org.example.a-b-9"), "ids the host writes");
    CHECK(!senderout_id_ok("") && !senderout_id_ok(NULL) && !senderout_id_ok("Org.x") && !senderout_id_ok("9x") &&
          !senderout_id_ok("a/b") && !senderout_id_ok("a\"b") &&
          !senderout_id_ok("a123456789012345678901234567890123456789012345678901234567890123"),
          "and none else");
    CHECK(senderout_claim("../x", why, sizeof(why)) == 400, "a claim for no id: 400");

    printf("A claim:\n");
    CHECK(senderout_claim(A, why, sizeof(why)) == 0, "granted: %s", why);
    CHECK(!strcmp(last_sent(), "sender out") && kept_out, "the controller keeps the sender out");
    CHECK(holds("ext:org.openglow.alignment"), "the package holds the machine");
    CHECK(senderout_holder(id, sizeof(id)) && !strcmp(id, A), "and is the holder");
    CHECK(senderout_json(doc, sizeof(doc), 1) > 0 &&
          strstr(doc, "\"sender_out\":{\"holder\":{\"id\":\"org.openglow.alignment\"") && strstr(doc, "\"notice\":null"),
          "the status says so: %s", doc);
    CHECK(senderout_pkg_json(A, doc, sizeof(doc)) > 0 && !strcmp(doc, "{\"out\":true,\"released\":false}"),
          "and the package's own: %s", doc);
    CHECK(senderout_pkg_json(B, doc, sizeof(doc)) > 0 && !strcmp(doc, "{\"out\":false,\"released\":false}"),
          "another package's: %s", doc);
    int before = nsent;
    CHECK(senderout_claim(A, why, sizeof(why)) == 0 && nsent == before, "claimed again, nothing changes");
    CHECK(senderout_claim(B, why, sizeof(why)) == 409 && strstr(why, "another extension (org.openglow.alignment)"),
          "a second package is refused: %s", why);
    CHECK(senderout_end(B, why, sizeof(why)) == 0 && kept_out && holds("ext:org.openglow.alignment"),
          "and its end changes nothing");
    CHECK(senderout_end(A, why, sizeof(why)) == 0, "the package's own end");
    CHECK(!strcmp(last_sent(), "sender in") && !kept_out && lease_holder(id, sizeof(id)) == 0,
          "lets the sender in and gives up the machine");

    printf("Refusals:\n");
    snprintf(next_reply, sizeof(next_reply), "busy:state");
    CHECK(senderout_claim(A, why, sizeof(why)) == 409 && strstr(why, "not idle"), "a busy machine: %s", why);
    CHECK(lease_holder(id, sizeof(id)) == 0 && !senderout_holder(id, sizeof(id)), "and nothing is held");
    snprintf(next_reply, sizeof(next_reply), "busy:sender");
    CHECK(senderout_claim(A, why, sizeof(why)) == 409 && strstr(why, "Grbl sender is using"), "a busy sender: %s", why);
    snprintf(next_reply, sizeof(next_reply), "ok");
    unreachable = 1;
    CHECK(senderout_claim(A, why, sizeof(why)) == 503, "a controller that does not answer: 503");
    unreachable = 0;
    running = 0;
    CHECK(senderout_claim(A, why, sizeof(why)) == 409 && strstr(why, "not running"), "no controller: %s", why);
    running = 1;
    CHECK(lease_take("diag:flow-verify", LEASE_HARDWARE, NULL, why, sizeof(why)) == 0, "a diagnostic holds the machine");
    before = nsent;
    CHECK(senderout_claim(A, why, sizeof(why)) == 409 && strstr(why, "a diagnostic") && nsent == before,
          "another holder of the machine refuses it before the controller is asked: %s", why);
    lease_release("diag:flow-verify");

    printf("A claim its keeper stops keeping:\n");
    CHECK(senderout_claim(A, why, sizeof(why)) == 0, "granted");
    senderout_tick();
    CHECK(kept_out && senderout_holder(id, sizeof(id)), "inside the grace, with no file yet, it stands");
    keep(A, 0, 1);
    nap(SENDEROUT_GRACE_S + 0.1);
    keep(A, 0, 1);
    senderout_tick();
    CHECK(kept_out && senderout_holder(id, sizeof(id)), "kept fresh, it stands");
    keep(A, SENDEROUT_FRESH_S + 1, 1);
    senderout_tick();
    CHECK(!kept_out && !senderout_holder(id, sizeof(id)) && lease_holder(id, sizeof(id)) == 0,
          "stale, it ends: the sender is let in and the machine given up");
    CHECK(senderout_json(doc, sizeof(doc), 0) > 0 &&
          strstr(doc, "\"notice\":{\"id\":\"org.openglow.alignment\",\"why\":\"stopped\"}"),
          "and the notice names it: %s", doc);
    senderout_notice_clear();
    CHECK(senderout_json(doc, sizeof(doc), 0) > 0 && strstr(doc, "\"notice\":null"), "the operator clears it");
    CHECK(senderout_claim(A, why, sizeof(why)) == 0, "a new claim");
    keep(A, 0, 0);
    nap(SENDEROUT_GRACE_S + 0.1);
    keep(A, 0, 0);
    senderout_tick();
    CHECK(!kept_out && !senderout_holder(id, sizeof(id)), "a fresh file that says in ends it too");
    unkeep(A);

    printf("The operator lets the sender back in:\n");
    CHECK(senderout_claim(A, why, sizeof(why)) == 0, "granted");
    keep(A, 0, 1);
    CHECK(senderout_release(why, sizeof(why)) == 0 && !kept_out && lease_holder(id, sizeof(id)) == 0,
          "released: the sender is let in and the machine given up");
    CHECK(senderout_pkg_json(A, doc, sizeof(doc)) > 0 && !strcmp(doc, "{\"out\":false,\"released\":true}"),
          "the package learns it: %s", doc);
    senderout_tick();
    CHECK(senderout_claim(A, why, sizeof(why)) == 409 && strstr(why, "operator let"),
          "and its claim is refused while it still keeps it: %s", why);
    CHECK(senderout_end(A, why, sizeof(why)) == 0 &&
          senderout_pkg_json(A, doc, sizeof(doc)) > 0 && !strcmp(doc, "{\"out\":false,\"released\":false}"),
          "until the package itself says in");
    CHECK(senderout_claim(A, why, sizeof(why)) == 0, "then it may claim again");
    CHECK(senderout_release(why, sizeof(why)) == 0, "released again");
    keep(A, SENDEROUT_FRESH_S + 1, 1);
    senderout_tick();
    CHECK(senderout_pkg_json(A, doc, sizeof(doc)) > 0 && !strcmp(doc, "{\"out\":false,\"released\":false}"),
          "a package that stops claiming is forgiven");
    unkeep(A);

    printf("The controller is brought back in line:\n");
    CHECK(senderout_claim(A, why, sizeof(why)) == 0, "granted");
    keep(A, 0, 1);
    kept_out = 0;                       /* the controller restarted */
    nap(SENDEROUT_REPUSH_S + 0.1);
    keep(A, 0, 1);
    senderout_tick();
    CHECK(kept_out && !strcmp(last_sent(), "sender out"), "a restarted controller is told to keep the sender out");
    CHECK(senderout_end(A, why, sizeof(why)) == 0 && !kept_out, "ended");
    unkeep(A);
    kept_out = 1;                       /* one a restarted daemon left behind */
    nap(SENDEROUT_REPUSH_S + 0.1);
    senderout_tick();
    CHECK(!kept_out && !strcmp(last_sent(), "sender in"), "a controller that keeps the sender out for nobody lets it in");

    printf("A daemon that starts takes up a claim the host keeps:\n");
    keep(B, 0, 1);
    senderout_init();
    CHECK(senderout_holder(id, sizeof(id)) && !strcmp(id, B) && holds("ext:org.example.other"),
          "taken up, the machine with it");
    nap(SENDEROUT_REPUSH_S + 0.1);
    keep(B, 0, 1);
    senderout_tick();
    CHECK(kept_out, "and the controller told");
    senderout_end(B, why, sizeof(why));
    unkeep(B);

    rmdir(sub);
    rmdir(dir);
    printf(fails ? "FAIL: senderout_test, %d failure(s)\n" : "PASS: senderout_test\n", fails);
    return fails ? 1 : 0;
}
