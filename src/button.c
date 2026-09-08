/*
 * button.c - the physical button as an edge
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The advisories step ends with a press of the machine's button: the
 * one act that proves a person is at the machine, which no network
 * client can fake. This module waits for the release-to-press edge on
 * EV_SW bit 2 by polling the switch state (no grab, so the controller
 * is undisturbed; no controller runs during the wait in any case). A
 * button already held when the wait starts does not count: the wait
 * needs a release first, so a wedged button cannot accept anything.
 */
#define _GNU_SOURCE
#include "button.h"

#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define SW_BIT_BUTTON 2
#define POLL_MS 40

enum { B_IDLE, B_WAITING, B_PRESSED, B_TIMEOUT, B_CANCELLED };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int state = B_IDLE;
static volatile int cancel;
static pthread_t th;
static int th_live;

static const char *switch_dev(void)
{
    const char *d = getenv("GF_SWITCH_DEV");
    return d && *d ? d : "/dev/input/event0";
}

/* -1 unreadable, else 0/1. */
static int read_button(void)
{
    uint8_t sw[2] = { 0 };
    int fd = open(switch_dev(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    int ok = ioctl(fd, EVIOCGSW(sizeof(sw)), sw) >= 0;
    close(fd);
    if (!ok)
        return -1;
    return (sw[SW_BIT_BUTTON / 8] >> (SW_BIT_BUTTON % 8)) & 1;
}

int button_held(void)
{
    return read_button() == 1;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void *wait_thread(void *arg)
{
    int timeout_s = (int)(long)arg;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int released = read_button() == 0;
    int result = B_TIMEOUT;
    for (;;) {
        if (cancel) {
            result = B_CANCELLED;
            break;
        }
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec >= timeout_s)
            break;
        int b = read_button();
        if (b == 0)
            released = 1;
        else if (b == 1 && released) {
            /* Debounce: still held two polls later. */
            sleep_ms(POLL_MS);
            if (read_button() == 1) {
                result = B_PRESSED;
                break;
            }
        }
        sleep_ms(POLL_MS);
    }
    pthread_mutex_lock(&mu);
    state = result;
    pthread_mutex_unlock(&mu);
    return NULL;
}

int button_wait_start(int timeout_s)
{
    pthread_mutex_lock(&mu);
    if (state == B_WAITING) {
        pthread_mutex_unlock(&mu);
        return -1;
    }
    if (th_live) {
        pthread_join(th, NULL);
        th_live = 0;
    }
    cancel = 0;
    state = B_WAITING;
    if (pthread_create(&th, NULL, wait_thread, (void *)(long)timeout_s) != 0) {
        state = B_IDLE;
        pthread_mutex_unlock(&mu);
        return -1;
    }
    th_live = 1;
    pthread_mutex_unlock(&mu);
    return 0;
}

void button_wait_cancel(void)
{
    pthread_mutex_lock(&mu);
    int live = state == B_WAITING;
    pthread_mutex_unlock(&mu);
    if (!live)
        return;
    cancel = 1;
    pthread_join(th, NULL);
    th_live = 0;
}

const char *button_state(void)
{
    pthread_mutex_lock(&mu);
    int s = state;
    pthread_mutex_unlock(&mu);
    switch (s) {
    case B_WAITING:   return "waiting";
    case B_PRESSED:   return "pressed";
    case B_TIMEOUT:   return "timeout";
    case B_CANCELLED: return "cancelled";
    default:          return "idle";
    }
}

int button_take_pressed(void)
{
    pthread_mutex_lock(&mu);
    int p = state == B_PRESSED;
    if (p)
        state = B_IDLE;
    pthread_mutex_unlock(&mu);
    return p;
}

int button_held_for(int hold_s)
{
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (read_button() != 1)
            return 0;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec >= hold_s)
            return 1;
        sleep_ms(100);
    }
}
