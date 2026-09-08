/*
 * jobstream.c - the daemon's own Grbl sender with a witness
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See jobstream.h. The loop ticks at 25 Hz: it harvests the
 * controller's answers, feeds the next line when the last one is
 * acknowledged, samples the witnesses, and judges the end. Because a
 * new connection displaces the sender (last connection wins), a run
 * should be refused by its caller while jobstream_sender_connected()
 * says a sender is on the socket; in the wizard's loopback posture no
 * sender can be.
 */
#define _GNU_SOURCE
#include "jobstream.h"
#include "fflog.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char *sysfs_root(void)
{
    const char *r = getenv("GF_SYSFS_ROOT");
    return r && *r ? r : "/sys/glowforge";
}

static int grbl_port(void)
{
    const char *p = getenv("GF_GRBL_PORT");
    int port = p && *p ? atoi(p) : 23;
    return port > 0 && port < 65536 ? port : 23;
}

int jobstream_sender_connected(void)
{
    const char *d = getenv("GF_RUN_DIR");
    char path[192], body[512];
    snprintf(path, sizeof(path), "%s/grbl.state", d && *d ? d : "/run/forgefirm");
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(body, 1, sizeof(body) - 1, f);
    fclose(f);
    body[n] = '\0';
    if (strstr(body, "\"connected\":true"))
        return 1;
    if (strstr(body, "\"connected\":false"))
        return 0;
    return -1;
}

static long rd_long(const char *attr, long fallback)
{
    char path[192], text[32];
    snprintf(path, sizeof(path), "%s/%s", sysfs_root(), attr);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return fallback;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return fallback;
    text[n] = '\0';
    return strtol(text, NULL, 10);
}

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int connect_loopback(void)
{
    int port = grbl_port();
    int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        struct sockaddr_in6 sa = { .sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port),
                                   .sin6_addr = IN6ADDR_LOOPBACK_INIT };
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
            return fd;
        close(fd);
    }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int jobstream_lines_gen(void *ctx, char *buf, size_t len)
{
    jobstream_lines_t *l = ctx;
    if (!l->text)
        return 0;
    const char *p = l->text + l->off;
    while (*p == '\n' || *p == '\r')
        p++;
    if (!*p)
        return 0;
    const char *nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    if (n >= len)
        n = len - 1;
    memcpy(buf, p, n);
    buf[n] = '\0';
    l->off = (size_t)(p - l->text) + n;
    return 1;
}

#define JOBSTREAM_INFLIGHT_BYTES 512    /* half the controller's RX ring */
#define JOBSTREAM_INFLIGHT_LINES 32

/* A line the controller takes out of the ordinary stream: a $ command
 * (refused unless idle), M102 (the laser keys reload, which must see
 * every earlier line's settings write applied in order), the program
 * end. */
static int line_is_barrier(const char *l)
{
    return l[0] == '$' || !strncmp(l, "M102", 4) ||
           (l[0] == 'M' && l[1] == '2' && (l[2] == '\0' || l[2] == ' '));
}

int jobstream_run(const jobstream_cfg_t *cfg, jobstream_run_t *run, char *err, size_t elen)
{
    struct timespec tick = { 0, (long)(1e9 / JOBSTREAM_HZ) };
    char rx[512], line[256];
    size_t rxn = 0;
    int done_sending = 0, dark_run = 0, rc = -1;
    /* The lines out and not yet acknowledged, by sequence number: the
     * controller's RX ring is 1024 bytes and half of it stays free, so
     * the planner is never starved on short segments and the ring can
     * never overrun. */
    char sent_text[JOBSTREAM_INFLIGHT_LINES][64];
    size_t sent_len[JOBSTREAM_INFLIGHT_LINES], inflight = 0;
    int barrier = 0, have_held = 0;
    memset(run, 0, sizeof(*run));
    run->tp_base = -1;

    int fd = connect_loopback();
    if (fd < 0) {
        snprintf(err, elen, "cannot reach the controller on the Grbl socket");
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    double t0 = mono_s(), t_last_ack = t0;
    for (;;) {
        double now = mono_s(), t = now - t0;

        /* The witnesses. */
        jobstream_sample_t s;
        s.t = t;
        s.hv = rd_long("pic/hv_current", 0);
        s.tp = rd_long("head/beam_detect_analog", 0);
        s.lon = rd_long("cnc/laser_on_sampled", 0);
        s.ir[0] = rd_long("pic/lid_ir_1", 0);
        s.ir[1] = rd_long("pic/lid_ir_2", 0);
        s.ir[2] = rd_long("pic/lid_ir_3", 0);
        s.ir[3] = rd_long("pic/lid_ir_4", 0);
        run->samples++;
        if (s.hv > run->hv_max)
            run->hv_max = s.hv;
        if (s.lon > run->lon_max)
            run->lon_max = s.lon;
        if (s.hv > JOBSTREAM_HV_ON) {
            if (!run->lit) {
                run->lit = 1;
                run->t_first_lit = t;
                fflog(LOG_INFO, "jobstream: the tube is lit");
            }
            run->t_last_lit = t;
            dark_run = 0;
            if (s.tp > run->tp_max)
                run->tp_max = s.tp;
        } else {
            if (run->lit)
                dark_run++;
            if (run->tp_base < 0 && !run->lit)
                run->tp_base = s.tp;
        }
        if (cfg->sample)
            cfg->sample(cfg->ctx, &s);

        /* The controller's answers. */
        if (!done_sending || run->acked < run->sent) {
            ssize_t r;
            while ((r = read(fd, rx + rxn, sizeof(rx) - 1 - rxn)) > 0) {
                rxn += (size_t)r;
                rx[rxn] = '\0';
                char *nl;
                while ((nl = memchr(rx, '\n', rxn))) {
                    *nl = '\0';
                    char *l = rx;
                    while (*l == '\r' || *l == ' ')
                        l++;
                    if (!strncmp(l, "ok", 2)) {
                        run->acked++;
                        inflight -= sent_len[(run->acked - 1) % JOBSTREAM_INFLIGHT_LINES];
                        if (run->acked >= run->sent)
                            barrier = 0;
                        t_last_ack = now;
                    } else if (!strncmp(l, "error", 5) || !strncmp(l, "ALARM", 5)) {
                        snprintf(err, elen, "the controller answered %.24s on line %d (%s)",
                                 l, run->acked + 1,
                                 run->acked < run->sent ? sent_text[run->acked % JOBSTREAM_INFLIGHT_LINES] : "");
                        goto fail;
                    }
                    size_t rest = rxn - (size_t)(nl + 1 - rx);
                    memmove(rx, nl + 1, rest);
                    rxn = rest;
                }
                if (rxn >= sizeof(rx) - 1)
                    rxn = 0;            /* a status frame overran: drop it */
            }
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                snprintf(err, elen, "the controller closed the connection (a sender took the socket?)");
                goto fail;
            }
        }

        /* The next lines, as many as the ring takes. A line the controller
         * handles out of band (M102, a $ setting, the program end) is a
         * barrier: sent alone once every earlier line is acknowledged,
         * and followed by nothing until it is. */
        while (!done_sending && !barrier) {
            if (!have_held) {
                int g = cfg->gen(cfg->ctx, line, sizeof(line) - 2);
                if (g < 0) {
                    snprintf(err, elen, "%s", line);
                    goto fail;
                }
                if (g == 2)
                    break;              /* nothing yet: the generator is pacing itself */
                if (g == 0) {
                    done_sending = 1;
                    fflog(LOG_INFO, "jobstream: the program streamed to the end (%d lines)", run->sent);
                    break;
                }
                have_held = 1;
            }
            size_t n = strlen(line);
            int is_barrier = line_is_barrier(line);
            if (is_barrier && run->acked < run->sent)
                break;
            if (!is_barrier && (inflight + n + 1 > JOBSTREAM_INFLIGHT_BYTES ||
                                run->sent - run->acked >= JOBSTREAM_INFLIGHT_LINES))
                break;
            snprintf(sent_text[run->sent % JOBSTREAM_INFLIGHT_LINES], 64, "%.60s", line);
            sent_len[run->sent % JOBSTREAM_INFLIGHT_LINES] = n + 1;
            line[n++] = '\n';
            if (write(fd, line, n) != (ssize_t)n) {
                snprintf(err, elen, "cannot write to the controller");
                goto fail;
            }
            inflight += n;
            run->sent++;
            have_held = 0;
            if (is_barrier)
                barrier = 1;
        }

        if (cfg->abort_flag && *cfg->abort_flag) {
            snprintf(err, elen, "aborted");
            goto fail;
        }
        /* The end. A program's last line is M2, which the controller
         * acknowledges only once every buffered motion has played, so
         * every line acknowledged means the job is over. A program that
         * expects an emission must have shown one; the caller may ask
         * for a stretch of dark after the last discharge on top. */
        int all_acked = done_sending && run->acked >= run->sent;
        if (all_acked) {
            if (cfg->wait_timeout_s <= 0) {
                rc = 0;
                break;
            }
            if (!run->lit) {
                if (now - t_last_ack > 2.0) {
                    snprintf(err, elen, "the program ended without a discharge");
                    goto fail;
                }
            } else if (cfg->end_dark_s <= 0 || dark_run >= (int)(cfg->end_dark_s * JOBSTREAM_HZ)) {
                rc = 0;
                break;
            }
        }
        if (cfg->wait_timeout_s > 0 && !run->lit && t > cfg->wait_timeout_s) {
            snprintf(err, elen, "no discharge seen: the press never came");
            goto fail;
        }
        if (cfg->run_timeout_s > 0 && t > cfg->run_timeout_s) {
            snprintf(err, elen, "the run did not finish within %.0f s", cfg->run_timeout_s);
            goto fail;
        }
        nanosleep(&tick, NULL);
    }
    close(fd);
    return rc;

fail:
    /* A controlled stop and the latch relocked, whatever the state. */
    if (write(fd, "\x18", 1) != 1)
        fflog(LOG_WARNING, "jobstream: could not send the soft reset");
    close(fd);
    fflog(LOG_WARNING, "jobstream: %s", err);
    return -1;
}
