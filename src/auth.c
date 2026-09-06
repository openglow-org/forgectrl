/*
 * auth.c - forgectrl: HTTP access control
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The panel and its API are served in the clear on the LAN, and the LAN
 * is trusted: any host on it can read the panel (and the token in it)
 * and so reach every route, and the read routes (status, settings,
 * the cameras) need no token at all. What the layers below close is a
 * hostile page in a same-LAN browser (CSRF) and a DNS-rebinding host;
 * the one thing no LAN client can do is what needs the machine button
 * held (an unsigned install, the fuse identity). Three cheap layers:
 *
 *  1. Host must be an address literal, localhost, or one of the machine's
 *     own names (forgefirm, forgefirm.local, the fuse hostname). A
 *     rebinding attacker reaches the daemon under their own hostname,
 *     which is none of those, so the pages (and the token in them)
 *     cannot be read back, and no state-changing call from that origin
 *     is honored.
 *  2. Sec-Fetch-Site, when the browser sends it, must be same-origin or
 *     none; a cross-site request is refused. An Origin header, when
 *     present, must itself be an address literal.
 *  3. A first-boot bearer token, stored 0600 in /data and embedded in
 *     the panel, is required on every state-changing endpoint. A CSRF
 *     page cannot read the token (same-origin policy hides the panel
 *     response), so it cannot forge an authorized call even against an
 *     old browser that omits Sec-Fetch-Site.
 *
 * The cooling report channel is authenticated differently: it only ever
 * comes from the controller on the same host, so it is restricted to a
 * loopback peer rather than the token.
 *
 * On top of the three layers sits the login. Once the first-run wizard
 * has created the account, every state-changing route also needs a
 * valid session cookie (users.c, session.c), except from this host
 * (the init scripts, the controllers, the acceptance tool on the
 * board) and on a dev image, where the token alone still writes so the
 * bench tools keep working. Before the account exists, the token alone
 * writes: the wizard's own calls need it, and the page that carries it
 * is served to whoever reaches the machine first. The read-only routes
 * stay open to the LAN by default (panel_open_reads = 1) so LightBurn
 * reads the camera; the setting closes them to sessions and this host.
 * The login itself and the session cookie travel over HTTPS only.
 */
#define _GNU_SOURCE
#include "auth.h"
#include "camkey.h"
#include "fflog.h"
#include "logs.h"
#include "peer.h"
#include "session.h"
#include "settings.h"
#include "users.h"

#include <arpa/inet.h>

#include <ctype.h>
#include <fcntl.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOKEN_DIR   "/data/forgefirm"
#define TOKEN_FILE  TOKEN_DIR "/panel.token"
#define TOKEN_HEX   32                 /* 128 bits */
#define SWITCH_DEV  "/dev/input/event0"
#define SW_BIT_BUTTON 2

static char token[TOKEN_HEX + 1];

static void generate_token(void)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[TOKEN_HEX / 2];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, raw, sizeof(raw)) != (ssize_t)sizeof(raw)) {
        if (fd >= 0)
            close(fd);
        /* Never ship a predictable token: without entropy the panel
         * must stay locked rather than fall back to something guessable. */
        token[0] = '\0';
        fflog(LOG_ERR, "auth: cannot read /dev/urandom - "
                       "panel token unavailable");
        return;
    }
    close(fd);
    for (size_t i = 0; i < sizeof(raw); i++) {
        token[i * 2] = hex[raw[i] >> 4];
        token[i * 2 + 1] = hex[raw[i] & 0xf];
    }
    token[TOKEN_HEX] = '\0';
}

void auth_init(void)
{
    mkdir(TOKEN_DIR, 0755);

    FILE *f = fopen(TOKEN_FILE, "r");
    if (f) {
        char buf[TOKEN_HEX + 4] = "";
        if (fgets(buf, sizeof(buf), f)) {
            size_t n = strspn(buf, "0123456789abcdef");
            if (n == TOKEN_HEX && (buf[n] == '\0' || buf[n] == '\n')) {
                memcpy(token, buf, TOKEN_HEX);
                token[TOKEN_HEX] = '\0';
            }
        }
        fclose(f);
        if (token[0])
            return;
    }

    generate_token();
    if (!token[0])
        return;

    int fd = open(TOKEN_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fflog(LOG_ERR, "auth: cannot persist panel token");
        return;
    }
    (void)!write(fd, token, TOKEN_HEX);
    (void)!write(fd, "\n", 1);
    close(fd);
    fflog(LOG_NOTICE, "auth: generated a new panel token");
}

const char *auth_token(void)
{
    return token;
}

/* Constant-time equality over the token length. */
static int token_eq(const char *a)
{
    if (!token[0] || !a)
        return 0;
    unsigned diff = 0;
    size_t i;
    for (i = 0; i < TOKEN_HEX && a[i]; i++)
        diff |= (unsigned char)a[i] ^ (unsigned char)token[i];
    /* length must match exactly */
    diff |= (unsigned)i ^ TOKEN_HEX;
    diff |= (unsigned char)a[i];        /* a[TOKEN_HEX] must be the terminator */
    return diff == 0;
}

/* A host string (Host header value, or an Origin's authority) is
 * accepted as an address literal, localhost, or one of the machine's
 * own names; never any other DNS name, which is the vehicle for a
 * rebinding attack. */
static int host_is_literal(const char *h)
{
    if (!h || !*h)
        return 0;
    if (*h == '[')
        return 1;                       /* bracketed IPv6 literal */

    char hb[128];
    size_t o = 0;
    for (; h[o] && h[o] != ':' && o + 1 < sizeof(hb); o++)
        hb[o] = h[o];
    hb[o] = '\0';

    if (!hb[0])
        return 0;
    /* The machine's own names pass with the literals: they are what the
     * certificate carries and what mDNS answers to, and a rebinding
     * attacker cannot make a browser send them. Compared without case
     * (DNS names are case-insensitive), a trailing dot stripped. */
    size_t n = strlen(hb);
    if (n > 1 && hb[n - 1] == '.')
        hb[--n] = '\0';
    for (size_t i = 0; hb[i]; i++)
        hb[i] = (char)tolower((unsigned char)hb[i]);
    if (!strcmp(hb, "localhost") || !strcmp(hb, "forgefirm") ||
        !strcmp(hb, "forgefirm.local"))
        return 1;
    char mid[16], midl[32];
    machine_id(mid, sizeof(mid));
    if (mid[0]) {
        for (size_t i = 0; mid[i]; i++)
            mid[i] = (char)tolower((unsigned char)mid[i]);
        snprintf(midl, sizeof(midl), "%s.local", mid);
        if (!strcmp(hb, mid) || !strcmp(hb, midl))
            return 1;
    }
    for (size_t i = 0; hb[i]; i++)
        if (!isdigit((unsigned char)hb[i]) && hb[i] != '.')
            return 0;                   /* any other name is refused */
    return 1;
}

/* Extract the authority (host[:port]) from a scheme://authority[/...]
 * Origin value into out. */
static void origin_authority(const char *origin, char *out, size_t len)
{
    out[0] = '\0';
    const char *p = strstr(origin, "://");
    if (!p)
        return;
    p += 3;
    size_t o = 0;
    for (; p[o] && p[o] != '/' && o + 1 < len; o++)
        out[o] = p[o];
    out[o] = '\0';
}

static int deny(struct _u_response *res, unsigned status, const char *msg)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    ulfius_set_string_body_response(res, status, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return 0;
}

/* Layer 1: Host literal + Sec-Fetch-Site + Origin. Returns 1 to allow. */
static int origin_ok(const struct _u_request *req)
{
    const char *host = u_map_get_case(req->map_header, "Host");
    if (!host_is_literal(host))
        return 0;

    const char *sfs = u_map_get_case(req->map_header, "Sec-Fetch-Site");
    if (sfs && strcmp(sfs, "same-origin") && strcmp(sfs, "none"))
        return 0;

    const char *origin = u_map_get_case(req->map_header, "Origin");
    if (origin && strcmp(origin, "null")) {
        char auth[128];
        origin_authority(origin, auth, sizeof(auth));
        if (!host_is_literal(auth))
            return 0;
    }
    return 1;
}

int auth_session_ok(const struct _u_request *req)
{
    const char *cookie = u_map_get_case(req->map_header, "Cookie");
    char id[SESSION_ID_HEX + 1];
    return cookie && session_from_cookie(cookie, id, sizeof(id)) &&
           session_valid(id);
}

int auth_peer_local(const struct _u_request *req)
{
    return peer_is_loopback(req->client_address);
}

void auth_peer_text(const struct _u_request *req, char *buf, size_t len)
{
    buf[0] = '\0';
    const struct sockaddr *sa = req->client_address;
    if (!sa)
        return;
    if (sa->sa_family == AF_INET)
        inet_ntop(AF_INET, &((const struct sockaddr_in *)sa)->sin_addr, buf, (socklen_t)len);
    else if (sa->sa_family == AF_INET6)
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)sa)->sin6_addr, buf, (socklen_t)len);
}

int auth_dev_image(void)
{
    static int checked, dev;
    if (!checked) {
        dev = access("/etc/forgefirm-dev", F_OK) == 0;
        checked = 1;
    }
    return dev;
}

/* Who may change state, beyond the token: a logged-in session, this
 * host, a dev image, or a machine with no account yet. */
static int writer_ok(const struct _u_request *req)
{
    return auth_session_ok(req) || auth_peer_local(req) || auth_dev_image() ||
           !users_exist();
}

int auth_origin_ok(const struct _u_request *req, struct _u_response *res)
{
    if (!origin_ok(req))
        return deny(res, 403, "request origin refused");
    return 1;
}

/* The camera key on a read: a `key` query parameter or the header. A
 * valid key is a credential in its own right, so it also passes the
 * origin checks: a camera consumer sends whatever Host it likes, and a
 * rebinding page cannot present a key it does not have. */
static int camkey_presented(const struct _u_request *req)
{
    const char *k = u_map_get_case(req->map_header, "X-ForgeFIRM-Camera-Key");
    if (!k)
        k = u_map_get(req->map_url, "key");
    return camkey_valid(k);
}

int auth_read_ok(const struct _u_request *req, struct _u_response *res)
{
    if (camkey_presented(req))
        return 1;
    if (!origin_ok(req))
        return deny(res, 403, "request origin refused");
    if (settings_get_bool("panel_open_reads", 1))
        return 1;
    if (auth_session_ok(req) || auth_peer_local(req))
        return 1;
    return deny(res, 403, "login required");
}

int auth_write_permitted(const struct _u_request *req)
{
    if (!origin_ok(req))
        return 0;
    const char *tok = u_map_get_case(req->map_header, "X-ForgeFIRM-Token");
    if (!tok)
        tok = u_map_get(req->map_url, "token");
    return token_eq(tok) && writer_ok(req);
}

int auth_write_ok(const struct _u_request *req, struct _u_response *res)
{
    if (!origin_ok(req))
        return deny(res, 403, "request origin refused");
    const char *tok = u_map_get_case(req->map_header, "X-ForgeFIRM-Token");
    if (!tok)
        tok = u_map_get(req->map_url, "token");
    if (!token_eq(tok))
        return deny(res, 403, "authentication required");
    if (!writer_ok(req))
        return deny(res, 403, "login required");
    return 1;
}

int auth_loopback_ok(const struct _u_request *req, struct _u_response *res)
{
    if (!origin_ok(req))
        return deny(res, 403, "request origin refused");
    /* The peer must be the whole sockaddr: ulfius 2.7.15 copies only
     * sizeof(struct sockaddr) into client_address, which truncates the
     * sockaddr_in6 a dual-stack listener reports for every peer, and a
     * truncated copy fails this check closed. The image carries the
     * ulfius patch that copies the family's length. */
    if (!peer_is_loopback(req->client_address))
        return deny(res, 403, "loopback only");
    return 1;
}

int operator_present(void)
{
    uint8_t sw[2] = { 0 };
    int fd = open(SWITCH_DEV, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 0;
    int ok = ioctl(fd, EVIOCGSW(sizeof(sw)), sw) >= 0;
    close(fd);
    return ok && (sw[SW_BIT_BUTTON / 8] & (1u << (SW_BIT_BUTTON % 8)));
}
