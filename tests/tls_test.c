/*
 * tls_test.c - host test: the panel's self-signed certificate
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "../src/tls.h"

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); failures++; } } while (0)

int main(void)
{
    char dir[] = "/tmp/tls-test-XXXXXX";
    if (!mkdtemp(dir)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    setenv("FORGECTRL_DATA_DIR", dir, 1);

    CHECK(tls_init() == 0, "a certificate is made at first start");
    CHECK(tls_key_pem() && strstr(tls_key_pem(), "PRIVATE KEY"), "the key is PEM");
    CHECK(tls_cert_pem() && strstr(tls_cert_pem(), "BEGIN CERTIFICATE"), "the certificate is PEM");
    char fp[128];
    snprintf(fp, sizeof(fp), "%s", tls_fingerprint());
    CHECK(strlen(fp) == 95 && fp[2] == ':', "the fingerprint is 32 colon-separated bytes");

    /* The names on it. */
    gnutls_x509_crt_t crt;
    gnutls_datum_t d = { (unsigned char *)tls_cert_pem(), (unsigned)strlen(tls_cert_pem()) };
    gnutls_x509_crt_init(&crt);
    CHECK(gnutls_x509_crt_import(crt, &d, GNUTLS_X509_FMT_PEM) == 0, "the certificate parses");
    char self[64];
    if (gethostname(self, sizeof(self)) != 0)
        self[0] = '\0';
    self[sizeof(self) - 1] = '\0';
    if (!self[0])
        snprintf(self, sizeof(self), "forgefirm");
    int have_host = 0, others = 0;
    for (unsigned i = 0; i < 8; i++) {
        char name[64];
        size_t nl = sizeof(name);
        unsigned type;
        int rc = gnutls_x509_crt_get_subject_alt_name2(crt, i, name, &nl, &type, NULL);
        if (rc < 0)
            break;
        name[nl < sizeof(name) ? nl : sizeof(name) - 1] = '\0';
        if (!strcmp(name, self))
            have_host = 1;
        else
            others++;
    }
    CHECK(have_host, "the machine's hostname is on it");
    CHECK(others == 0, "it carries that one name and nothing else");
    gnutls_x509_crt_deinit(crt);

    /* A second daemon start loads the same certificate. */
    char key[300], cert[300];
    snprintf(key, sizeof(key), "%s/tls/key.pem", dir);
    snprintf(cert, sizeof(cert), "%s/tls/cert.pem", dir);
    CHECK(access(key, F_OK) == 0 && access(cert, F_OK) == 0, "the files are persisted");
    /* Simulate a new process: the module keeps state, so test the file
     * path by comparing the fingerprint of the stored certificate. */
    gnutls_x509_crt_init(&crt);
    FILE *f = fopen(cert, "r");
    char pem[8192] = "";
    if (f) {
        size_t n = fread(pem, 1, sizeof(pem) - 1, f);
        pem[n] = '\0';
        fclose(f);
    }
    d.data = (unsigned char *)pem;
    d.size = (unsigned)strlen(pem);
    unsigned char raw[32];
    size_t rl = sizeof(raw);
    int ok = gnutls_x509_crt_import(crt, &d, GNUTLS_X509_FMT_PEM) == 0 &&
             gnutls_x509_crt_get_fingerprint(crt, GNUTLS_DIG_SHA256, raw, &rl) == 0;
    char stored[128] = "";
    if (ok) {
        static const char hex[] = "0123456789ABCDEF";
        size_t o = 0;
        for (size_t i = 0; i < rl; i++) {
            if (i)
                stored[o++] = ':';
            stored[o++] = hex[raw[i] >> 4];
            stored[o++] = hex[raw[i] & 0xf];
        }
        stored[o] = '\0';
    }
    CHECK(ok && !strcmp(stored, fp), "the stored certificate has the served fingerprint");
    gnutls_x509_crt_deinit(crt);

    unlink(key);
    unlink(cert);
    snprintf(key, sizeof(key), "%s/tls", dir);
    rmdir(key);
    rmdir(dir);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
