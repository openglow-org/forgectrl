/*
 * mcode.c - the M-codes extension packages answer: the relay between the GRBL controller and the extension host
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See mcode.h.
 */
#define _GNU_SOURCE
#include "mcode.h"

#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "extpkg.h"
#include "fflog.h"
#include "grblport.h"

void mcode_answer_line(unsigned seq, const char *doc, const char *why, char *out, size_t len)
{
    char text[MCODE_TEXT_MAX + 1] = "", raw[300] = "";
    int ok = 0;
    json_t *j = doc ? json_loads(doc, 0, NULL) : NULL;
    if (json_is_object(j)) {
        json_int_t status = json_integer_value(json_object_get(j, "status"));
        json_t *body = json_object_get(j, "body");
        const char *msg = json_string_value(json_object_get(body, "message"));
        const char *err = json_string_value(json_object_get(body, "error"));
        ok = status >= 200 && status < 300;
        if (ok)
            snprintf(raw, sizeof(raw), "%s", msg ? msg : "");
        else if (err || msg)
            snprintf(raw, sizeof(raw), "%s", err ? err : msg);
        else
            snprintf(raw, sizeof(raw), "its extension answered %lld", (long long)status);
    } else {
        snprintf(raw, sizeof(raw), "%s", doc ? "its extension's answer is not JSON" : why && why[0] ? why : "no answer");
    }
    json_decref(j);

    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && n < MCODE_TEXT_MAX; p++) {
        unsigned char c = *p;
        if (c == '[')
            c = '(';
        else if (c == ']')
            c = ')';
        else if (c < 0x20 || c > 0x7e)
            c = ' ';
        text[n++] = (char)c;
    }
    while (n > 0 && text[n - 1] == ' ')
        n--;
    text[n] = '\0';
    snprintf(out, len, "%u %s%s%s", seq, ok ? "ok" : "fail", text[0] ? " " : "", text);
}

#ifndef MCODE_NO_THREAD
#include "super.h"

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void nap(double s)
{
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    nanosleep(&ts, NULL);
}

static void *relay(void *arg)
{
    char want[128], pushed[128] = "", reply[512], line[200], why[300];
    double pushed_at = 0;
    unsigned done = 0;
    (void)arg;
    for (;;) {
        double wait = MCODE_IDLE_S;
        if (!super_grbl_running()) {
            pushed[0] = '\0';               /* a controller starts knowing none */
            nap(wait);
            continue;
        }
        extpkg_mcode_table(want, sizeof(want));
        if (strcmp(want, pushed) != 0 || mono() - pushed_at >= MCODE_REPUSH_S) {
            if (grblport_request(GRBLPORT_SET_DAEMON, GRBLPORT_MCODES, want, reply, sizeof(reply)) == GRBLPORT_OK
                && strcmp(reply, "ok") == 0) {
                if (strcmp(want, pushed) != 0)
                    fflog(LOG_INFO, "mcode: the GRBL controller answers %s",
                          strcmp(want, "-") ? want : "no M-code of a package");
                snprintf(pushed, sizeof(pushed), "%s", want);
                pushed_at = mono();
            } else {
                pushed[0] = '\0';
            }
        }
        if (pushed[0] && strcmp(pushed, "-") != 0
            && grblport_request(GRBLPORT_SET_DAEMON, GRBLPORT_STATE, NULL, reply, sizeof(reply)) == GRBLPORT_OK) {
            json_t *st = json_loads(reply, 0, NULL), *m = json_object_get(st, "mcode");
            if (json_is_true(json_object_get(st, "sender")))
                wait = MCODE_POLL_S;
            if (json_is_object(m)) {
                unsigned seq = (unsigned)json_integer_value(json_object_get(m, "seq"));
                int code = (int)json_integer_value(json_object_get(m, "code"));
                char *words = json_dumps(json_object_get(m, "words"), JSON_COMPACT);
                if (seq && seq != done && words) {
                    fflog(LOG_INFO, "mcode: M%d (seq %u) goes to its extension", code, seq);
                    why[0] = '\0';
                    char *doc = extpkg_mcode_json(code, words, why, sizeof(why));
                    mcode_answer_line(seq, doc, why, line, sizeof(line));
                    free(doc);
                    int rc = grblport_request(GRBLPORT_SET_DAEMON, GRBLPORT_MCODE_RESULT, line, reply, sizeof(reply));
                    fflog(rc == GRBLPORT_OK && strcmp(reply, "ok") == 0 ? LOG_INFO : LOG_WARNING,
                          "mcode: M%d: told the GRBL controller %s (%s)", code, line, reply);
                    done = seq;
                }
                free(words);
                wait = 0.02;
            }
            json_decref(st);
        }
        nap(wait);
    }
    return NULL;
}

void mcode_init(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, relay, NULL) != 0)
        fflog(LOG_ERR, "mcode: the relay did not start: no M-code of a package is answered");
    pthread_attr_destroy(&a);
}
#endif
