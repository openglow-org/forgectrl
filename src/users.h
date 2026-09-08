/*
 * users.h - the panel account (see users.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_USERS_H
#define FORGECTRL_USERS_H
#include <stddef.h>

#define USERS_NAME_MAX 32
#define USERS_PASSWORD_MIN 8
#define USERS_PASSWORD_MAX 128

/* Load the record (users in the data directory) and the reset marker.
 * Idempotent; call once at startup. */
void users_init(void);
/* Whether an account exists and is not marked for re-creation. */
int users_exist(void);
/* The account name into buf; returns 0, or -1 when none. */
int users_name(char *buf, size_t len);
/* Validate a name (2 to 32 characters: lowercase letters, digits, '-',
 * '_', starting with a letter) and a password (8 to 128 bytes, not
 * equal to the name). Return 0, or -1 with reason filled. */
int users_check_name(const char *name, char *reason, size_t rlen);
int users_check_password(const char *name, const char *password,
                         char *reason, size_t rlen);
/* Create (or replace) the one account: hashes the password with
 * sha512-crypt, writes the record, clears the reset marker, and
 * replays the account into the system (users_replay). Returns 0, or
 * -1 with reason. The uid is 1000. */
int users_create(const char *name, const char *password, char *reason,
                 size_t rlen);
/* Verify a login. Returns 1 on a match, 0 otherwise; constant time
 * over the hash comparison. */
int users_verify(const char *name, const char *password);
/* Mark the account for re-creation (the button-hold reset): the
 * record stays until a new account replaces it, but users_exist()
 * reads false and the wizard's account step runs. */
int users_mark_reset(void);
int users_reset_pending(void);
/* Replay the record into the system accounts by running the same
 * script the boot runs (/etc/init.d/forgefirm-users). Best effort;
 * logs on failure. */
void users_replay(void);
/* The uid the account gets. */
#define USERS_UID 1000L
#endif
