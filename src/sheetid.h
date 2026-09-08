/*
 * sheetid.h - the commissioning sheet id (see sheetid.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_SHEETID_H
#define FORGECTRL_SHEETID_H
#include <stddef.h>

#define SHEETID_LEN 11              /* "XXXXX-XXXXX" */
#define SHEETID_SALT_LEN 32

/* Load or create the per-machine secret salt in the data directory.
 * Idempotent; call once at startup. */
void sheetid_init(void);
/* The machine's sheet id into out (SHEETID_LEN + 1 bytes). Returns 0,
 * or -1 when the salt or the serial is unavailable (out is emptied). */
int sheetid_get(char *out, size_t len);
/* The pure computation, for the tests: an HMAC-SHA256 of the text under
 * the salt, base32 (RFC 4648, no padding), the first ten characters
 * split 5-5. */
void sheetid_compute(const unsigned char *salt, size_t slen,
                     const char *text, char *out, size_t len);
/* The same derivation for another identifying value (the head serial
 * in the record), so no identifying number is stored in the clear. */
int sheetid_derive(const char *text, char *out, size_t len);
#endif
