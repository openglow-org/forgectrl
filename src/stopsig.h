/*
 * stopsig.h - the daemon's termination signals, and the wait for one
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_STOPSIG_H
#define FORGECTRL_STOPSIG_H

#include <sys/types.h>

/* Catch SIGINT and SIGTERM. Installed before any thread or child exists,
 * so a termination that lands during a slow init still ends in the
 * shutdown path. */
void stopsig_init(void);

/* Whether a termination signal has landed. */
int stopsig_requested(void);

/* Block until a termination signal has landed, on whatever thread its
 * handler ran, however close to this call it came: the handler writes an
 * eventfd this waits on, so no signal falls between a check and a wait. */
void stopsig_wait(void);

/* fork(), for every child the daemon makes; its return and errno are
 * fork()'s. A forked child starts with the daemon's handler, which would
 * take a SIGTERM or a SIGINT sent to the child before its exec and note it
 * in the child's memory: the program then starts as if never signaled. So
 * the forking thread blocks both across the fork, the parent puts its mask
 * back right after it, whether the fork made a child or failed, and the
 * child gives both their default action and unblocks them before it
 * returns here: a signal sent to it from its first instant on ends it,
 * before its exec or after. The child's part is async-signal-safe. No
 * other file in src/ calls fork() (tests/test_routes.py holds it so). */
pid_t stopsig_fork(void);

#endif
