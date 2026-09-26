/*
 * senderout.c - an extension package keeps the Grbl sender out
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See senderout.h. One mutex holds the state; the port and the lease are
 * asked outside it, so a slow controller never stalls a status read.
 */
#include "senderout.h"

#include <ctype.h>
#include <dirent.h>
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fflog.h"
#include "grblport.h"
#include "lease.h"
#include "paths.h"
#include "super.h"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char holder[SENDEROUT_ID_MAX + 1];      /* "" when nobody keeps the sender out */
static double since, claimed_at;
static char released[SENDEROUT_ID_MAX + 1];    /* the claim the operator ended, until the package says in */
static char notice_id[SENDEROUT_ID_MAX + 1];   /* one that stopped while it kept the sender out */
static double pushed_at;

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int senderout_id_ok(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n == 0 || n > SENDEROUT_ID_MAX || !islower((unsigned char)id[0]))
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!(islower((unsigned char)id[i]) || isdigit((unsigned char)id[i]) || id[i] == '.' || id[i] == '-'))
            return 0;
    return 1;
}

static void owner_of(const char *id, char *buf, size_t len)
{
    snprintf(buf, len, "ext:%s", id);
}

/* The controller's own word: 0 ok, else the HTTP status with why. */
static int port_sender(const char *arg, char *why, size_t len)
{
    char reply[160];
    if (!super_grbl_running()) {
        snprintf(why, len, "the GRBL controller is not running");
        return 409;
    }
    if (grblport_request(GRBLPORT_SET_DAEMON, GRBLPORT_SENDER, arg, reply, sizeof(reply)) != GRBLPORT_OK) {
        snprintf(why, len, "the GRBL controller did not answer");
        return 503;
    }
    if (!strcmp(reply, "ok"))
        return 0;
    if (!strcmp(reply, "busy:state"))
        snprintf(why, len, "the machine is not idle: a program, a jog, a hold, an armed laser, or an alarm");
    else if (!strcmp(reply, "busy:sender"))
        snprintf(why, len, "the Grbl sender is using the machine: wait until it is done");
    else
        snprintf(why, len, "the GRBL controller refused (%.32s)", reply);
    return 409;
}

/* Let every sender in, and give up the machine. Called outside mu. */
static void let_in(const char *id)
{
    char why[160], owner[LEASE_OWNER_MAX];
    if (port_sender("in", why, sizeof(why)) != 0)
        fflog(LOG_WARNING, "senderout: the controller was not told to let senders in (%s); "
                           "the keeper tries again", why);
    owner_of(id, owner, sizeof(owner));
    lease_release(owner);
}

int senderout_claim(const char *id, char *why, size_t len)
{
    char owner[LEASE_OWNER_MAX], lw[160];
    if (!senderout_id_ok(id)) {
        snprintf(why, len, "that is not a package id");
        return 400;
    }
    pthread_mutex_lock(&mu);
    if (holder[0] && strcmp(holder, id)) {
        snprintf(why, len, "another extension (%s) keeps the Grbl sender out", holder);
        pthread_mutex_unlock(&mu);
        return 409;
    }
    if (!strcmp(holder, id)) {
        claimed_at = mono();
        pthread_mutex_unlock(&mu);
        return 0;
    }
    if (!strcmp(released, id)) {
        snprintf(why, len, "the operator let the Grbl sender back in: end this use of the machine first");
        pthread_mutex_unlock(&mu);
        return 409;
    }
    pthread_mutex_unlock(&mu);

    owner_of(id, owner, sizeof(owner));
    if (lease_take(owner, LEASE_EXTENSION, NULL, lw, sizeof(lw)) != 0) {
        snprintf(why, len, "%s", lw);
        return 409;
    }
    int rc = port_sender("out", why, len);
    if (rc) {
        lease_release(owner);
        return rc;
    }
    pthread_mutex_lock(&mu);
    snprintf(holder, sizeof(holder), "%s", id);
    since = claimed_at = pushed_at = mono();
    notice_id[0] = '\0';
    pthread_mutex_unlock(&mu);
    fflog(LOG_NOTICE, "senderout: %s keeps the Grbl sender out", id);
    return 0;
}

int senderout_end(const char *id, char *why, size_t len)
{
    if (!senderout_id_ok(id)) {
        snprintf(why, len, "that is not a package id");
        return 400;
    }
    pthread_mutex_lock(&mu);
    if (!strcmp(released, id))
        released[0] = '\0';
    int was = !strcmp(holder, id);
    if (was)
        holder[0] = '\0';
    pthread_mutex_unlock(&mu);
    if (was) {
        let_in(id);
        fflog(LOG_NOTICE, "senderout: %s lets the Grbl sender back in", id);
    }
    return 0;
}

int senderout_release(char *why, size_t len)
{
    char id[SENDEROUT_ID_MAX + 1];
    (void)why;
    (void)len;
    pthread_mutex_lock(&mu);
    snprintf(id, sizeof(id), "%s", holder);
    if (id[0]) {
        snprintf(released, sizeof(released), "%s", id);
        holder[0] = '\0';
    }
    pthread_mutex_unlock(&mu);
    if (id[0]) {
        let_in(id);
        fflog(LOG_NOTICE, "senderout: the operator let the Grbl sender back in (%s kept it out)", id);
    }
    return 0;
}

void senderout_notice_clear(void)
{
    pthread_mutex_lock(&mu);
    notice_id[0] = '\0';
    pthread_mutex_unlock(&mu);
}

int senderout_json(char *buf, size_t len, int with_key)
{
    char h[160] = "null", n[160] = "null";
    pthread_mutex_lock(&mu);
    if (holder[0])
        snprintf(h, sizeof(h), "{\"id\":\"%s\",\"for_s\":%.0f}", holder, mono() - since);
    if (notice_id[0])
        snprintf(n, sizeof(n), "{\"id\":\"%s\",\"why\":\"stopped\"}", notice_id);
    pthread_mutex_unlock(&mu);
    int w = snprintf(buf, len, "%s{\"holder\":%s,\"notice\":%s}", with_key ? "\"sender_out\":" : "", h, n);
    return w > 0 && (size_t)w < len ? w : -1;
}

int senderout_pkg_json(const char *id, char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    int out = id && holder[0] && !strcmp(holder, id);
    int rel = id && released[0] && !strcmp(released, id);
    pthread_mutex_unlock(&mu);
    int w = snprintf(buf, len, "{\"out\":%s,\"released\":%s}", out ? "true" : "false", rel ? "true" : "false");
    return w > 0 && (size_t)w < len ? w : -1;
}

int senderout_holder(char *id, size_t len)
{
    pthread_mutex_lock(&mu);
    snprintf(id, len, "%s", holder);
    int is = holder[0] != '\0';
    pthread_mutex_unlock(&mu);
    return is;
}

/* The monotonic time the host last wrote a package's claim, and whether
 * it says out; 0 when there is no such file or it cannot be read. The
 * file is in the holds' own form (forgeext's holdkeep), and raised is the
 * claim. */
static double claim_written(const char *id, int *out)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/sender-out/%s.json", ff_run_dir(), id);
    json_t *j = json_load_file(path, 0, NULL);
    double ts = 0;
    *out = 0;
    if (j) {
        const char *jid = json_string_value(json_object_get(j, "id"));
        if (jid && !strcmp(jid, id)) {
            ts = json_number_value(json_object_get(j, "ts_mono"));
            *out = json_is_true(json_object_get(j, "raised"));
        }
        json_decref(j);
    }
    return ts;
}

static int claim_fresh(const char *id)
{
    int out;
    double ts = claim_written(id, &out);
    return out && ts > 0 && mono() - ts <= SENDEROUT_FRESH_S;
}

void senderout_tick(void)
{
    char id[SENDEROUT_ID_MAX + 1], rel[SENDEROUT_ID_MAX + 1], why[160], reply[512];
    pthread_mutex_lock(&mu);
    snprintf(id, sizeof(id), "%s", holder);
    snprintf(rel, sizeof(rel), "%s", released);
    double claimed = claimed_at;
    int push_due = mono() - pushed_at >= SENDEROUT_REPUSH_S;
    if (push_due)
        pushed_at = mono();
    pthread_mutex_unlock(&mu);

    /* A claim its keeper stopped keeping ends, and says so. */
    if (id[0] && mono() - claimed > SENDEROUT_GRACE_S && !claim_fresh(id)) {
        pthread_mutex_lock(&mu);
        int still = !strcmp(holder, id);
        if (still) {
            holder[0] = '\0';
            snprintf(notice_id, sizeof(notice_id), "%s", id);
        }
        pthread_mutex_unlock(&mu);
        if (still) {
            let_in(id);
            fflog(LOG_WARNING, "senderout: %s stopped while it kept the Grbl sender out; senders are "
                               "let back in, and the head was not moved", id);
        }
        id[0] = '\0';
    }
    /* The operator's refusal lasts until the package stops claiming. */
    if (rel[0] && !claim_fresh(rel)) {
        pthread_mutex_lock(&mu);
        if (!strcmp(released, rel))
            released[0] = '\0';
        pthread_mutex_unlock(&mu);
    }
    /* The controller agrees with the claim: a restarted one lets senders
     * in, and one a restarted daemon left keeping them out is told. */
    if (!push_due || !super_grbl_running())
        return;
    if (grblport_request(GRBLPORT_SET_DAEMON, GRBLPORT_STATE, NULL, reply, sizeof(reply)) != GRBLPORT_OK)
        return;
    json_t *st = json_loads(reply, 0, NULL);
    int out = json_is_true(json_object_get(st, "sender_out"));
    json_decref(st);
    if (id[0] && !out) {
        if (port_sender("out", why, sizeof(why)) == 0)
            fflog(LOG_NOTICE, "senderout: the GRBL controller keeps the sender out for %s again", id);
    } else if (!id[0] && out) {
        if (port_sender("in", why, sizeof(why)) == 0)
            fflog(LOG_NOTICE, "senderout: the GRBL controller lets senders in again");
    }
}

#ifndef SENDEROUT_NO_THREAD
static void *keeper(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, (long)(SENDEROUT_TICK_S * 1e9) };
    for (;;) {
        senderout_tick();
        nanosleep(&ts, NULL);
    }
    return NULL;
}
#endif

void senderout_init(void)
{
    /* A claim the host keeps fresh outlived the daemon that granted it:
     * it is taken up again, the lease with it. */
    char dir[256], owner[LEASE_OWNER_MAX], lw[160];
    snprintf(dir, sizeof(dir), "%s/sender-out", ff_run_dir());
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char id[SENDEROUT_ID_MAX + 1];
        size_t n = strlen(e->d_name);
        if (n < 6 || n - 5 > SENDEROUT_ID_MAX || strcmp(e->d_name + n - 5, ".json"))
            continue;
        snprintf(id, sizeof(id), "%.*s", (int)(n - 5), e->d_name);
        if (!senderout_id_ok(id) || !claim_fresh(id))
            continue;
        owner_of(id, owner, sizeof(owner));
        if (lease_take(owner, LEASE_EXTENSION, NULL, lw, sizeof(lw)) != 0)
            continue;
        pthread_mutex_lock(&mu);
        snprintf(holder, sizeof(holder), "%s", id);
        since = claimed_at = mono();
        pushed_at = 0;
        pthread_mutex_unlock(&mu);
        fflog(LOG_NOTICE, "senderout: %s still keeps the Grbl sender out", id);
        break;
    }
    if (d)
        closedir(d);
#ifndef SENDEROUT_NO_THREAD
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, keeper, NULL) != 0)
        fflog(LOG_ERR, "senderout: the keeper did not start");
    pthread_attr_destroy(&a);
#endif
}
