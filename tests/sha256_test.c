/*
 * sha256_test.c - host test: the digest, the HMAC, and the comparison
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Vectors: FIPS 180-4 (the empty string, "abc", the 56-byte and the
 * million-'a' messages) and RFC 4231 (HMAC cases 1, 2, and 6, the last
 * with a key longer than one block).
 */
#include "../src/sha256.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check_hex(const char *name, const unsigned char *d, const char *want)
{
    char got[65];
    sha256_hex(d, 32, got);
    if (strcmp(got, want)) {
        printf("FAIL %s\n  got  %s\n  want %s\n", name, got, want);
        failures++;
    } else
        printf("ok   %s\n", name);
}

int main(void)
{
    unsigned char d[32];

    sha256("", 0, d);
    check_hex("sha256 empty", d,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha256("abc", 3, d);
    check_hex("sha256 abc", d,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    check_hex("sha256 448-bit", d,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* One million 'a', fed in odd-sized pieces to exercise the block
     * boundary handling. */
    sha256_ctx c;
    sha256_init(&c);
    unsigned char chunk[97];
    memset(chunk, 'a', sizeof(chunk));
    size_t left = 1000000;
    while (left) {
        size_t n = left < sizeof(chunk) ? left : sizeof(chunk);
        sha256_update(&c, chunk, n);
        left -= n;
    }
    sha256_final(&c, d);
    check_hex("sha256 million a", d,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    /* RFC 4231 case 1 */
    unsigned char k1[20];
    memset(k1, 0x0b, sizeof(k1));
    hmac_sha256(k1, sizeof(k1), "Hi There", 8, d);
    check_hex("hmac case 1", d,
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    /* case 2: key "Jefe" */
    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    check_hex("hmac case 2", d,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    /* case 6: a 131-byte key (longer than a block) */
    unsigned char k6[131];
    memset(k6, 0xaa, sizeof(k6));
    hmac_sha256(k6, sizeof(k6),
                "Test Using Larger Than Block-Size Key - Hash Key First", 54, d);
    check_hex("hmac case 6", d,
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    /* ct_equal */
    if (!ct_equal("abcdef", "abcdef", 6) || ct_equal("abcdef", "abcdeg", 6) ||
        ct_equal("abcdef", "Abcdef", 6)) {
        printf("FAIL ct_equal\n");
        failures++;
    } else
        printf("ok   ct_equal\n");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
