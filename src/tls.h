/*
 * tls.h - the panel's self-signed certificate (see tls.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_TLS_H
#define FORGECTRL_TLS_H

/* Load the key and certificate from the data directory, or generate
 * them at first start. Returns 0 when HTTPS can be served, -1 when not
 * (the daemon then serves HTTP only and says so). */
int tls_init(void);
/* The PEM texts for the listener; NULL until tls_init() succeeded. */
const char *tls_key_pem(void);
const char *tls_cert_pem(void);
/* The certificate's SHA-256 fingerprint, colon-separated uppercase hex,
 * for the welcome screen and the System tab; "" when unavailable. */
const char *tls_fingerprint(void);
/* The DNS names the certificate carries, space-separated; "" when none. */
const char *tls_names(void);
/* The certificate's activation and expiration, ISO 8601 UTC; "" when
 * unavailable. */
const char *tls_valid_from(void);
const char *tls_valid_until(void);
#endif
