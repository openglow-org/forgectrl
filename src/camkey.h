/*
 * camkey.h - the camera key (see camkey.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_CAMKEY_H
#define FORGECTRL_CAMKEY_H
#include <stddef.h>

#define CAMKEY_HEX 32               /* 128 bits */

/* Load or create the key in the data directory. Idempotent; call once
 * at startup. */
void camkey_init(void);
/* The key, or "" when none could be made. */
const char *camkey_get(void);
/* Replace the key with a new one; every URL that carried the old one
 * stops working. Returns 0, or -1 without entropy. */
int camkey_rotate(void);
/* Constant-time check of a presented key. */
int camkey_valid(const char *presented);
#endif
