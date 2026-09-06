/*
 * sheetid.c - the commissioning sheet id
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The commissioning sheet is meant to be photographed and shared. It
 * must identify the machine to its owner without revealing the serial
 * number, and the panel's machine id is out too: that id is the serial
 * in base 23, so it reverses. The sheet id is an HMAC-SHA256 of the
 * fuse serial under a 256-bit secret salt that never leaves the
 * machine, rendered as ten base32 characters. It is stable for the
 * machine, shown on the Commissioning tab so an owner can match a
 * sheet, and useless to anyone without the salt.
 *
 * The salt follows the panel token's rules: created once from
 * /dev/urandom, mode 0600, and never replaced by anything guessable
 * when entropy is unavailable (the id is then simply unavailable).
 */
#define _GNU_SOURCE
#include "sheetid.h"
#include "fflog.h"
#include "paths.h"
#include "sha256.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

unsigned long fuse_serial(void);            /* main.c */

static unsigned char salt[SHEETID_SALT_LEN];
static int have_salt;

static void salt_path(char *buf, size_t len)
{
    snprintf(buf, len, "%s/sheet.salt", ff_data_dir());
}

void sheetid_init(void)
{
    char path[256];
    salt_path(path, sizeof(path));
    mkdir(ff_data_dir(), 0755);

    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, salt, sizeof(salt));
        close(fd);
        if (n == (ssize_t)sizeof(salt)) {
            have_salt = 1;
            return;
        }
        fflog(LOG_WARNING, "sheetid: %s is not %d bytes, regenerating",
              path, SHEETID_SALT_LEN);
    }

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, salt, sizeof(salt)) != (ssize_t)sizeof(salt)) {
        if (fd >= 0)
            close(fd);
        memset(salt, 0, sizeof(salt));
        fflog(LOG_ERR, "sheetid: cannot read /dev/urandom - sheet id unavailable");
        return;
    }
    close(fd);

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, salt, sizeof(salt)) != (ssize_t)sizeof(salt)) {
        if (fd >= 0)
            close(fd);
        memset(salt, 0, sizeof(salt));
        fflog(LOG_ERR, "sheetid: cannot persist the salt at %s", path);
        return;
    }
    close(fd);
    have_salt = 1;
    fflog(LOG_NOTICE, "sheetid: generated a new sheet salt");
}

void sheetid_compute(const unsigned char *s, size_t slen, const char *text,
                     char *out, size_t len)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    unsigned char mac[SHA256_LEN];
    hmac_sha256(s, slen, text, strlen(text), mac);
    /* Ten base32 characters need 50 bits: the first 7 bytes suffice. */
    char b32[12];
    unsigned long long acc = 0;
    int bits = 0, o = 0;
    for (int i = 0; i < 7 && o < 10; i++) {
        acc = (acc << 8) | mac[i];
        bits += 8;
        while (bits >= 5 && o < 10) {
            bits -= 5;
            b32[o++] = alphabet[(acc >> bits) & 0x1f];
        }
    }
    b32[o] = '\0';
    snprintf(out, len, "%.5s-%.5s", b32, b32 + 5);
}

int sheetid_derive(const char *text, char *out, size_t len)
{
    if (!have_salt || !text || !*text) {
        if (len)
            out[0] = '\0';
        return -1;
    }
    sheetid_compute(salt, sizeof(salt), text, out, len);
    return 0;
}

int sheetid_get(char *out, size_t len)
{
    char serial[24];
    unsigned long s = fuse_serial();
    if (!s) {
        if (len)
            out[0] = '\0';
        return -1;
    }
    snprintf(serial, sizeof(serial), "%lu", s);
    return sheetid_derive(serial, out, len);
}
