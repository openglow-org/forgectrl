/*
 * holds.c - the holds an extension package has on a job, as the engine reads them
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See holds.h for the rules. Everything in a hold file is written by the
 * extension host, and the reason in it by a package: the text that goes on
 * into the verdict is cut to printable ASCII without the two characters
 * that would end a JSON string.
 */
#define _GNU_SOURCE
#include "holds.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void plain(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e || *s == '"' || *s == '\\')
            *s = '?';
}

/* The first hold that stands names the verdict's reason. */
static void stand(holds_t *out, const char *text)
{
    if (!out->standing)
        snprintf(out->reason, sizeof(out->reason), "%s", text);
    out->standing = 1;
}

static int hold_file(const struct dirent *e)
{
    size_t n = strlen(e->d_name);
    return e->d_name[0] != '.' && n > 5 && strcmp(e->d_name + n - 5, ".json") == 0;
}

/* A package id as the host installs them: lowercase reverse-DNS. */
static int id_shaped(const char *s)
{
    size_t n = strlen(s);
    return n > 0 && n < 64 && s[0] != '.' && strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789.-") == n;
}

static int marker_file(const struct dirent *e)
{
    return id_shaped(e->d_name);
}

/* One file: 0 when it was read and judged, -1 when it cannot be read. */
static int judge(const char *dir, const char *name, double now, holds_t *out)
{
    char path[512], text[HOLDS_FILE_MAX + 1], id[72], why[HOLDS_REASON_MAX], say[HOLDS_REASON_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, text, sizeof(text));
    close(fd);
    if (n <= 0 || n > HOLDS_FILE_MAX)
        return -1;
    text[n] = '\0';

    json_t *j = json_loads(text, JSON_REJECT_DUPLICATES, NULL);
    if (!json_is_object(j)) {
        json_decref(j);
        return -1;
    }
    const json_t *jid = json_object_get(j, "id"), *jreq = json_object_get(j, "required"),
                 *jup = json_object_get(j, "raised"), *jwhy = json_object_get(j, "reason"),
                 *jts = json_object_get(j, "ts_mono");
    int ok = json_is_string(jid) && json_is_boolean(jreq) && json_is_boolean(jup) && json_is_string(jwhy) &&
             json_is_number(jts);
    /* The file is named after the package it speaks for. */
    size_t stem = strlen(name) - 5;
    ok = ok && strlen(json_string_value(jid)) == stem && strncmp(json_string_value(jid), name, stem) == 0 &&
         stem < sizeof(id);
    if (!ok) {
        json_decref(j);
        return -1;
    }
    snprintf(id, sizeof(id), "%s", json_string_value(jid));
    snprintf(why, sizeof(why), "%s", json_string_value(jwhy));
    int required = json_is_true(jreq), raised = json_is_true(jup);
    double age = now - json_number_value(jts);
    json_decref(j);
    plain(id);
    plain(why);

    if (age > HOLDS_FRESH_S || age < -HOLDS_FRESH_S) {
        if (required) {
            out->stale_required++;
            snprintf(say, sizeof(say), "%.64s: the extension host is not answering", id);
            stand(out, say);
        } else {
            out->stale_advisory++;
        }
        return 0;
    }
    if (raised) {
        out->raised++;
        snprintf(say, sizeof(say), "%.64s: %.44s", id, why[0] ? why : "hold");
        stand(out, say);
    }
    return 0;
}

void holds_read(const char *dir, const char *required_dir, double now, holds_t *out)
{
    static char spoken[HOLDS_MAX][72];  /* the packages a file speaks for (the engine's one thread calls this) */
    int nspoken = 0;
    memset(out, 0, sizeof(*out));
    struct dirent **list = NULL;
    int n = scandir(dir, &list, hold_file, alphasort);
    if (n < 0 && errno != ENOENT) {
        /* A directory that is there and cannot be read may hold anything. */
        out->unreadable++;
        stand(out, "the hold files cannot be read");
    }
    for (int i = 0; i < n; i++) {
        if (i < HOLDS_MAX) {
            out->files++;
            snprintf(spoken[nspoken++], sizeof(spoken[0]), "%.*s", (int)(strlen(list[i]->d_name) - 5), list[i]->d_name);
            if (judge(dir, list[i]->d_name, now, out) != 0) {
                char name[72], say[HOLDS_REASON_MAX];
                snprintf(name, sizeof(name), "%.64s", list[i]->d_name);
                plain(name);
                out->unreadable++;
                snprintf(say, sizeof(say), "hold file %.64s cannot be read", name);
                stand(out, say);
            }
        }
        free(list[i]);
    }
    free(list);
    if (n > HOLDS_MAX) {
        out->unreadable += n - HOLDS_MAX;
        stand(out, "more hold files than there are package accounts");
    }

    /* A required hold nobody has spoken for: the files are gone with a
     * reboot, and a host that never came up wrote none. */
    if (!required_dir)
        return;
    list = NULL;
    n = scandir(required_dir, &list, marker_file, alphasort);
    if (n < 0 && errno != ENOENT) {
        out->unreadable++;
        stand(out, "the required holds cannot be read");
    }
    for (int i = 0; i < n; i++) {
        int has = 0;
        for (int k = 0; k < nspoken; k++)
            has |= strcmp(spoken[k], list[i]->d_name) == 0;
        if (!has && i < HOLDS_MAX) {
            char say[HOLDS_REASON_MAX];
            out->stale_required++;
            snprintf(say, sizeof(say), "%.64s: the extension host is not answering", list[i]->d_name);
            stand(out, say);
        }
        free(list[i]);
    }
    free(list);
}
