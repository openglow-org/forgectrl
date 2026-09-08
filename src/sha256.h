/*
 * sha256.h - SHA-256, HMAC-SHA256, and constant-time comparison
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A small, dependency-free implementation for the daemon's own needs:
 * the commissioning sheet id (an HMAC of the serial under a secret
 * salt), the hash of each advisory document the record stores, and
 * the session id comparison. The vectors in tests/sha256_test.c pin it
 * to FIPS 180-4 and RFC 4231.
 */
#ifndef FORGECTRL_SHA256_H
#define FORGECTRL_SHA256_H
#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN 32

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    unsigned char block[64];
    size_t fill;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, unsigned char out[SHA256_LEN]);
/* One-shot digest of a buffer. */
void sha256(const void *data, size_t len, unsigned char out[SHA256_LEN]);
/* HMAC-SHA256 with a key of any length. */
void hmac_sha256(const void *key, size_t klen, const void *data, size_t len,
                 unsigned char out[SHA256_LEN]);
/* Lowercase hex of a digest into out (2*len + 1 bytes). */
void sha256_hex(const unsigned char *digest, size_t len, char *out);
/* Constant-time equality of two buffers of the same length: 1 if equal. */
int ct_equal(const void *a, const void *b, size_t len);
#endif
