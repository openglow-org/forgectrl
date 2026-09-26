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
 * 1 if the request carries a valid origin and token. No scoped token
 * passes it. */
int auth_write_permitted(const struct _u_request *req);
/* The same for a sink whose route a scoped token may reach: the sink
 * runs before any route's callback, so it names the capability itself. */
int auth_write_permitted_cap(const struct _u_request *req, const char *cap);

/* Scoped tokens (tokens.h). The route table's wrapper brackets every
 * route callback with these, on the request's thread: the capability a
 * scoped token needs on this route (NULL: none reaches it; "camera":
 * camera.<the request's cam>), and whether the request came over plain
 * HTTP. Every guard above reads them. A request that presents a scoped
 * token is judged by it alone: the capability, and HTTPS unless the peer
 * is this host; and a token that arrives in a camera route's URL holds
 * camera capabilities and nothing else. It needs no session and passes
 * the origin checks, which
 * exist for browsers; a page cannot put the header on a cross-origin
 * request. */
void auth_route_begin(const char *cap, int plain_http);
void auth_route_end(void);

/* Who the guard let in, on this request's thread: the extension host's
 * own credential (whose word about a package id is the only one taken),
 * or an operator's scoped token. Neither: a session, the panel token, a
 * camera key, or an open read. */
int auth_by_host(void);
int auth_by_scoped(void);

/* A camera request from the panel itself (a session, or the panel
 * token) or from the extension host on this machine (its client header,
 * from the loopback, where no package reaches). Only such a request may
 * use the head camera with the lid open; the cloud client, a camera key,
 * or a token from the network is none of these. */
int auth_local_viewer(const struct _u_request *req);

/* Operator-present factor: true only while the physical button is held.
 * Gates the irrevocable fuse view and unsigned-firmware installs. */
int operator_present(void);

#endif /* FORGECTRL_AUTH_H */
