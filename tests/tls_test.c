/*
 * tls_test.c - host test: the panel's self-signed certificate
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "../src/tls.h"

#include <fcntl.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); failures++; } } while (0)

/* What a desktop browser offers: AES-GCM ahead of ChaCha20, because its
 * CPU has AES instructions. */
#define DESKTOP "NORMAL:-CIPHER-ALL:+AES-128-GCM:+AES-256-GCM:+CHACHA20-POLY1305"

/* One handshake over a socket pair, the server set up as the listener
 * is (the panel's certificate and key), each side with its own
 * priorities. Returns the cipher both agreed, or GNUTLS_CIPHER_UNKNOWN
 * when the handshake failed; the protocol agreed goes in *version. */
static gnutls_cipher_algorithm_t agree(const char *server_prio, const char *client_prio,
                                       gnutls_protocol_t *version)
{
    gnutls_cipher_algorithm_t cipher = GNUTLS_CIPHER_UNKNOWN;
    gnutls_certificate_credentials_t scred = NULL, ccred = NULL;
    gnutls_session_t s = NULL, c = NULL;
    gnutls_datum_t key = { (unsigned char *)tls_key_pem(), (unsigned)strlen(tls_key_pem()) };
    gnutls_datum_t cert = { (unsigned char *)tls_cert_pem(), (unsigned)strlen(tls_cert_pem()) };
    int sv[2];
    *version = GNUTLS_VERSION_UNKNOWN;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return cipher;
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    if (gnutls_certificate_allocate_credentials(&scred) < 0 ||
        gnutls_certificate_allocate_credentials(&ccred) < 0 ||
        gnutls_certificate_set_x509_key_mem(scred, &cert, &key, GNUTLS_X509_FMT_PEM) < 0 ||
        gnutls_init(&s, GNUTLS_SERVER | GNUTLS_NONBLOCK) < 0 ||
        gnutls_init(&c, GNUTLS_CLIENT | GNUTLS_NONBLOCK) < 0 ||
        gnutls_credentials_set(s, GNUTLS_CRD_CERTIFICATE, scred) < 0 ||
        gnutls_credentials_set(c, GNUTLS_CRD_CERTIFICATE, ccred) < 0 ||
        gnutls_priority_set_direct(s, server_prio, NULL) < 0 ||
        gnutls_priority_set_direct(c, client_prio, NULL) < 0)
        goto out;
    gnutls_transport_set_int(s, sv[0]);
    gnutls_transport_set_int(c, sv[1]);
    int sdone = 0, cdone = 0;
    for (int i = 0; i < 100000 && !(sdone && cdone); i++) {
        int r;
        if (!sdone && (r = gnutls_handshake(s)) != GNUTLS_E_AGAIN) {
            if (r < 0 && gnutls_error_is_fatal(r))
                goto out;
            sdone = r == 0;
        }
        if (!cdone && (r = gnutls_handshake(c)) != GNUTLS_E_AGAIN) {
            if (r < 0 && gnutls_error_is_fatal(r))
                goto out;
            cdone = r == 0;
        }
    }
    if (sdone && cdone && gnutls_cipher_get(s) == gnutls_cipher_get(c)) {
        cipher = gnutls_cipher_get(s);
        *version = gnutls_protocol_get_version(s);
    }
out:
    if (s)
        gnutls_deinit(s);
    if (c)
        gnutls_deinit(c);
    if (scred)
        gnutls_certificate_free_credentials(scred);
    if (ccred)
        gnutls_certificate_free_credentials(ccred);
    close(sv[0]);
    close(sv[1]);
    return cipher;
}

/* The ciphers a priority string allows, as a bit set over the cipher
 * ids, and the first of them. */
static unsigned long long cipher_set(const char *prio, unsigned *first)
{
    gnutls_priority_t p;
    const unsigned *list;
    unsigned long long set = 0;
    *first = GNUTLS_CIPHER_UNKNOWN;
    if (gnutls_priority_init(&p, prio, NULL) < 0)
        return 0;
    int n = gnutls_priority_cipher_list(p, &list);
    for (int i = 0; i < n; i++) {
        if (list[i] < 64)
            set |= 1ULL << list[i];
        if (i == 0)
            *first = list[i];
    }
    gnutls_priority_deinit(p);
    return set;
}

/* The listener's cipher order: its own choice, ChaCha20-Poly1305 first,
 * and every client that connected under NORMAL still does. */
static void check_cipher_order(void)
{
    const char *prio = tls_priorities();
    gnutls_protocol_t v;
    gnutls_priority_t p;
    int parsed = gnutls_priority_init(&p, prio, NULL) == 0;
    CHECK(parsed, "the priority string parses");
    if (parsed)
        gnutls_priority_deinit(p);

    unsigned first_normal, first_ours;
    unsigned long long normal = cipher_set("NORMAL", &first_normal);
    unsigned long long ours = cipher_set(prio, &first_ours);
    CHECK(normal && ours == normal, "it allows exactly NORMAL's ciphers");
    CHECK(first_ours == GNUTLS_CIPHER_CHACHA20_POLY1305, "ChaCha20-Poly1305 is its first cipher");

    /* The control: a server that follows the client picks AES-GCM for a
     * desktop browser, so the checks below can tell the orders apart. */
    CHECK(agree("NORMAL", DESKTOP, &v) == GNUTLS_CIPHER_AES_128_GCM,
          "control: a client-order server gives a desktop browser AES-128-GCM");
    CHECK(agree(prio, DESKTOP, &v) == GNUTLS_CIPHER_CHACHA20_POLY1305 && v == GNUTLS_TLS1_3,
          "a desktop browser gets ChaCha20-Poly1305 over TLS 1.3");
    CHECK(agree(prio, DESKTOP ":-VERS-ALL:+VERS-TLS1.2", &v) == GNUTLS_CIPHER_CHACHA20_POLY1305 &&
              v == GNUTLS_TLS1_2,
          "and over TLS 1.2");
    CHECK(agree(prio, "NORMAL:-CIPHER-ALL:+AES-256-GCM:+AES-128-GCM", &v) == GNUTLS_CIPHER_AES_128_GCM,
          "a client without ChaCha20 gets AES-128-GCM ahead of AES-256-GCM");
    CHECK(agree(prio, "NORMAL:-CIPHER-ALL:+AES-256-GCM", &v) == GNUTLS_CIPHER_AES_256_GCM,
          "one with AES-256-GCM alone gets that");
    CHECK(agree(prio, "NORMAL:-VERS-ALL:+VERS-TLS1.2:-CIPHER-ALL:+AES-128-CBC", &v) ==
              GNUTLS_CIPHER_AES_128_CBC && v == GNUTLS_TLS1_2,
          "a TLS 1.2 client with CBC alone still connects");
}

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

    check_cipher_order();

    unlink(key);
    unlink(cert);
    snprintf(key, sizeof(key), "%s/tls", dir);
    rmdir(key);
    rmdir(dir);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
