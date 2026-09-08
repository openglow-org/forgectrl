/*
 * sheetid_test.c - host test: the commissioning sheet id
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The golden ids come from an independent implementation (Python's
 * hmac and base64.b32encode) with the salt 0x00..0x1f.
 */
#include "../src/sheetid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned long serial_value = 123456789;
unsigned long fuse_serial(void) { return serial_value; }

static int failures;

static void expect(const char *name, const char *got, const char *want)
{
    if (strcmp(got, want)) {
        printf("FAIL %s: got %s want %s\n", name, got, want);
        failures++;
    } else
        printf("ok   %s: %s\n", name, got);
}

int main(void)
{
    unsigned char salt[32];
    for (int i = 0; i < 32; i++)
        salt[i] = (unsigned char)i;
    char id[SHEETID_LEN + 1];

    sheetid_compute(salt, sizeof(salt), "123456789", id, sizeof(id));
    expect("fixed salt, serial 123456789", id, "UL5UU-LS5PI");
    sheetid_compute(salt, sizeof(salt), "987654321", id, sizeof(id));
    expect("fixed salt, serial 987654321", id, "K6JXB-KMRVF");
    sheetid_compute(salt, sizeof(salt), "1", id, sizeof(id));
    expect("fixed salt, serial 1", id, "O5Q3D-TBFEJ");

    /* The id never contains the serial's digits as a run. */
    if (strstr(id, "123456789")) {
        printf("FAIL the id leaks the serial\n");
        failures++;
    }

    /* A live salt: made once in a scratch directory, stable across a
     * second init, and different ids for different serials. */
    char dir[] = "/tmp/sheetid-test-XXXXXX";
    if (!mkdtemp(dir)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    setenv("FORGECTRL_DATA_DIR", dir, 1);
    sheetid_init();
    char a[SHEETID_LEN + 1], b[SHEETID_LEN + 1], c[SHEETID_LEN + 1];
    if (sheetid_get(a, sizeof(a)) != 0 || strlen(a) != SHEETID_LEN) {
        printf("FAIL sheetid_get after init\n");
        failures++;
    }
    sheetid_init();                     /* loads the same salt */
    sheetid_get(b, sizeof(b));
    expect("stable across a reload", b, a);
    serial_value = 42;
    sheetid_get(c, sizeof(c));
    if (!strcmp(c, a)) {
        printf("FAIL two serials gave one id\n");
        failures++;
    } else
        printf("ok   another serial, another id\n");
    serial_value = 0;
    if (sheetid_get(c, sizeof(c)) != -1 || c[0]) {
        printf("FAIL no serial must give no id\n");
        failures++;
    } else
        printf("ok   no serial, no id\n");

    char path[300];
    snprintf(path, sizeof(path), "%s/sheet.salt", dir);
    unlink(path);
    rmdir(dir);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
