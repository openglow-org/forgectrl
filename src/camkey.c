/*
 * camkey.c - the camera key
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A consumer of the camera (LightBurn's camera URL, a stream viewer, a
 * script) has no login session and often no TLS. The camera key is a
 * per-machine secret that authorizes the read-only routes alone, on
 * either listener, as a `key` query parameter or an X-ForgeFIRM-Camera-Key
 * header. It never authorizes a write, so a leaked key exposes what the
 * read routes show and nothing more, and the owner rotates it from the
 * panel. It is made at first start like the panel token: from
 * /dev/urandom, mode 0600 in the data directory, never anything
 * guessable when entropy is unavailable.
 */
#define _GNU_SOURCE
#include "camkey.h"
#include "fflog.h"
#include "paths.h"
#include "sha256.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char key[CAMKEY_HEX + 1];

static void key_path(char *buf, size_t len)
{
    snprintf(buf, len, "%s/camera.key", ff_data_dir());
}

static int make_key_locked(void)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[CAMKEY_HEX / 2];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, raw, sizeof(raw)) != (ssize_t)sizeof(raw)) {
        if (fd >= 0)
            close(fd);
        key[0] = '\0';
        fflog(LOG_ERR, "camkey: cannot read /dev/urandom - camera key unavailable");
        return -1;
    }
    close(fd);
    for (size_t i = 0; i < sizeof(raw); i++) {
        key[i * 2] = hex[raw[i] >> 4];
        key[i * 2 + 1] = hex[raw[i] & 0xf];
    }
    key[CAMKEY_HEX] = '\0';

    char path[256], tmp[264];
    key_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    mkdir(ff_data_dir(), 0755);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int ok = fd >= 0 && write(fd, key, CAMKEY_HEX) == CAMKEY_HEX &&
             write(fd, "\n", 1) == 1 && fsync(fd) == 0;
    if (fd >= 0)
        close(fd);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        key[0] = '\0';
        fflog(LOG_ERR, "camkey: cannot persist the camera key");
        return -1;
    }
    return 0;
}

void camkey_init(void)
{
    char path[256], buf[CAMKEY_HEX + 4] = "";
    key_path(path, sizeof(path));
    pthread_mutex_lock(&mu);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            size_t n = strspn(buf, "0123456789abcdef");
            if (n == CAMKEY_HEX && (buf[n] == '\0' || buf[n] == '\n')) {
                memcpy(key, buf, CAMKEY_HEX);
                key[CAMKEY_HEX] = '\0';
            }
        }
        fclose(f);
    }
    if (!key[0] && make_key_locked() == 0)
        fflog(LOG_NOTICE, "camkey: generated a new camera key");
    pthread_mutex_unlock(&mu);
}

const char *camkey_get(void)
{
    return key;
}

int camkey_rotate(void)
{
    pthread_mutex_lock(&mu);
    int rc = make_key_locked();
    pthread_mutex_unlock(&mu);
    if (rc == 0)
        fflog(LOG_NOTICE, "camkey: camera key rotated");
    return rc;
}

int camkey_valid(const char *presented)
{
    if (!presented || !key[0] || strlen(presented) != CAMKEY_HEX)
        return 0;
    pthread_mutex_lock(&mu);
    int ok = key[0] && ct_equal(presented, key, CAMKEY_HEX);
    pthread_mutex_unlock(&mu);
    return ok;
}
