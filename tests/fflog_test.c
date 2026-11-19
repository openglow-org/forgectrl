/*
 * fflog_test.c - host unit test for the log emitter's socket
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * fflog.c with its connect and send faked: a line goes out on the socket
 * the emitter holds, and the send runs under the lock a reconnect closes
 * that socket under, so no other thread can be handed its number between
 * the read and the send. Then the peer goes away: the send fails, the
 * emitter reconnects under the same lock and sends again.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int fake_connect(int fd, const struct sockaddr *sa, socklen_t len);
ssize_t fake_send(int fd, const void *buf, size_t len, int flags);
#define connect fake_connect
#define send fake_send

#include "../src/fflog.c"

#undef connect
#undef send

#include <errno.h>

static int sends, unlocked_sends, wrong_fd, gone_once;

int fake_connect(int fd, const struct sockaddr *sa, socklen_t len)
{
    (void)fd;
    (void)sa;
    (void)len;
    return 0;
}

ssize_t fake_send(int fd, const void *buf, size_t len, int flags)
{
    (void)buf;
    (void)flags;
    sends++;
    /* The lock is held here, by this thread: a try finds it taken. */
    if (pthread_mutex_trylock(&mu) == 0) {
        unlocked_sends++;
        pthread_mutex_unlock(&mu);
    }
    if (fd != sock)
        wrong_fd++;
    if (gone_once) {
        gone_once = 0;
        errno = ECONNREFUSED;
        return -1;
    }
    return (ssize_t)len;
}

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    setenv("FFLOG_CONF", "/nonexistent", 1);
    fflog_init("fflog_test");
    printf("the send runs on the held socket, under its lock\n");
    fflog(LOG_ERR, "a line");
    CHECK(sends == 1 && unlocked_sends == 0 && wrong_fd == 0, "one send, under the lock, on the socket held");
    gone_once = 1;
    fflog(LOG_ERR, "a line after the peer went away");
    CHECK(sends == 3 && unlocked_sends == 0 && wrong_fd == 0,
          "a send the peer refused: reconnected and sent again, every send under the lock");
    printf(failures ? "fflog_test: %d FAILED\n" : "fflog_test: all ok\n", failures);
    return failures ? 1 : 0;
}
