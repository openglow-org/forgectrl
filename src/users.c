/*
 * users.c - the panel account
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * One account, created by the first-run wizard, is the panel login and
 * the SSH login. The record is one line, `name:hash:uid`, in the data
 * directory at mode 0600; the hash is sha512-crypt from crypt(3), the
 * same string the system's shadow file takes, so there is one hash and
 * one truth. The boot replays the record into /etc/passwd and
 * /etc/shadow (the rootfs is rewritten by every update), and this
 * module replays it the same way when the account changes.
 *
 * Root is not in the record and is never touched: root has no password
 * and works at the console by design; sshd refuses it.
 *
 * The reset: holding the button through the daemon's start marks the
 * account for re-creation. The old record stays (the panel keeps
 * refusing the old password), users_exist() reads false, and the
 * wizard's account step runs again.
 */
#define _GNU_SOURCE
#include "users.h"
#include "fflog.h"
#include "paths.h"
#include "sha256.h"

#include <crypt.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REPLAY_SCRIPT "/etc/init.d/forgefirm-users"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char u_name[USERS_NAME_MAX + 1];
static char u_hash[128];
static long u_uid;
static int u_have;
static int u_reset;

static void record_path(char *buf, size_t len)
{
    snprintf(buf, len, "%s/users", ff_data_dir());
}

static void reset_path(char *buf, size_t len)
{
    snprintf(buf, len, "%s/users.reset", ff_data_dir());
}

static void load_locked(void)
{
    char path[256], line[256];
    record_path(path, sizeof(path));
    u_have = 0;
    u_name[0] = u_hash[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *h = strchr(line, ':');
        if (h) {
            *h++ = '\0';
            char *u = strchr(h, ':');
            if (u) {
                *u++ = '\0';
                if (strlen(line) <= USERS_NAME_MAX && strlen(h) < sizeof(u_hash) &&
                    h[0] == '$') {
                    snprintf(u_name, sizeof(u_name), "%s", line);
                    snprintf(u_hash, sizeof(u_hash), "%s", h);
                    u_uid = atol(u);
                    u_have = 1;
                }
            }
        }
    }
    fclose(f);
    if (!u_have)
        fflog(LOG_WARNING, "users: %s is not a valid record", path);
    reset_path(path, sizeof(path));
    u_reset = access(path, F_OK) == 0;
}

void users_init(void)
{
    pthread_mutex_lock(&mu);
    load_locked();
    fflog(LOG_INFO, "users: %s%s", u_have ? "account present" : "no account",
          u_reset ? " (reset pending)" : "");
    pthread_mutex_unlock(&mu);
}

int users_exist(void)
{
    pthread_mutex_lock(&mu);
    int v = u_have && !u_reset;
    pthread_mutex_unlock(&mu);
    return v;
}

int users_name(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    int have = u_have;
    snprintf(buf, len, "%s", have ? u_name : "");
    pthread_mutex_unlock(&mu);
    return have ? 0 : -1;
}

int users_check_name(const char *name, char *reason, size_t rlen)
{
    size_t n = name ? strlen(name) : 0;
    if (n < 2 || n > USERS_NAME_MAX) {
        snprintf(reason, rlen, "the name must be 2 to %d characters", USERS_NAME_MAX);
        return -1;
    }
    if (!islower((unsigned char)name[0])) {
        snprintf(reason, rlen, "the name must start with a lowercase letter");
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!islower(c) && !isdigit(c) && c != '-' && c != '_') {
            snprintf(reason, rlen, "the name can hold lowercase letters, digits, '-' and '_'");
            return -1;
        }
    }
    if (!strcmp(name, "root")) {
        snprintf(reason, rlen, "root is the system account");
        return -1;
    }
    return 0;
}

int users_check_password(const char *name, const char *password,
                         char *reason, size_t rlen)
{
    size_t n = password ? strlen(password) : 0;
    if (n < USERS_PASSWORD_MIN) {
        snprintf(reason, rlen, "the password must be at least %d characters",
                 USERS_PASSWORD_MIN);
        return -1;
    }
    if (n > USERS_PASSWORD_MAX) {
        snprintf(reason, rlen, "the password must be at most %d characters",
                 USERS_PASSWORD_MAX);
        return -1;
    }
    if (name && !strcmp(name, password)) {
        snprintf(reason, rlen, "the password must not be the name");
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)password[i] < 0x20 || password[i] == ':') {
            snprintf(reason, rlen, "the password cannot hold control characters or ':'");
            return -1;
        }
    return 0;
}

/* A sha512-crypt salt: "$6$" + 16 characters from the crypt alphabet,
 * from /dev/urandom. Returns 0, or -1 without entropy (no account is
 * made with a weak salt). */
static int make_salt(char *out, size_t len)
{
    static const char alphabet[] =
        "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char raw[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, raw, sizeof(raw)) != (ssize_t)sizeof(raw)) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    close(fd);
    if (len < 3 + sizeof(raw) + 1)
        return -1;
    out[0] = '$'; out[1] = '6'; out[2] = '$';
    for (size_t i = 0; i < sizeof(raw); i++)
        out[3 + i] = alphabet[raw[i] & 63];
    out[3 + sizeof(raw)] = '\0';
    return 0;
}

static int save_locked(void)
{
    char path[256], tmp[264];
    record_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    mkdir(ff_data_dir(), 0755);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    char line[256];
    int n = snprintf(line, sizeof(line), "%s:%s:%ld\n", u_name, u_hash, u_uid);
    int ok = n > 0 && n < (int)sizeof(line) && write(fd, line, (size_t)n) == n &&
             fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int users_create(const char *name, const char *password, char *reason,
                 size_t rlen)
{
    if (users_check_name(name, reason, rlen) != 0 ||
        users_check_password(name, password, reason, rlen) != 0)
        return -1;
    char salt[32];
    if (make_salt(salt, sizeof(salt)) != 0) {
        snprintf(reason, rlen, "no entropy available for the password hash");
        return -1;
    }
    struct crypt_data cd;
    memset(&cd, 0, sizeof(cd));
    const char *h = crypt_r(password, salt, &cd);
    if (!h || h[0] != '$' || strlen(h) >= sizeof(u_hash)) {
        snprintf(reason, rlen, "the password hash failed");
        return -1;
    }
    pthread_mutex_lock(&mu);
    snprintf(u_name, sizeof(u_name), "%s", name);
    snprintf(u_hash, sizeof(u_hash), "%s", h);
    u_uid = USERS_UID;
    u_have = 1;
    int rc = save_locked();
    if (rc == 0) {
        char path[256];
        reset_path(path, sizeof(path));
        unlink(path);
        u_reset = 0;
    }
    memset(&cd, 0, sizeof(cd));
    pthread_mutex_unlock(&mu);
    if (rc != 0) {
        snprintf(reason, rlen, "cannot write the account record");
        return -1;
    }
    fflog(LOG_NOTICE, "users: account '%s' created", name);
    users_replay();
    return 0;
}

int users_verify(const char *name, const char *password)
{
    if (!name || !password)
        return 0;
    pthread_mutex_lock(&mu);
    int have = u_have && !u_reset;
    char hash[128], uname[USERS_NAME_MAX + 1];
    snprintf(hash, sizeof(hash), "%s", u_hash);
    snprintf(uname, sizeof(uname), "%s", u_name);
    pthread_mutex_unlock(&mu);
    if (!have)
        return 0;
    /* Always run the hash so a wrong name costs the same as a wrong
     * password. */
    struct crypt_data cd;
    memset(&cd, 0, sizeof(cd));
    const char *h = crypt_r(password, hash, &cd);
    int ok = h && strlen(h) == strlen(hash) && ct_equal(h, hash, strlen(hash));
    ok = ok && strlen(name) == strlen(uname) && ct_equal(name, uname, strlen(uname));
    memset(&cd, 0, sizeof(cd));
    return ok;
}

int users_mark_reset(void)
{
    char path[256];
    reset_path(path, sizeof(path));
    mkdir(ff_data_dir(), 0755);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    close(fd);
    pthread_mutex_lock(&mu);
    u_reset = 1;
    pthread_mutex_unlock(&mu);
    fflog(LOG_WARNING, "users: account reset requested; the account step runs again");
    return 0;
}

int users_reset_pending(void)
{
    pthread_mutex_lock(&mu);
    int v = u_reset;
    pthread_mutex_unlock(&mu);
    return v;
}

void users_replay(void)
{
    if (access(REPLAY_SCRIPT, X_OK) != 0)
        return;                         /* host builds */
    if (system(REPLAY_SCRIPT " start >/dev/null 2>&1") != 0)
        fflog(LOG_ERR, "users: the account replay script failed");
}
