/*
 * events.c - the machine's event stream
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See events.h. One sampler thread, asleep while nobody listens, feeds a
 * ring of formatted events; each stream keeps its own place in the ring
 * and waits on one condition. A stream that falls a whole ring behind
 * is told how many events it lost, never handed a stale one.
 */
#define _GNU_SOURCE
#include "events.h"
#include "fflog.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RING 64
#define EVENT_TEXT 256
/* A client that went away is noticed at the next write to it, so the
 * keep-alive is also how soon a dead stream gives its place back. */
#define KEEPALIVE_S 5

void (*events_gather)(events_snap_t *snap);

/* ---- admission ---- */

int events_admit(events_slots_t *s, const char *peer, int *replaced)
{
    int free_slot = -1;
    if (!peer[0])
        peer = "?";
    *replaced = 0;
    for (int i = 0; i < EVENTS_MAX_STREAMS; i++) {
        if (!s->peer[i][0]) {
            if (free_slot < 0)
                free_slot = i;
        } else if (!strcmp(s->peer[i], peer)) {
            *replaced = 1;
            return i;
        }
    }
    if (free_slot < 0)
        return EVENTS_ADMIT_FULL;
    snprintf(s->peer[free_slot], sizeof(s->peer[0]), "%s", peer);
    return free_slot;
}

void events_release(events_slots_t *s, int slot)
{
    if (slot >= 0 && slot < EVENTS_MAX_STREAMS)
        s->peer[slot][0] = '\0';
}

/* ---- the edge detector ---- */

#define SW_BUTTON    (1ul << 2)
#define SW_LID       (1ul << 3)     /* both lid switches in series: set = closed */
#define SW_INTERLOCK (1ul << 5)     /* set = the remote interlock loop is open */

static int is_running(const events_snap_t *s)
{
    return !strcmp(s->controller, "running");
}

void events_diff(const events_snap_t *a, const events_snap_t *b, events_emit_fn emit, void *ctx)
{
    char d[160];

    if ((a->switches ^ b->switches) & SW_LID) {
        snprintf(d, sizeof(d), "{\"closed\":%s}", (b->switches & SW_LID) ? "true" : "false");
        emit(ctx, "lid", d);
    }
    if ((a->switches ^ b->switches) & SW_INTERLOCK) {
        snprintf(d, sizeof(d), "{\"ok\":%s}", (b->switches & SW_INTERLOCK) ? "false" : "true");
        emit(ctx, "interlock", d);
    }

    if (strcmp(a->mode, b->mode)) {
        snprintf(d, sizeof(d), "{\"mode\":\"%s\"}", b->mode);
        emit(ctx, "mode.changed", d);
    }
    if (is_running(a) != is_running(b) || (!is_running(b) && strcmp(a->controller, b->controller))) {
        snprintf(d, sizeof(d), "{\"mode\":\"%s\",\"state\":\"%s\"}", b->mode, b->controller);
        emit(ctx, is_running(b) ? "controller.started" : "controller.stopped", d);
    }

    if (strcmp(a->verdict, b->verdict) || a->fire_ok != b->fire_ok) {
        snprintf(d, sizeof(d), "{\"verdict\":\"%s\",\"fire_ok\":%s}", b->verdict,
                 b->fire_ok ? "true" : "false");
        emit(ctx, "cooling.verdict", d);
    }

    /* A job, by the armed window: the button wait, the window open, a
     * hold and its end inside it, the window closed. */
    if (!a->arming && b->arming)
        emit(ctx, "job.arming", "{}");
    if (!a->armed && b->armed)
        emit(ctx, "job.armed", "{}");
    if (a->armed && b->armed) {
        int held_a = !strcmp(a->gstate, "Hold") || !strcmp(a->gstate, "Door");
        int held_b = !strcmp(b->gstate, "Hold") || !strcmp(b->gstate, "Door");
        if (!held_a && held_b) {
            snprintf(d, sizeof(d), "{\"reason\":\"%s\"}",
                     !strcmp(b->gstate, "Door") ? "lid" : b->fire_ok ? "hold" : "cooling");
            emit(ctx, "job.paused", d);
        } else if (held_a && !held_b && strcmp(b->gstate, "Alarm"))
            emit(ctx, "job.resumed", "{}");
    }
    if (a->armed && !b->armed) {
        snprintf(d, sizeof(d), "{\"result\":\"%s\"}", b->alarm ? "alarm" : "ended");
        emit(ctx, "job.ended", d);
    }

    if (b->alarm && a->alarm != b->alarm) {
        snprintf(d, sizeof(d), "{\"code\":%d}", b->alarm);
        emit(ctx, "alarm", d);
    }

    if (strcmp(a->gstate, "Home") && !strcmp(b->gstate, "Home"))
        emit(ctx, "homing.started", "{}");
    if ((b->homed_axes & 3) == 3 &&
        ((a->homed_axes & 3) != 3 || (!strcmp(a->gstate, "Home") && strcmp(b->gstate, "Home")))) {
        snprintf(d, sizeof(d), "{\"source\":\"%s\",\"axes\":%u}", b->home_source, b->homed_axes);
        emit(ctx, "homing.completed", d);
    } else if (!strcmp(a->gstate, "Home") && strcmp(b->gstate, "Home"))
        emit(ctx, "homing.failed", "{}");

    if (a->released != b->released)
        emit(ctx, b->released ? "motors.released" : "motors.energized", "{}");

    if (strcmp(a->lease, b->lease)) {
        if (b->lease[0])
            snprintf(d, sizeof(d), "{\"owner\":\"%s\"}", b->lease);
        else
            snprintf(d, sizeof(d), "{\"owner\":null}");
        emit(ctx, "lease.changed", d);
    }
}

/* ---- the ring and the streams ---- */

struct events_client {
    int slot;
    unsigned gen;               /* the slot's generation when it was taken */
    unsigned long next;         /* the sequence number it reads next */
    int greeted, farewelled;
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t news = PTHREAD_COND_INITIALIZER;     /* an event, or the stop */
static pthread_cond_t wanted = PTHREAD_COND_INITIALIZER;   /* the first listener */
static char ring[RING][EVENT_TEXT];
static unsigned long head = 1;                              /* the next sequence number */
static events_slots_t slots;
/* One more than the cap: the last is the extension host's (events.h). */
static unsigned slot_gen[EVENTS_MAX_STREAMS + 1];          /* moves when a slot changes hands */
static int listeners, stopping, started;
static int host_open;                                      /* the extension host holds its slot */
static pthread_t sampler;

static void publish_locked(const char *name, const char *data)
{
    snprintf(ring[head % RING], EVENT_TEXT, "id: %lu\nevent: %s\ndata: %s\n\n", head, name, data);
    head++;
    pthread_cond_broadcast(&news);
}

void events_publish(const char *name, const char *data_json)
{
    pthread_mutex_lock(&mu);
    if (listeners)
        publish_locked(name, data_json);
    pthread_mutex_unlock(&mu);
}

static void emit_locked(void *ctx, const char *name, const char *data)
{
    (void)ctx;
    publish_locked(name, data);
}

static void *sampler_thread(void *arg)
{
    (void)arg;
    events_snap_t prev, cur;
    const struct timespec tick = { 0, 1000000000L / EVENTS_HZ };
    int have_prev = 0;

    pthread_mutex_lock(&mu);
    while (!stopping) {
        if (!listeners) {
            /* Nobody listens: no reads, and the next listener starts from
             * a fresh baseline, not from what was true hours ago. */
            have_prev = 0;
            pthread_cond_wait(&wanted, &mu);
            continue;
        }
        pthread_mutex_unlock(&mu);
        if (have_prev)
            cur = prev;
        else
            memset(&cur, 0, sizeof(cur));
        if (events_gather)
            events_gather(&cur);
        pthread_mutex_lock(&mu);
        if (have_prev)
            events_diff(&prev, &cur, emit_locked, NULL);
        prev = cur;
        have_prev = 1;
        pthread_mutex_unlock(&mu);
        nanosleep(&tick, NULL);
        pthread_mutex_lock(&mu);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

void events_init(void)
{
    pthread_mutex_lock(&mu);
    stopping = 0;
    if (!started && pthread_create(&sampler, NULL, sampler_thread, NULL) == 0)
        started = 1;
    pthread_mutex_unlock(&mu);
}

void events_shutdown(void)
{
    pthread_mutex_lock(&mu);
    stopping = 1;
    pthread_cond_broadcast(&news);
    pthread_cond_broadcast(&wanted);
    int join = started;
    started = 0;
    pthread_mutex_unlock(&mu);
    if (join)
        pthread_join(sampler, NULL);
}

static events_client_t *open_slot(const char *peer, int host, char *why, size_t len)
{
    events_client_t *c = calloc(1, sizeof(*c));
    if (!c) {
        snprintf(why, len, "out of memory");
        return NULL;
    }
    int replaced = 0;
    pthread_mutex_lock(&mu);
    if (host) {
        /* Its slot is its own: never full, and the older host stream ends. */
        c->slot = stopping ? EVENTS_ADMIT_FULL : EVENTS_HOST_SLOT;
        replaced = host_open;
        host_open = c->slot >= 0;
    } else {
        c->slot = stopping ? EVENTS_ADMIT_FULL : events_admit(&slots, peer, &replaced);
    }
    if (c->slot >= 0) {
        c->gen = ++slot_gen[c->slot];   /* the older stream of this address, if any, ends */
        c->next = head;
        listeners++;
        pthread_cond_signal(&wanted);
        pthread_cond_broadcast(&news);
    }
    pthread_mutex_unlock(&mu);
    if (c->slot < 0) {
        if (stopping)
            snprintf(why, len, "the daemon is stopping");
        else
            snprintf(why, len, "every event stream is taken (%d in all): fan out from one client",
                     EVENTS_MAX_STREAMS);
        free(c);
        return NULL;
    }
    fflog(LOG_INFO, "events: stream opened for %s%s", peer,
          replaced ? ", in place of its older one" : "");
    return c;
}

events_client_t *events_open(const char *peer, char *why, size_t len)
{
    return open_slot(peer, 0, why, len);
}

events_client_t *events_open_host(char *why, size_t len)
{
    return open_slot("the extension host", 1, why, len);
}

void events_close(events_client_t *c)
{
    if (!c)
        return;
    pthread_mutex_lock(&mu);
    /* A replaced stream's slot is no longer its own. The host's slot is
     * outside the table of addresses and has nothing to give back. */
    if (slot_gen[c->slot] == c->gen) {
        if (c->slot == EVENTS_HOST_SLOT)
            host_open = 0;
        else
            events_release(&slots, c->slot);
    }
    listeners--;
    pthread_mutex_unlock(&mu);
    free(c);
}

long events_next(events_client_t *c, char *buf, size_t max)
{
    long n = -1;

    pthread_mutex_lock(&mu);
    if (!c->greeted && !stopping) {
        c->greeted = 1;
        n = snprintf(buf, max, "retry: 5000\nevent: hello\ndata: {\"max_streams\":%d}\n\n",
                     EVENTS_MAX_STREAMS);
        goto out;
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += KEEPALIVE_S;
    while (!stopping && slot_gen[c->slot] == c->gen && c->next == head) {
        if (pthread_cond_timedwait(&news, &mu, &until) != 0)
            break;
    }
    if (stopping)
        goto out;
    if (slot_gen[c->slot] != c->gen) {
        if (!c->farewelled) {
            c->farewelled = 1;
            n = snprintf(buf, max, "event: bye\ndata: {\"reason\":\"replaced\"}\n\n");
        }
        goto out;
    }
    if (c->next == head) {
        n = snprintf(buf, max, ": keep-alive\n\n");
        goto out;
    }
    if (head - c->next > RING) {
        unsigned long lost = head - RING - c->next;
        c->next = head - RING;
        n = snprintf(buf, max, ": %lu events were lost to a slow reader\n\n", lost);
        goto out;
    }
    n = snprintf(buf, max, "%s", ring[c->next % RING]);
    c->next++;
out:
    pthread_mutex_unlock(&mu);
    if (n >= (long)max)
        n = (long)max - 1;
    return n;
}
