/*
 * session.c - panel login sessions
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A session is 256 random bits, carried in an HttpOnly, Secure,
 * SameSite=Strict cookie, and idle-expired after twelve hours. The
 * sessions live in a root-only file in the runtime directory, so a
 * restart of the daemon keeps everyone logged in and a reboot logs
 * everyone out; the file is rewritten when a session is made or ended,
 * and its idle stamps every few minutes. The store is small: one
 * operator and a few browsers. Comparison is constant time. Login failures are counted per client address, and
 * five in a row lock that address for thirty seconds; the count is
 * the cheap brake against a guessing loop on the LAN, not a
 * substitute for a good password.
 */
#define _GNU_SOURCE
#include "session.h"
#include "sha256.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_SESSIONS 16
#define MAX_ADDRS 32
#define SESSION_SAVE_S 300      /* how often the idle stamps reach the store */

typedef struct {
    char id[SESSION_ID_HEX + 1];
    char name[40];
    time_t created;
    time_t seen;
    int used;
} session_t;

typedef struct {
    char addr[64];
    int fails;
    time_t until;
    int used;
} addr_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static session_t sessions[MAX_SESSIONS];
static addr_t addrs[MAX_ADDRS];
static time_t (*clock_fn)(void);
static char store_path[256];    /* "" = memory only */
static time_t saved_at;

static void save_locked(void);

static time_t now(void)
{
    return clock_fn ? clock_fn() : time(NULL);
}

void session_set_clock(time_t (*fn)(void))
{
    clock_fn = fn;
}

static int random_hex(char *out, size_t hexlen)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[SESSION_ID_HEX / 2];
    if (hexlen / 2 > sizeof(raw))
        return -1;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, raw, hexlen / 2);
    close(fd);
    if (n != (ssize_t)(hexlen / 2))
        return -1;
    for (size_t i = 0; i < hexlen / 2; i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 0xf];
    }
    out[hexlen] = '\0';
    return 0;
}

static int expire_locked(time_t t)
{
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used && t - sessions[i].seen > SESSION_IDLE_S) {
            memset(&sessions[i], 0, sizeof(sessions[i]));
            n++;
        }
    return n;
}

int session_create(const char *name, char *out, size_t len)
{
    if (len < SESSION_ID_HEX + 1)
        return -1;
    char id[SESSION_ID_HEX + 1];
    if (random_hex(id, SESSION_ID_HEX) != 0)
        return -1;
    time_t t = now();
    pthread_mutex_lock(&mu);
    expire_locked(t);
    int slot = -1, oldest = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].used) {
            slot = i;
            break;
        }
        if (oldest < 0 || sessions[i].seen < sessions[oldest].seen)
            oldest = i;
    }
    if (slot < 0)
        slot = oldest;
    memset(&sessions[slot], 0, sizeof(sessions[slot]));
    snprintf(sessions[slot].id, sizeof(sessions[slot].id), "%s", id);
    snprintf(sessions[slot].name, sizeof(sessions[slot].name), "%s",
             name ? name : "");
    sessions[slot].created = sessions[slot].seen = t;
    sessions[slot].used = 1;
    save_locked();
    pthread_mutex_unlock(&mu);
    snprintf(out, len, "%s", id);
    return 0;
}

static int id_well_formed(const char *id)
{
    if (!id || strlen(id) != SESSION_ID_HEX)
        return 0;
    for (int i = 0; i < SESSION_ID_HEX; i++)
        if (!strchr("0123456789abcdef", id[i]))
            return 0;
    return 1;
}

/* The store: one line per session, the id, the stamps, the name. */
static void save_locked(void)
{
    if (!store_path[0])
        return;
    char tmp[280];
    snprintf(tmp, sizeof(tmp), "%s.tmp", store_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return;
    }
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used)
            fprintf(f, "%s %ld %ld %s\n", sessions[i].id, (long)sessions[i].created,
                    (long)sessions[i].seen, sessions[i].name);
    int ok = fflush(f) == 0 && fsync(fd) == 0;
    fclose(f);
    if (!ok || rename(tmp, store_path) != 0)
        unlink(tmp);
    saved_at = now();
}

static void load_locked(void)
{
    memset(sessions, 0, sizeof(sessions));
    FILE *f = fopen(store_path, "r");
    if (!f)
        return;
    char line[200];
    time_t t = now();
    int n = 0;
    while (n < MAX_SESSIONS && fgets(line, sizeof(line), f)) {
        char id[SESSION_ID_HEX + 1], name[40] = "";
        long created = 0, seen = 0;
        if (sscanf(line, "%64s %ld %ld %39[^\n]", id, &created, &seen, name) < 3)
            continue;
        if (!id_well_formed(id) || t - (time_t)seen > SESSION_IDLE_S)
            continue;
        snprintf(sessions[n].id, sizeof(sessions[n].id), "%s", id);
        snprintf(sessions[n].name, sizeof(sessions[n].name), "%s", name);
        sessions[n].created = (time_t)created;
        sessions[n].seen = (time_t)seen;
        sessions[n].used = 1;
        n++;
    }
    fclose(f);
}

void session_set_store(const char *path)
{
    pthread_mutex_lock(&mu);
    snprintf(store_path, sizeof(store_path), "%s", path ? path : "");
    if (store_path[0])
        load_locked();
    pthread_mutex_unlock(&mu);
}

int session_valid(const char *id)
{
    if (!id_well_formed(id))
        return 0;
    time_t t = now();
    int found = 0;
    pthread_mutex_lock(&mu);
    int expired = expire_locked(t);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].used)
            continue;
        /* Compare every live session in constant time; do not stop at
         * the first match. */
        int eq = ct_equal(sessions[i].id, id, SESSION_ID_HEX);
        if (eq) {
            sessions[i].seen = t;
            found = 1;
        }
    }
    if (expired || (found && t - saved_at >= SESSION_SAVE_S))
        save_locked();
    pthread_mutex_unlock(&mu);
    return found;
}

void session_end(const char *id)
{
    pthread_mutex_lock(&mu);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].used)
            continue;
        if (!id || (id_well_formed(id) &&
                    ct_equal(sessions[i].id, id, SESSION_ID_HEX)))
            memset(&sessions[i], 0, sizeof(sessions[i]));
    }
    save_locked();
    pthread_mutex_unlock(&mu);
}

int session_from_cookie(const char *cookie, char *out, size_t len)
{
    if (!cookie || len < SESSION_ID_HEX + 1)
        return 0;
    const char *p = cookie;
    size_t klen = strlen(SESSION_COOKIE);
    while (*p) {
        while (*p == ' ' || *p == ';')
            p++;
        if (!strncmp(p, SESSION_COOKIE, klen) && p[klen] == '=') {
            p += klen + 1;
            size_t n = strcspn(p, "; ");
            if (n == SESSION_ID_HEX) {
                memcpy(out, p, n);
                out[n] = '\0';
                return id_well_formed(out);
            }
            return 0;
        }
        p += strcspn(p, ";");
    }
    return 0;
}

void session_cookie_set(const char *id, char *out, size_t len)
{
    snprintf(out, len, SESSION_COOKIE "=%s; Path=/; HttpOnly; Secure; "
             "SameSite=Strict; Max-Age=%d", id, SESSION_IDLE_S);
}

void session_cookie_clear(char *out, size_t len)
{
    snprintf(out, len, SESSION_COOKIE "=; Path=/; HttpOnly; Secure; "
             "SameSite=Strict; Max-Age=0");
}

static addr_t *addr_slot_locked(const char *addr, int make)
{
    addr_t *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < MAX_ADDRS; i++) {
        if (addrs[i].used && !strcmp(addrs[i].addr, addr))
            return &addrs[i];
        if (!addrs[i].used && !free_slot)
            free_slot = &addrs[i];
        if (addrs[i].used && (!oldest || addrs[i].until < oldest->until))
            oldest = &addrs[i];
    }
    if (!make)
        return NULL;
    addr_t *a = free_slot ? free_slot : oldest;
    memset(a, 0, sizeof(*a));
    snprintf(a->addr, sizeof(a->addr), "%s", addr);
    a->used = 1;
    return a;
}

int login_locked(const char *addr, int *seconds_left)
{
    if (!addr)
        addr = "";
    time_t t = now();
    pthread_mutex_lock(&mu);
    addr_t *a = addr_slot_locked(addr, 0);
    int locked = a && a->until > t;
    if (seconds_left)
        *seconds_left = locked ? (int)(a->until - t) : 0;
    pthread_mutex_unlock(&mu);
    return locked;
}

void login_failed(const char *addr)
{
    if (!addr)
        addr = "";
    time_t t = now();
    pthread_mutex_lock(&mu);
    addr_t *a = addr_slot_locked(addr, 1);
    if (a->until && a->until <= t)
        a->fails = 0;               /* a lock that expired starts over */
    a->fails++;
    if (a->fails >= LOGIN_FAILS) {
        a->until = t + LOGIN_LOCK_S;
        a->fails = 0;
    }
    pthread_mutex_unlock(&mu);
}

void login_succeeded(const char *addr)
{
    if (!addr)
        addr = "";
    pthread_mutex_lock(&mu);
    addr_t *a = addr_slot_locked(addr, 0);
    if (a)
        memset(a, 0, sizeof(*a));
    pthread_mutex_unlock(&mu);
}
