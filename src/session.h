/*
 * session.h - panel login sessions (see session.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_SESSION_H
#define FORGECTRL_SESSION_H
#include <stddef.h>
#include <time.h>

#define SESSION_ID_HEX 64           /* 256 bits */
#define SESSION_IDLE_S (12 * 3600)  /* expiry, idle */
#define SESSION_COOKIE "ffsid"
#define LOGIN_FAILS 5               /* failures from one address ... */
#define LOGIN_LOCK_S 30             /* ... lock it for this long */

/* Create a session for name; the id lands in out (SESSION_ID_HEX + 1
 * bytes). Returns 0, or -1 without entropy or when the store is full
 * (the oldest is then evicted first, so full is rare). */
int session_create(const char *name, char *out, size_t len);
/* Look a session id up; refreshes its idle timer. Returns 1 when
 * valid, 0 otherwise. */
int session_valid(const char *id);
/* End one session, or every session (id NULL). */
void session_end(const char *id);
/* The session store: a root-only file the sessions survive a daemon
 * restart in. Reads the file back (less the expired sessions) in place
 * of what is in memory; NULL or "" keeps the sessions in memory only. */
void session_set_store(const char *path);
/* Parse the session id out of a Cookie header value into out. Returns
 * 1 when a well-formed id was present. */
int session_from_cookie(const char *cookie, char *out, size_t len);
/* The Set-Cookie value for a new session and for a cleared one. */
void session_cookie_set(const char *id, char *out, size_t len);
void session_cookie_clear(char *out, size_t len);
/* Login throttling per client address: is the address locked out; and
 * record a failure or a success. */
int  login_locked(const char *addr, int *seconds_left);
void login_failed(const char *addr);
void login_succeeded(const char *addr);
/* The test clock hook: the store reads time through this. */
void session_set_clock(time_t (*now)(void));
#endif
