/*
 * lease.c - the machine lease: one answer to "who has the machine?"
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See lease.h. Two levels at most: a holder, and one owner it let in
 * under itself.
 */
#include "lease.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DEPTH 2

int (*lease_sender_connected)(void);
int (*lease_motors_released)(void);

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char owner[LEASE_OWNER_MAX];
    lease_kind_t kind;
    double since;
} held[DEPTH];
static int depth;

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static const char *kind_name(lease_kind_t k)
{
    return k == LEASE_HARDWARE ? "hardware" : k == LEASE_SENDER ? "sender" :
           k == LEASE_SYSTEM ? "system" : "export";
}

void lease_words(const char *owner, char *buf, size_t len)
{
    static const struct { const char *prefix, *words; } what[] = {
        { "diag:",   "a diagnostic" },
        { "wizard:", "a setup wizard" },
        { "update:", "an update job" },
        { "job:",    "a job" },
        { "ext:",    "an extension" },
    };
    for (size_t i = 0; i < sizeof(what) / sizeof(what[0]); i++) {
        size_t n = strlen(what[i].prefix);
        if (!strncmp(owner, what[i].prefix, n)) {
            snprintf(buf, len, "%s (%s)", what[i].words, owner + n);
            return;
        }
    }
    if (!strcmp(owner, "recorder"))
        snprintf(buf, len, "the dose-curve recorder");
    else if (!strcmp(owner, "logs.export"))
        snprintf(buf, len, "a log export");
    else
        snprintf(buf, len, "%s", owner);
}

static void refusal_locked(char *why, size_t len)
{
    char words[LEASE_OWNER_MAX + 32];
    lease_words(held[depth - 1].owner, words, sizeof(words));
    snprintf(why, len, "%s holds the machine", words);
}

int lease_take(const char *owner, lease_kind_t kind, const char *under, char *why, size_t len)
{
    int rc = -1;

    /* The name goes into a JSON document as it is. */
    if (!owner || !*owner || strlen(owner) >= LEASE_OWNER_MAX || strpbrk(owner, "\"\\\n")) {
        snprintf(why, len, "the owner has no usable name");
        return -1;
    }
    /* Asked outside the lock: it reads a file, and nothing here waits on it. */
    int client = kind == LEASE_SENDER && lease_sender_connected && lease_sender_connected();

    pthread_mutex_lock(&mu);
    if (depth == 0 && under)
        snprintf(why, len, "%s does not hold the machine", under);
    else if (depth == 1 && (!under || strcmp(under, held[0].owner)))
        refusal_locked(why, len);
    else if (depth >= DEPTH)
        refusal_locked(why, len);
    else if (client)
        snprintf(why, len, "a sender is connected to the machine - close it first");
    else {
        snprintf(held[depth].owner, sizeof(held[depth].owner), "%s", owner);
        held[depth].kind = kind;
        held[depth].since = mono_s();
        depth++;
        rc = 0;
    }
    pthread_mutex_unlock(&mu);
    return rc;
}

void lease_release(const char *owner)
{
    pthread_mutex_lock(&mu);
    if (depth > 0 && owner && !strcmp(held[depth - 1].owner, owner))
        depth--;
    pthread_mutex_unlock(&mu);
}

int lease_refusal(char *why, size_t len)
{
    pthread_mutex_lock(&mu);
    int is = depth > 0;
    if (is)
        refusal_locked(why, len);
    pthread_mutex_unlock(&mu);
    return is;
}

int lease_refusal_for(const char *as, char *why, size_t len)
{
    pthread_mutex_lock(&mu);
    int is = depth > 0;
    for (int i = 0; as && i < depth; i++)
        if (!strcmp(held[i].owner, as))
            is = 0;
    if (is)
        refusal_locked(why, len);
    pthread_mutex_unlock(&mu);
    return is;
}

int lease_refusal_locks(char *why, size_t len)
{
    pthread_mutex_lock(&mu);
    int is = depth > 0 && held[depth - 1].kind != LEASE_EXPORT;
    if (is)
        refusal_locked(why, len);
    pthread_mutex_unlock(&mu);
    return is;
}

int lease_holder(char *owner, size_t len)
{
    pthread_mutex_lock(&mu);
    int is = depth > 0;
    if (is)
        snprintf(owner, len, "%s", held[depth - 1].owner);
    pthread_mutex_unlock(&mu);
    return is;
}

int lease_json(char *buf, size_t len)
{
    /* Outside the lock, as in lease_take(). */
    int client = lease_sender_connected && lease_sender_connected();
    int released = lease_motors_released && lease_motors_released();
    char words[LEASE_OWNER_MAX + 32];
    int n;

    pthread_mutex_lock(&mu);
    if (depth == 0)
        n = snprintf(buf, len, "\"lease\":{\"holder\":null,");
    else {
        lease_words(held[depth - 1].owner, words, sizeof(words));
        n = snprintf(buf, len, "\"lease\":{\"holder\":{\"owner\":\"%s\",\"kind\":\"%s\",\"for_s\":%.0f,"
                               "\"words\":\"%s\"",
                     held[depth - 1].owner, kind_name(held[depth - 1].kind),
                     mono_s() - held[depth - 1].since, words);
        if (n > 0 && (size_t)n < len && depth > 1)
            n += snprintf(buf + n, len - (size_t)n, ",\"under\":\"%s\"", held[0].owner);
        if (n > 0 && (size_t)n < len)
            n += snprintf(buf + n, len - (size_t)n, "},");
    }
    pthread_mutex_unlock(&mu);
    if (n < 0 || (size_t)n >= len)
        return -1;
    int m = snprintf(buf + n, len - (size_t)n, "\"observed\":{\"sender\":%s,\"motors_released\":%s}}",
                     client ? "true" : "false", released ? "true" : "false");
    return m < 0 || (size_t)(n + m) >= len ? -1 : n + m;
}
