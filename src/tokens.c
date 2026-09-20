/*
 * tokens.c - scoped API tokens: a named credential that reaches what it was granted
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See tokens.h. The store is <data dir>/tokens, mode 0600, one token a
 * line: id:sha256:created:last_used:capabilities:name. It is written
 * whole, through a temporary file and a rename. The last-used time is
 * kept in memory and written at most every ten minutes, and at a create,
 * a revoke, and the daemon's exit: a client that polls with its token
 * must not wear the flash. Both times are the wall clock's, for display
 * only; the ten minutes are CLOCK_MONOTONIC's.
 */
#define _GNU_SOURCE
#include "tokens.h"
#include "fflog.h"
#include "paths.h"
#include "sha256.h"

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FLUSH_EVERY_S 600.0

/* The closed list: every capability of the plan's table that a route
 * serves today. */
static const char *const CAPS[] = {
    "machine.read", "events", "camera.lid", "camera.head", "motion.jog", "motion.job",
};
#define NCAPS (sizeof(CAPS) / sizeof(CAPS[0]))

typedef struct {
    char id[TOKENS_ID_HEX + 1];
    unsigned char hash[SHA256_LEN];
    char name[TOKENS_NAME_MAX + 1];
    unsigned caps;                      /* bit i: CAPS[i] */
    long created, last_used;
} token_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static token_t store[TOKENS_MAX];
static int count;
static int dirty;                       /* a last_used newer than the file's */
static double last_flush;

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void store_path(char *buf, size_t len)
{
    snprintf(buf, len, "%s/tokens", ff_data_dir());
}

static int cap_index(const char *cap, size_t n)
{
    for (size_t i = 0; i < NCAPS; i++)
        if (strlen(CAPS[i]) == n && !strncmp(CAPS[i], cap, n))
            return (int)i;
    return -1;
}

int tokens_cap_known(const char *cap)
{
    return cap && cap_index(cap, strlen(cap)) >= 0;
}

/* A comma-separated list to the bit set: 0 with the set, or -1 with the
 * word that is no capability in bad. */
static int caps_parse(const char *csv, unsigned *set, char *bad, size_t blen)
{
    *set = 0;
    for (const char *p = csv ? csv : ""; *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        size_t n = strcspn(p, ", ");
        if (!n)
            break;
        int i = cap_index(p, n);
        if (i < 0) {
            snprintf(bad, blen, "%.*s", (int)(n < 40 ? n : 40), p);
            return -1;
        }
        *set |= 1u << i;
        p += n;
    }
    return 0;
}

static size_t caps_text(unsigned set, char *buf, size_t len, const char *quote)
{
    size_t off = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < NCAPS && off < len; i++)
        if (set & (1u << i))
            off += (size_t)snprintf(buf + off, len - off, "%s%s%s%s", off ? "," : "", quote, CAPS[i], quote);
    return off;
}

static int hex_ok(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i]))
            return 0;
    return 1;
}

static int name_ok(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n > TOKENS_NAME_MAX || name[0] == ' ' || name[n - 1] == ' ')
        return 0;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr(" ._-", *p))
            return 0;
    return 1;
}

/* ------------------------------------------------------------ the file */

static void load_locked(void)
{
    char path[256], line[512];
    store_path(path, sizeof(path));
    count = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (count < TOKENS_MAX && fgets(line, sizeof(line), f)) {
        token_t t = { 0 };
        char *field[6], *p = line;
        line[strcspn(line, "\r\n")] = '\0';
        int n = 0;
        for (; n < 5 && p; n++) {
            field[n] = p;
            p = strchr(p, ':');
            if (p)
                *p++ = '\0';
        }
        field[5] = p;
        char bad[48];
        if (n != 5 || !p || strlen(field[0]) != TOKENS_ID_HEX || !hex_ok(field[0], TOKENS_ID_HEX) ||
            strlen(field[1]) != 2 * SHA256_LEN || !hex_ok(field[1], 2 * SHA256_LEN) ||
            caps_parse(field[4], &t.caps, bad, sizeof(bad)) != 0 || !t.caps || !name_ok(field[5])) {
            fflog(LOG_WARNING, "tokens: a line of %s is not a token and is dropped", path);
            continue;
        }
        snprintf(t.id, sizeof(t.id), "%s", field[0]);
        for (int i = 0; i < SHA256_LEN; i++) {
            unsigned v;
            sscanf(field[1] + 2 * i, "%2x", &v);
            t.hash[i] = (unsigned char)v;
        }
        t.created = atol(field[2]);
        t.last_used = atol(field[3]);
        snprintf(t.name, sizeof(t.name), "%s", field[5]);
        store[count++] = t;
    }
    fclose(f);
}

static int save_locked(void)
{
    char path[256], tmp[264], hash[2 * SHA256_LEN + 1], caps[160];
    store_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    mkdir(ff_data_dir(), 0755);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    int ok = 1;
    for (int i = 0; i < count && ok; i++) {
        char line[512];
        sha256_hex(store[i].hash, SHA256_LEN, hash);
        caps_text(store[i].caps, caps, sizeof(caps), "");
        int n = snprintf(line, sizeof(line), "%s:%s:%ld:%ld:%s:%s\n", store[i].id, hash,
                         store[i].created, store[i].last_used, caps, store[i].name);
        ok = n > 0 && n < (int)sizeof(line) && write(fd, line, (size_t)n) == n;
    }
    ok = ok && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    dirty = 0;
    last_flush = mono_s();
    return 0;
}

void tokens_init(void)
{
    pthread_mutex_lock(&mu);
    load_locked();
    last_flush = mono_s();
    fflog(LOG_INFO, "tokens: %d scoped token%s", count, count == 1 ? "" : "s");
    pthread_mutex_unlock(&mu);
}

void tokens_flush(void)
{
    pthread_mutex_lock(&mu);
    if (dirty && save_locked() != 0)
        fflog(LOG_WARNING, "tokens: cannot write the store");
    pthread_mutex_unlock(&mu);
}

/* ------------------------------------------------------------ the check */

int tokens_looks_scoped(const char *presented)
{
    return presented && !strncmp(presented, TOKENS_PREFIX, strlen(TOKENS_PREFIX));
}

int tokens_check(const char *presented, const char *cap, char *id)
{
    if (id)
        id[0] = '\0';
    if (!tokens_looks_scoped(presented) || strlen(presented) != TOKENS_TEXT_LEN ||
        !hex_ok(presented + strlen(TOKENS_PREFIX), TOKENS_TEXT_LEN - strlen(TOKENS_PREFIX)))
        return -1;
    unsigned char h[SHA256_LEN];
    sha256(presented, TOKENS_TEXT_LEN, h);

    size_t n = cap ? strlen(cap) : 0;
    int any = n > 4 && !strcmp(cap + n - 4, ".any");
    int rc = -1;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < count; i++) {
        if (!ct_equal(h, store[i].hash, SHA256_LEN))
            continue;
        if (id)
            snprintf(id, TOKENS_ID_HEX + 1, "%s", store[i].id);
        rc = 0;
        for (size_t c = 0; cap && c < NCAPS; c++) {
            if (!(store[i].caps & (1u << c)))
                continue;
            if (any ? !strncmp(CAPS[c], cap, n - 3) : !strcmp(CAPS[c], cap))
                rc = 1;
        }
        if (rc == 1) {
            store[i].last_used = (long)time(NULL);
            dirty = 1;
            if (mono_s() - last_flush > FLUSH_EVERY_S && save_locked() != 0)
                fflog(LOG_WARNING, "tokens: cannot write the store");
        }
        break;
    }
    pthread_mutex_unlock(&mu);
    return rc;
}

int tokens_holds_only(const char *presented, const char *prefix)
{
    if (!tokens_looks_scoped(presented) || strlen(presented) != TOKENS_TEXT_LEN || !prefix)
        return 0;
    unsigned char h[SHA256_LEN];
    sha256(presented, TOKENS_TEXT_LEN, h);
    int only = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < count; i++) {
        if (!ct_equal(h, store[i].hash, SHA256_LEN))
            continue;
        only = 1;
        for (size_t c = 0; c < NCAPS; c++)
            if ((store[i].caps & (1u << c)) && strncmp(CAPS[c], prefix, strlen(prefix)))
                only = 0;
        break;
    }
    pthread_mutex_unlock(&mu);
    return only;
}

/* ------------------------------------------------- create, revoke, list */

static int random_hex(char *out, size_t bytes)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    int ok = fd >= 0 && bytes <= sizeof(raw) && read(fd, raw, bytes) == (ssize_t)bytes;
    if (fd >= 0)
        close(fd);
    if (!ok)
        return -1;
    for (size_t i = 0; i < bytes; i++) {
        out[2 * i] = hex[raw[i] >> 4];
        out[2 * i + 1] = hex[raw[i] & 0xf];
    }
    out[2 * bytes] = '\0';
    return 0;
}

int tokens_create(const char *name, const char *caps, char *token, char *id,
                  char *err, size_t elen)
{
    unsigned set;
    char bad[48];
    token[0] = id[0] = '\0';
    if (!name_ok(name)) {
        snprintf(err, elen, "name is 1 to %d of letters, digits, spaces, '.', '_' and '-'", TOKENS_NAME_MAX);
        return -1;
    }
    if (caps_parse(caps, &set, bad, sizeof(bad)) != 0) {
        snprintf(err, elen, "%s is not a capability a token can hold", bad);
        return -1;
    }
    if (!set) {
        snprintf(err, elen, "a token holds at least one capability");
        return -1;
    }
    token_t t = { .caps = set, .created = (long)time(NULL) };
    snprintf(t.name, sizeof(t.name), "%s", name);
    /* Never a predictable token: with no entropy there is no token. */
    snprintf(token, TOKENS_TEXT_LEN + 1, "%s", TOKENS_PREFIX);
    if (random_hex(token + strlen(TOKENS_PREFIX), 16) != 0 || random_hex(t.id, TOKENS_ID_HEX / 2) != 0) {
        token[0] = '\0';
        snprintf(err, elen, "cannot read /dev/urandom");
        return -1;
    }
    sha256(token, TOKENS_TEXT_LEN, t.hash);

    int rc = -1;
    pthread_mutex_lock(&mu);
    int clash = 0;
    for (int i = 0; i < count; i++)
        clash |= !strcmp(store[i].name, name) ? 1 : !strcmp(store[i].id, t.id) ? 2 : 0;
    if (count >= TOKENS_MAX)
        snprintf(err, elen, "the machine holds %d tokens at most: revoke one first", TOKENS_MAX);
    else if (clash & 1)
        snprintf(err, elen, "a token named %s exists already", name);
    else if (clash & 2)
        snprintf(err, elen, "try again");       /* a 32-bit id met its twin */
    else {
        store[count++] = t;
        if (save_locked() == 0)
            rc = 0;
        else {
            count--;
            snprintf(err, elen, "cannot write the token store");
        }
    }
    pthread_mutex_unlock(&mu);
    if (rc != 0) {
        token[0] = '\0';
        return -1;
    }
    snprintf(id, TOKENS_ID_HEX + 1, "%s", t.id);
    char text[160];
    caps_text(set, text, sizeof(text), "");
    fflog(LOG_NOTICE, "tokens: created %s (%s) holding %s", t.id, name, text);
    return 0;
}

int tokens_revoke(const char *id, char *err, size_t elen)
{
    int rc = -1;
    char name[TOKENS_NAME_MAX + 1] = "";
    pthread_mutex_lock(&mu);
    for (int i = 0; id && i < count; i++) {
        if (strcmp(store[i].id, id))
            continue;
        token_t gone = store[i];
        memmove(&store[i], &store[i + 1], (size_t)(count - i - 1) * sizeof(store[0]));
        count--;
        if (save_locked() == 0) {
            snprintf(name, sizeof(name), "%s", gone.name);
            rc = 0;
        } else {
            /* Not revoked on disk is not revoked: say so, and keep it listed. */
            memmove(&store[i + 1], &store[i], (size_t)(count - i) * sizeof(store[0]));
            store[i] = gone;
            count++;
            snprintf(err, elen, "cannot write the token store");
            rc = -2;
        }
        break;
    }
    pthread_mutex_unlock(&mu);
    if (rc == -1)
        snprintf(err, elen, "no such token");
    if (rc == 0)
        fflog(LOG_NOTICE, "tokens: revoked %s (%s)", id, name);
    return rc == 0 ? 0 : -1;
}

int tokens_json(char *buf, size_t len)
{
    char caps[200];
    size_t off = 0;
    caps_text((1u << NCAPS) - 1, caps, sizeof(caps), "\"");
    pthread_mutex_lock(&mu);
    off += (size_t)snprintf(buf + off, len - off, "{\"max\":%d,\"caps\":[%s],\"tokens\":[", TOKENS_MAX, caps);
    for (int i = 0; i < count && off < len; i++) {
        caps_text(store[i].caps, caps, sizeof(caps), "\"");
        off += (size_t)snprintf(buf + off, len - off,
                                "%s{\"id\":\"%s\",\"name\":\"%s\",\"caps\":[%s],\"created\":%ld,\"last_used\":%ld}",
                                i ? "," : "", store[i].id, store[i].name, caps, store[i].created,
                                store[i].last_used);
    }
    pthread_mutex_unlock(&mu);
    if (off < len)
        off += (size_t)snprintf(buf + off, len - off, "]}");
    return off < len ? 0 : -1;
}
