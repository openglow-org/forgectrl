/*
 * grblport.c - the daemon's client of the GRBL controller's port
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See grblport.h. The daemon is thread-per-connection, so requests are
 * serialized under one lock around one connection. A connection that
 * fails is dropped and tried again once: a controller that restarted
 * left a dead socket behind, and the first request after it should not
 * be the one that pays.
 *
 * The controller answers one request at a time, in order, and the
 * answer to a line it injected (a jog, a release, a home) goes to
 * whichever client holds the port when the line ends, so a request this
 * client gave up on can answer on its next connection. Such an answer is
 * never the state's JSON (the state is answered at once, never
 * injected), so every new connection begins with a fence: a state
 * request, and every line before its JSON read and dropped. Only then
 * does the request go out, and its answer is its own.
 *
 * A jog's cancel does not wait behind another request: when the lock is
 * taken it cuts the connection instead, which the controller takes as
 * the cancel of the port's jog (its client is the jog's dead-man), and
 * the request it cut is not sent again.
 */
#define _GNU_SOURCE
#include "grblport.h"
#include "paths.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SOCK_NAME "grbl.ctl"

/* The slowest answer is the release's: it waits up to 3 s for the kernel
 * to finish a move's tail before it writes the currents. The host test
 * builds with a shorter wait. */
#ifndef GRBLPORT_REPLY_TIMEOUT_MS
#define GRBLPORT_REPLY_TIMEOUT_MS 6000
#endif

static const struct {
    const char *word;
    grblport_set_t set;         /* the least set that may use it */
    int takes_arg;
} ops[] = {
    [GRBLPORT_STATE]    = { "state",    GRBLPORT_SET_PACKAGE, 0 },
    [GRBLPORT_JOG]      = { "jog",      GRBLPORT_SET_PACKAGE, 1 },
    [GRBLPORT_CANCEL]   = { "cancel",   GRBLPORT_SET_PACKAGE, 0 },
    [GRBLPORT_RELEASE]  = { "release",  GRBLPORT_SET_PANEL,   0 },
    [GRBLPORT_ENERGIZE] = { "energize", GRBLPORT_SET_PANEL,   0 },
    [GRBLPORT_HOME]     = { "home",     GRBLPORT_SET_PANEL,   0 },
    [GRBLPORT_ENVELOPE] = { "envelope", GRBLPORT_SET_PANEL,   1 },
    [GRBLPORT_TRAY]     = { "tray",     GRBLPORT_SET_PANEL,   1 },
    [GRBLPORT_MCODES]   = { "mcodes",   GRBLPORT_SET_DAEMON,  1 },
    [GRBLPORT_MCODE_RESULT] = { "mcode_result", GRBLPORT_SET_DAEMON, 1 },
    [GRBLPORT_SENDER]   = { "sender",   GRBLPORT_SET_DAEMON,  1 },
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fd_mu = PTHREAD_MUTEX_INITIALIZER;   /* port_fd and cut, for the cancel */
static int port_fd = -1;
static int cut;                         /* a cancel cut the connection under the request */

static void drop(void)
{
    pthread_mutex_lock(&fd_mu);
    if (port_fd >= 0) {
        close(port_fd);
        port_fd = -1;
    }
    pthread_mutex_unlock(&fd_mu);
}

static int port_connect(void)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/" SOCK_NAME, ff_run_dir()) >=
        (int)sizeof(sa.sun_path))
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    pthread_mutex_lock(&fd_mu);
    port_fd = fd;
    pthread_mutex_unlock(&fd_mu);
    return 0;
}

/* One line back, within the reply timeout counted from t0. 0, or -1:
 * the connection is no good (it may have died with a controller that
 * has since restarted), -2: no whole line in time, -3: a whole line that
 * does not fit len (read to its end, so the next line is still a line). */
static int read_line(const struct timespec *t0, char *reply, size_t len)
{
    struct timespec now;
    size_t got = 0;
    int over = 0;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long left = GRBLPORT_REPLY_TIMEOUT_MS -
                    ((now.tv_sec - t0->tv_sec) * 1000 + (now.tv_nsec - t0->tv_nsec) / 1000000);
        if (left <= 0)
            return -2;
        struct pollfd p = { .fd = port_fd, .events = POLLIN };
        int r = poll(&p, 1, (int)left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return r == 0 ? -2 : -1;
        char c;
        ssize_t k = read(port_fd, &c, 1);
        if (k == 1) {
            if (c == '\n') {
                reply[got] = '\0';
                return over ? -3 : 0;
            }
            if (c == '\r')
                continue;
            if (got < len - 1)
                reply[got++] = c;
            else
                over = 1;
        } else if (k == 0 || (errno != EINTR && errno != EAGAIN))
            return got || over ? -2 : -1;   /* hung up: before any answer, or in the middle of one */
    }
}

/* One line out, one line back: read_line()'s codes. */
static int exchange(const char *line, size_t n, char *reply, size_t len)
{
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (send(port_fd, line, n, MSG_NOSIGNAL) != (ssize_t)n)
        return -1;
    return read_line(&t0, reply, len);
}

/* A new connection's fence: a state request, and every line before its
 * JSON dropped (an answer owed to a request given up on). read_line()'s
 * codes, but for -3, which drops the line as any other. */
static int fence(void)
{
    char line[512];
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (send(port_fd, "state\n", 6, MSG_NOSIGNAL) != 6)
        return -1;
    for (;;) {
        int x = read_line(&t0, line, sizeof(line));
        if (x == 0 && line[0] == '{')
            return 0;
        if (x == -1 || x == -2)
            return x;
    }
}

void (*grblport_alarm_touched)(void);

int grblport_request(grblport_set_t set, grblport_op_t op, const char *arg,
                     char *reply, size_t len)
{
    char line[160];

    if (len)
        reply[0] = '\0';
    if ((unsigned)op >= sizeof(ops) / sizeof(ops[0]) || set < ops[op].set) {
        snprintf(reply, len, "the operation is not open to this caller");
        return GRBLPORT_FORBIDDEN;
    }
    int n = ops[op].takes_arg ? snprintf(line, sizeof(line), "%s %s\n", ops[op].word, arg ? arg : "")
                              : snprintf(line, sizeof(line), "%s\n", ops[op].word);
    /* One request is one line: an argument that carries a line break
     * would be a second request, and that one would have passed no set
     * check at all. */
    if (n < 0 || n >= (int)sizeof(line) || (ops[op].takes_arg && (!arg || strpbrk(arg, "\r\n")))) {
        snprintf(reply, len, "the request is not one line");
        return GRBLPORT_FORBIDDEN;
    }

    /* A home, a release or an energize clears or changes an alarm with no
     * sender session: told before it goes out, whatever the answer. */
    if ((op == GRBLPORT_HOME || op == GRBLPORT_RELEASE || op == GRBLPORT_ENERGIZE) && grblport_alarm_touched)
        grblport_alarm_touched();

    int rc = GRBLPORT_UNREACHABLE;
    if (op == GRBLPORT_CANCEL && pthread_mutex_trylock(&lock) != 0) {
        /* Another request has the port: the cancel cuts the connection
         * under it rather than wait (the port's client is its jog's
         * dead-man), and that request is not sent again. */
        pthread_mutex_lock(&fd_mu);
        if (port_fd >= 0) {
            shutdown(port_fd, SHUT_RDWR);
            cut = 1;
        }
        pthread_mutex_unlock(&fd_mu);
        snprintf(reply, len, "ok");
        return GRBLPORT_OK;
    } else if (op != GRBLPORT_CANCEL)
        pthread_mutex_lock(&lock);
    /* Two tries, and only a kept connection that turns out dead earns the
     * second: a request the port answered late, or not at all, is never
     * sent again, nor one a cancel cut. */
    for (int attempt = 0; attempt < 2; attempt++) {
        int fresh = port_fd < 0;
        if (fresh && port_connect() != 0) {
            snprintf(reply, len, "the GRBL controller is not on its port");
            break;
        }
        int x = fresh ? fence() : 0;
        if (x == 0)
            x = exchange(line, (size_t)n, reply, len);
        if (x == 0 && op == GRBLPORT_STATE && (reply[0] != '{' || reply[strlen(reply) - 1] != '}')) {
            /* The state is one JSON object on one line, or it is no state. */
            snprintf(reply, len, "the GRBL controller's state was not understood");
            break;
        }
        if (x == 0) {
            rc = GRBLPORT_OK;
            break;
        }
        if (x == -3) {
            snprintf(reply, len, "the GRBL controller's answer does not fit");
            break;
        }
        drop();
        pthread_mutex_lock(&fd_mu);
        int was_cut = cut;
        cut = 0;
        pthread_mutex_unlock(&fd_mu);
        if (was_cut) {
            snprintf(reply, len, "a jog's cancel cut the request short");
            break;
        }
        if (x == -2 || fresh) {
            snprintf(reply, len, x == -2 ? "the GRBL controller did not answer"
                                         : "the GRBL controller closed its port");
            break;
        }
    }
    pthread_mutex_lock(&fd_mu);
    cut = 0;
    pthread_mutex_unlock(&fd_mu);
    pthread_mutex_unlock(&lock);
    return rc;
}

void grblport_close(void)
{
    pthread_mutex_lock(&lock);
    drop();
    pthread_mutex_unlock(&lock);
}

int grblport_jog_words(double dx, double dy, double dz, double feed,
                       char *buf, size_t len, char *err, size_t elen)
{
    if (!isfinite(dx) || !isfinite(dy) || !isfinite(dz) || !isfinite(feed)) {
        snprintf(err, elen, "x, y, z, and feed must be numbers");
        return -1;
    }
    if (fabs(dx) > GRBLPORT_JOG_MAX_XY_MM || fabs(dy) > GRBLPORT_JOG_MAX_XY_MM) {
        snprintf(err, elen, "one jog moves X and Y at most %.0f mm", GRBLPORT_JOG_MAX_XY_MM);
        return -1;
    }
    if (fabs(dz) > GRBLPORT_JOG_MAX_Z_MM) {
        snprintf(err, elen, "one jog moves Z at most %.0f mm", GRBLPORT_JOG_MAX_Z_MM);
        return -1;
    }
    if (feed < GRBLPORT_JOG_FEED_MIN || feed > GRBLPORT_JOG_FEED_MAX) {
        snprintf(err, elen, "feed must be %.0f to %.0f mm/min",
                 GRBLPORT_JOG_FEED_MIN, GRBLPORT_JOG_FEED_MAX);
        return -1;
    }

    size_t off = 0;
    int k = snprintf(buf, len, "G91 G21");
    if (k < 0 || (size_t)k >= len)
        goto toolong;
    off = (size_t)k;
    const double inc[3] = { dx, dy, dz };
    int moved = 0;
    for (int i = 0; i < 3; i++) {
        /* Below the port's three decimals an increment is no move. */
        if (fabs(inc[i]) < 0.0005)
            continue;
        k = snprintf(buf + off, len - off, " %c%.3f", "XYZ"[i], inc[i]);
        if (k < 0 || (size_t)k >= len - off)
            goto toolong;
        off += (size_t)k;
        moved = 1;
    }
    if (!moved) {
        snprintf(err, elen, "a jog needs an x, y, or z increment");
        return -1;
    }
    k = snprintf(buf + off, len - off, " F%.0f", feed);
    if (k < 0 || (size_t)k >= len - off)
        goto toolong;
    return 0;

toolong:
    snprintf(err, elen, "the jog does not fit a line");
    return -1;
}

int grblport_explain(const char *reply, char *why, size_t len)
{
    if (!strcmp(reply, "ok"))
        return 0;
    if (!strcmp(reply, "busy:released"))
        snprintf(why, len, "the X and Y motors are released: energize them first");
    else if (!strcmp(reply, "busy:sender"))
        snprintf(why, len, "the Grbl sender is using the machine: it goes first");
    else if (!strcmp(reply, "busy:state"))
        snprintf(why, len, "the controller is busy (a program, a hold, an alarm, or the sender's own jog)");
    else if (!strcmp(reply, "busy:mcode"))
        snprintf(why, len, "a job waits at an extension's M-code");
    else if (!strcmp(reply, "busy:armed"))
        snprintf(why, len, "the laser's armed window is open: no jog until the job ends");
    else if (!strcmp(reply, "error:saved"))
        snprintf(why, len, "the controller could not save the tray mode: nothing changed");
    else if (!strcmp(reply, "error:mode"))
        snprintf(why, len, "the homing method is not manual: a home is the Grbl sender's to start");
    else if (!strcmp(reply, "error:15"))
        snprintf(why, len, "the move would leave the work envelope");
    else if (!strcmp(reply, "error:9"))
        snprintf(why, len, "the controller is in an alarm: home the machine, or clear the alarm, first");
    else if (!strcmp(reply, "error:aborted"))
        snprintf(why, len, "the controller was reset before it answered");
    else if (!strncmp(reply, "error:", 6) || !strncmp(reply, "busy:", 5))
        snprintf(why, len, "the controller refused (%.32s)", reply);
    else {
        snprintf(why, len, "the controller's answer was not understood");
        return 502;
    }
    return 409;
}
