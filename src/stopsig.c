/*
 * stopsig.c - the daemon's termination signals, and the wait for one
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A process-directed signal runs its handler on any thread that does not
 * block it, and pause() returns only on the thread the handler ran on; a
 * flag tested before pause() also loses a signal that lands between the
 * test and the call. The handler here sets the flag and writes an
 * eventfd, and the wait polls that descriptor: it returns on any thread's
 * delivery, and a signal that came before the poll leaves the descriptor
 * readable. Without the eventfd (it could not be made) the wait looks at
 * the flag once a second.
 */
#define _GNU_SOURCE
#include "stopsig.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static volatile sig_atomic_t requested;
static int efd = -1;

static void on_stop(int sig)
{
    (void)sig;
    requested = 1;
    if (efd >= 0) {
        uint64_t one = 1;
        (void)!write(efd, &one, sizeof(one));
    }
}

void stopsig_init(void)
{
    if (efd < 0)
        efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int stopsig_requested(void)
{
    return requested != 0;
}

static void stop_set(sigset_t *s)
{
    sigemptyset(s);
    sigaddset(s, SIGTERM);
    sigaddset(s, SIGINT);
}

pid_t stopsig_fork(void)
{
    sigset_t s, was;
    stop_set(&s);
    pthread_sigmask(SIG_BLOCK, &s, &was);
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        pthread_sigmask(SIG_UNBLOCK, &s, NULL);
        return 0;
    }
    int err = errno;
    pthread_sigmask(SIG_SETMASK, &was, NULL);
    errno = err;
    return pid;
}

void stopsig_wait(void)
{
    while (!requested) {
        struct pollfd p = { .fd = efd, .events = POLLIN };
        if (poll(&p, 1, efd >= 0 ? -1 : 1000) > 0) {
            uint64_t n;
            (void)!read(efd, &n, sizeof(n));
        }
    }
}
