/*
 * stopsig_test.c - host unit test for the daemon's termination wait
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The daemon's main thread waits for SIGTERM while other threads run. A
 * SIGTERM whose handler runs on another thread ends the wait: the main
 * thread here blocks the signal, so the kernel can deliver it only to a
 * second thread, and the wait must return all the same. A wait that
 * returns only on its own thread's delivery (pause) never does, and the
 * alarm ends the test as a failure.
 */
#define _GNU_SOURCE
#include "../src/stopsig.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static void *terminator(void *arg)
{
    (void)arg;
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGTERM);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    kill(getpid(), SIGTERM);
    return NULL;
}

int main(void)
{
    stopsig_init();
    printf("a termination handled on another thread ends the wait\n");
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &s, NULL);
    pthread_t t;
    pthread_create(&t, NULL, terminator, NULL);
    alarm(3);                   /* a wait that never returns fails here */
    stopsig_wait();
    alarm(0);
    pthread_join(t, NULL);
    int ok = stopsig_requested();
    printf(ok ? "  ok: the wait returned, the termination noted\n"
              : "  FAIL: the wait returned with no termination noted\n");
    printf(ok ? "stopsig_test: all ok\n" : "stopsig_test: 1 FAILED\n");
    return ok ? 0 : 1;
}
