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
 * controller is brought back in line after a restart either way; a claim
 * is enforced only on the running controller's word (its "sender out"
 * taken, or its state saying so), never on a controller started since; the
 * job runner's keep for a run is the controller's "sender out", refused at
 * once on a busy machine, asked again while the sender is in its quiet
 * time and refused past the wait, asked under a claim too, and
 * neither the keeper nor a claim's end lets a sender in until the run's
 * end does; and a daemon that starts takes up a claim the host still
 * keeps.
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
static int busy_sender_left;            /* that many answers busy:sender first */
static int unreachable;
static int kept_out;                    /* what the controller says in its state */
static char sent[64][32];
static int nsent;

static long ctl_pid = 100;              /* the running controller; a new one is a new pid */
int super_grbl_running(void) { return running; }
long super_grbl_pid(void) { return running ? ctl_pid : 0; }

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
    if (nsent < 64)
        snprintf(sent[nsent++], sizeof(sent[0]), "sender %s", arg);
    if (busy_sender_left > 0) {
        busy_sender_left--;
        snprintf(reply, len, "busy:sender");
        return GRBLPORT_OK;
    }
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
    setenv("FORGECTRL_HOST_TEST", "1", 1);     /* the test hooks' gate (testhook.h) */
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
    printf("A claim is trusted on the controller's own word:\n");
    CHECK(senderout_enforced(A) && !senderout_enforced(B), "its sender out taken: enforced, for the holder alone");
    CHECK(senderout_keeps_machine(A) && !senderout_keeps_machine(B), "and the holder alone keeps the machine for its own jobs");
    ctl_pid++;                          /* the controller starts again, and keeps nobody out */
    kept_out = 0;
    CHECK(!senderout_enforced(A), "a controller started since is not the one that said so");
    CHECK(!senderout_keeps_machine(A), "nor does the holder keep the machine on that controller");
    snprintf(next_reply, sizeof(next_reply), "busy:sender");
    nap(SENDEROUT_REPUSH_S + 0.1);
    keep(A, 0, 1);
    senderout_tick();
    CHECK(!senderout_enforced(A), "its state keeps nobody out and the push is refused: not enforced");
    snprintf(next_reply, sizeof(next_reply), "ok");
    nap(SENDEROUT_REPUSH_S + 0.1);
    keep(A, 0, 1);
    senderout_tick();
    CHECK(kept_out && senderout_enforced(A), "the push taken: enforced again");
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
    CHECK(lease_take("diag:aa-offset-calibrate", LEASE_HARDWARE, NULL, why, sizeof(why)) == 0, "a diagnostic holds the machine");
    before = nsent;
    CHECK(senderout_claim(A, why, sizeof(why)) == 409 && strstr(why, "a diagnostic") && nsent == before,
          "another holder of the machine refuses it before the controller is asked: %s", why);
    lease_release("diag:aa-offset-calibrate");

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

    printf("The job runner keeps the sender out for a run:\n");
    nsent = 0;
    CHECK(senderout_runner_out(why, sizeof(why)) == 0 && kept_out && !strcmp(last_sent(), "sender out"),
          "with no claim it is the controller's sender out: %s", why);
    nap(SENDEROUT_REPUSH_S + 0.1);
    senderout_tick();
    CHECK(kept_out && strcmp(last_sent(), "sender in"), "the keeper does not let senders in under the run");
    senderout_runner_in();
    CHECK(!kept_out && !strcmp(last_sent(), "sender in"), "the run's end lets them in");
    snprintf(next_reply, sizeof(next_reply), "busy:state");
    before = nsent;
    CHECK(senderout_runner_out(why, sizeof(why)) == -1 && strstr(why, "not idle") && nsent == before + 1,
          "a machine the controller calls busy (a job held, a line running) refuses at once: %s", why);
    snprintf(next_reply, sizeof(next_reply), "ok");
    busy_sender_left = 1;
    before = nsent;
    CHECK(senderout_runner_out(why, sizeof(why)) == 0 && nsent == before + 2 && kept_out,
          "a sender still in its quiet time is asked again until it has passed");
    senderout_runner_in();
    busy_sender_left = 1000;
    CHECK(senderout_runner_out(why, sizeof(why)) == -1 && strstr(why, "Grbl sender is using"),
          "and one that stays busy past the wait refuses: %s", why);
    busy_sender_left = 0;
    CHECK(senderout_claim(A, why, sizeof(why)) == 0, "under a package's claim");
    keep(A, 0, 1);
    kept_out = 0;                       /* a controller started since keeps nobody out */
    before = nsent;
    CHECK(senderout_runner_out(why, sizeof(why)) == 0 && nsent == before + 1 && kept_out &&
          !strcmp(last_sent(), "sender out"), "the run still asks the controller's sender out: %s", why);
    before = nsent;
    CHECK(senderout_end(A, why, sizeof(why)) == 0 && kept_out && nsent == before,
          "the claim's end under the run leaves the sender out");
    senderout_runner_in();
    CHECK(!kept_out && !strcmp(last_sent(), "sender in"), "until the run ends");
    unkeep(A);

    printf("A daemon that starts takes up a claim the host keeps:\n");
    keep(B, 0, 1);
    senderout_init();
    CHECK(senderout_holder(id, sizeof(id)) && !strcmp(id, B) && holds("ext:org.example.other"),
          "taken up, the machine with it");
    /* Taken up from the host's file, it is this daemon's record only: no
     * controller has said it keeps the sender out for it yet. */
    CHECK(!senderout_enforced(B), "a claim taken up at the start is not trusted before the controller's word");
    nap(SENDEROUT_REPUSH_S + 0.1);
    keep(B, 0, 1);
    senderout_tick();
    CHECK(kept_out, "and the controller told");
    CHECK(senderout_enforced(B), "trusted once the running controller has taken its push");
    senderout_end(B, why, sizeof(why));
    unkeep(B);

    rmdir(sub);
    rmdir(dir);
    printf(fails ? "FAIL: senderout_test, %d failure(s)\n" : "PASS: senderout_test\n", fails);
    return fails ? 1 : 0;
}
