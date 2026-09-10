/*
 * tls.c - the panel's self-signed certificate
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The panel login and every state-changing route are served over
 * HTTPS. There is no authority to sign for a machine on a private
 * network, so the certificate is self-signed and per machine, made
 * once at first start and kept in the data directory (it survives
 * updates, so the browser's one-time exception holds). An ECDSA P-256
 * key: instant to make on the board's single core and accepted by
 * every current browser. The subject and the alternative name are the
 * machine's own hostname (forgefirm-<xxxx>), which is what a network
 * with dynamic DNS publishes and what the address bar shows when the
 * panel is reached by name rather than by address. Validity is limited
 * to what the strictest browsers accept for a trusted leaf; an expired
 * certificate is replaced at the next start, and so is one that names
 * another machine. The fingerprint the panel shows changes with it.
 *
 * What this buys: a passive listener on the LAN sees no password and
 * no session. What it does not buy: protection against an active
 * attacker who presents a certificate of their own; the fingerprint
 * on the panel's Commissioning card and the /cert page is the check
 * for that.
 */
#define _GNU_SOURCE
#include "tls.h"
#include "fflog.h"
#include "paths.h"

#include <errno.h>
#include <fcntl.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VALID_DAYS 820              /* under the 825-day leaf limit */

static char *key_pem, *cert_pem;
static char fingerprint[3 * 32];
static char names[256];
static char valid_from[32], valid_until[32];

static void iso_utc(time_t t, char *buf, size_t len)
{
    struct tm tm;
    if (t == (time_t)-1) {
        buf[0] = '\0';
        return;
    }
    gmtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* The machine's own name, which is the name the certificate carries. */
static void self_name(char *buf, size_t len)
{
    if (gethostname(buf, len) != 0)
        buf[0] = '\0';
    buf[len - 1] = '\0';                /* truncation leaves it unterminated */
    if (!buf[0])
        snprintf(buf, len, "forgefirm");
}

static void path_of(const char *name, char *buf, size_t len)
{
    snprintf(buf, len, "%s/tls/%s", ff_data_dir(), name);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n <= 0 || n > 65536) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static int write_file(const char *path, const char *text)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    size_t len = strlen(text);
    int ok = write(fd, text, len) == (ssize_t)len && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Fill the fingerprint from a certificate; returns 0 or -1. Also tells
 * whether the certificate is still inside its validity. */
static int inspect(gnutls_x509_crt_t crt, int *valid_now)
{
    unsigned char fp[32];
    size_t fplen = sizeof(fp);
    if (gnutls_x509_crt_get_fingerprint(crt, GNUTLS_DIG_SHA256, fp, &fplen) < 0)
        return -1;
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < fplen && o + 3 < sizeof(fingerprint); i++) {
        if (i)
            fingerprint[o++] = ':';
        fingerprint[o++] = hex[fp[i] >> 4];
        fingerprint[o++] = hex[fp[i] & 0xf];
    }
    fingerprint[o] = '\0';
    time_t now = time(NULL);
    time_t exp = gnutls_x509_crt_get_expiration_time(crt);
    time_t act = gnutls_x509_crt_get_activation_time(crt);
    iso_utc(act, valid_from, sizeof(valid_from));
    iso_utc(exp, valid_until, sizeof(valid_until));
    names[0] = '\0';
    for (unsigned i = 0; i < 16; i++) {
        char name[96];
        size_t nl = sizeof(name) - 1;
        unsigned type;
        int rc = gnutls_x509_crt_get_subject_alt_name2(crt, i, name, &nl, &type, NULL);
        if (rc < 0)
            break;
        if (type != GNUTLS_SAN_DNSNAME)
            continue;
        name[nl < sizeof(name) - 1 ? nl : sizeof(name) - 1] = '\0';
        size_t have = strlen(names);
        snprintf(names + have, sizeof(names) - have, "%s%s", have ? " " : "", name);
    }
    /* A clock that reads before the activation is a clock that is not
     * set yet (no battery): treat the certificate as valid rather than
     * regenerate one on every unsynced boot. */
    *valid_now = exp != (time_t)-1 && now < exp && (now >= act || now < 1700000000);
    return 0;
}

static int load_existing(void)
{
    char kp[280], cp[280];
    path_of("key.pem", kp, sizeof(kp));
    path_of("cert.pem", cp, sizeof(cp));
    char *k = read_file(kp), *c = read_file(cp);
    if (!k || !c) {
        free(k);
        free(c);
        return -1;
    }
    char self[64];
    self_name(self, sizeof(self));
    gnutls_x509_crt_t crt;
    gnutls_datum_t d = { (unsigned char *)c, (unsigned)strlen(c) };
    int ok = gnutls_x509_crt_init(&crt) == 0;
    int valid = 0;
    /* A stored certificate that does not name the machine is replaced,
     * the same way an expired one is: the name follows the MAC address,
     * so an image that named the machine differently leaves one behind.
     * The fingerprint changes with it, and the panel shows the new one. */
    if (ok && gnutls_x509_crt_import(crt, &d, GNUTLS_X509_FMT_PEM) == 0 &&
        inspect(crt, &valid) == 0 && valid && !strcmp(names, self)) {
        gnutls_x509_crt_deinit(crt);
        key_pem = k;
        cert_pem = c;
        return 0;
    }
    if (ok)
        gnutls_x509_crt_deinit(crt);
    free(k);
    free(c);
    fingerprint[0] = '\0';
    return -1;
}

static int generate(void)
{
    gnutls_x509_privkey_t key = NULL;
    gnutls_x509_crt_t crt = NULL;
    int rc = -1, r = 0;
    const char *step = NULL;            /* the step that failed, for the log */
    char host[64];
    self_name(host, sizeof(host));

    step = "object init";
    if ((r = gnutls_x509_privkey_init(&key)) < 0 || (r = gnutls_x509_crt_init(&crt)) < 0)
        goto out;
    step = "key generation";
    if ((r = gnutls_x509_privkey_generate(key, GNUTLS_PK_ECDSA,
                                          GNUTLS_CURVE_TO_BITS(GNUTLS_ECC_CURVE_SECP256R1),
                                          0)) < 0)
        goto out;
    step = "subject";
    if ((r = gnutls_x509_crt_set_version(crt, 3)) < 0 ||
        (r = gnutls_x509_crt_set_dn_by_oid(crt, GNUTLS_OID_X520_COMMON_NAME, 0,
                                           host, (unsigned)strlen(host))) < 0 ||
        (r = gnutls_x509_crt_set_dn_by_oid(crt, GNUTLS_OID_X520_ORGANIZATION_NAME, 0,
                                           "ForgeFIRM", 9)) < 0)
        goto out;
    unsigned char serial[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, serial, sizeof(serial)) != (ssize_t)sizeof(serial)) {
        fflog(LOG_ERR, "tls: reading /dev/urandom failed: %s", strerror(errno));
        if (fd >= 0)
            close(fd);
        step = NULL;
        goto out;
    }
    close(fd);
    serial[0] &= 0x7f;                  /* a positive serial */
    step = "serial";
    if ((r = gnutls_x509_crt_set_serial(crt, serial, sizeof(serial))) < 0)
        goto out;
    time_t now = time(NULL);
    if (now < 1700000000)
        now = 1700000000;               /* an unset clock: date it in the past */
    step = "validity";
    if ((r = gnutls_x509_crt_set_activation_time(crt, now - 86400)) < 0 ||
        (r = gnutls_x509_crt_set_expiration_time(crt, now + (time_t)VALID_DAYS * 86400)) < 0)
        goto out;
    step = "alternative names";
    if ((r = gnutls_x509_crt_set_subject_alt_name(crt, GNUTLS_SAN_DNSNAME, host,
                                                  (unsigned)strlen(host), GNUTLS_FSAN_SET)) < 0)
        goto out;
    step = "key usage";
    if ((r = gnutls_x509_crt_set_key_usage(crt, GNUTLS_KEY_DIGITAL_SIGNATURE)) < 0 ||
        (r = gnutls_x509_crt_set_key_purpose_oid(crt, GNUTLS_KP_TLS_WWW_SERVER, 0)) < 0 ||
        (r = gnutls_x509_crt_set_basic_constraints(crt, 0, -1)) < 0 ||
        (r = gnutls_x509_crt_set_key(crt, key)) < 0)
        goto out;
    step = "signing";
    if ((r = gnutls_x509_crt_sign2(crt, crt, key, GNUTLS_DIG_SHA256, 0)) < 0)
        goto out;

    step = "PEM export";
    gnutls_datum_t kout = { NULL, 0 }, cout = { NULL, 0 };
    if ((r = gnutls_x509_privkey_export2(key, GNUTLS_X509_FMT_PEM, &kout)) < 0 ||
        (r = gnutls_x509_crt_export2(crt, GNUTLS_X509_FMT_PEM, &cout)) < 0) {
        gnutls_free(kout.data);
        gnutls_free(cout.data);
        goto out;
    }
    step = NULL;
    char *k = malloc(kout.size + 1), *c = malloc(cout.size + 1);
    if (!k || !c) {
        fflog(LOG_ERR, "tls: out of memory");
        free(k);
        free(c);
        gnutls_free(kout.data);
        gnutls_free(cout.data);
        goto out;
    }
    memcpy(k, kout.data, kout.size);
    k[kout.size] = '\0';
    memcpy(c, cout.data, cout.size);
    c[cout.size] = '\0';
    gnutls_free(kout.data);
    gnutls_free(cout.data);

    char dir[280], kp[280], cp[280];
    snprintf(dir, sizeof(dir), "%s/tls", ff_data_dir());
    mkdir(ff_data_dir(), 0755);
    mkdir(dir, 0700);
    path_of("key.pem", kp, sizeof(kp));
    path_of("cert.pem", cp, sizeof(cp));
    if (write_file(kp, k) != 0 || write_file(cp, c) != 0) {
        fflog(LOG_ERR, "tls: cannot persist the certificate under %s", dir);
        free(k);
        free(c);
        goto out;
    }
    int valid;
    inspect(crt, &valid);
    key_pem = k;
    cert_pem = c;
    rc = 0;
    fflog(LOG_NOTICE, "tls: generated a certificate for %s (%s)", host, fingerprint);
out:
    if (step)
        fflog(LOG_ERR, "tls: %s failed: %s", step, gnutls_strerror(r));
    if (crt)
        gnutls_x509_crt_deinit(crt);
    if (key)
        gnutls_x509_privkey_deinit(key);
    return rc;
}

int tls_init(void)
{
    if (key_pem && cert_pem)
        return 0;
    if (load_existing() == 0)
        return 0;
    if (generate() == 0)
        return 0;
    fflog(LOG_ERR, "tls: no certificate; HTTPS is unavailable");
    return -1;
}

const char *tls_key_pem(void)
{
    return key_pem;
}

const char *tls_cert_pem(void)
{
    return cert_pem;
}

const char *tls_fingerprint(void)
{
    return fingerprint;
}

const char *tls_names(void)
{
    return names;
}

const char *tls_valid_from(void)
{
    return valid_from;
}

const char *tls_valid_until(void)
{
    return valid_until;
}
