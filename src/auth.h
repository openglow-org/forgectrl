/*
 * auth.h - forgectrl: HTTP access control
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_AUTH_H
#define FORGECTRL_AUTH_H

#include <stddef.h>
#include <ulfius.h>

/* Load (or first-boot generate) the panel bearer token from /data.
 * Idempotent; call once at startup before serving. */
void auth_init(void);

/* The panel token, for injection into the served page. Never empty
 * after auth_init(). */
const char *auth_token(void);

/* Request guards. Each returns 1 if the request may proceed, or 0 after
 * writing a 4xx response into res (the caller then returns
 * U_CALLBACK_COMPLETE). All three first apply the anti-CSRF /
 * anti-DNS-rebinding origin checks (Host must be an address literal, a
 * cross-site Sec-Fetch-Site is refused, a mismatched Origin is refused).
 *
 *  - auth_read_ok:     origin checks only (read-only endpoints, and the
 *                      panel itself so the embedded token cannot be read
 *                      back through a rebinding host).
 *  - auth_write_ok:    origin checks + the bearer token (every
 *                      state-changing endpoint and the fuse view).
 *  - auth_loopback_ok: origin checks + the peer must be loopback (the
 *                      controller's cooling report channel).
 */
int auth_read_ok(const struct _u_request *req, struct _u_response *res);
int auth_write_ok(const struct _u_request *req, struct _u_response *res);
int auth_loopback_ok(const struct _u_request *req, struct _u_response *res);
/* The origin checks alone: the pages and the login itself, which must
 * answer before any session exists and whatever panel_open_reads says. */
int auth_origin_ok(const struct _u_request *req, struct _u_response *res);
/* Whether the request carries a valid login session cookie. */
int auth_session_ok(const struct _u_request *req);
/* Whether the peer is this host (the init scripts, forgetest on the
 * board, the controllers). */
int auth_peer_local(const struct _u_request *req);
/* The peer address as text, for the login throttle. */
void auth_peer_text(const struct _u_request *req, char *buf, size_t len);
/* The dev-image marker (/etc/forgefirm-dev): token-only writes are
 * accepted on a dev image so the bench tools keep working. */
int auth_dev_image(void);

/* Silent form of the write check (origin + token), for the file-upload
 * sink which runs during body parse and has no response object. Returns
 * 1 if the request carries a valid origin and token. */
int auth_write_permitted(const struct _u_request *req);

/* Operator-present factor: true only while the physical button is held.
 * Gates the irrevocable fuse view and unsigned-firmware installs. */
int operator_present(void);

#endif /* FORGECTRL_AUTH_H */
