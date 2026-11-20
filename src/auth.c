/*
 * auth.c - forgectrl: HTTP access control
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
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
 *  1. Host must be an address literal, localhost, or the machine's own
 *     hostname (forgefirm-<xxxx>, one label, no domain). A rebinding
 *     attacker reaches the daemon under their own hostname, which is
 *     none of those, so the pages (and the token in them) cannot be
 *     read back, and no state-changing call from that origin is
 *     honored.
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
 * bench tools keep working. Before the account exists (a first run, or
 * a reset until the account step), the page that carries the token is
 * served to whoever reaches the machine, so the token alone writes on
 * the routes that page calls before it has an account and on no other:
 * the documents, the press, the account step, and the sanitized log
 * bundle (SETUP_ROUTES). The account step itself takes only the
 * browser that holds the press (wiz.c). A machine whose account record
 * cannot be read is no machine without an account (users_fault): the
 * token alone writes nothing there. The read-only routes
 * stay open to the LAN by default (panel_open_reads = 1) so a Grbl sender
 * reads the camera; the setting closes them to sessions, this host, and
 * the panel token where it may write. A setting that cannot be read, or
 * reads as neither 0 nor 1, closes them; a settings file with no such
 * key, or none, leaves them open. The login itself and the session
 * cookie travel over HTTPS only. The panel token is taken from its
 * header alone, never from a URL.
 *
 * A scoped token (tokens.c) is the credential of a client with no
 * browser and no session. A request that presents one is judged by it
 * alone, against the capability the route table gives the route it is
 * on; see auth.h.
 */
#define _GNU_SOURCE
#include "auth.h"
#include "camkey.h"
#include "fflog.h"
#include "logs.h"
#include "paths.h"
#include "peer.h"
#include "session.h"
#include "settings.h"
#include "testhook.h"
#include "tokens.h"
#include "users.h"

#include <arpa/inet.h>

#include <ctype.h>
#include <fcntl.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOKEN_NAME  "panel.token"   /* in the data directory (paths.h) */
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
    char path[256];
    snprintf(path, sizeof(path), "%s/" TOKEN_NAME, ff_data_dir());
    mkdir(ff_data_dir(), 0755);

    FILE *f = fopen(path, "r");
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

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
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
 * accepted as an address literal, localhost, or the machine's own
 * hostname; never any other DNS name, which is the vehicle for a
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
    /* The machine's own hostname passes with the literals: it is what
     * the certificate carries, and a rebinding attacker cannot make a
     * browser send it. The single label only: a name with a domain on
     * it can be registered by anyone, so it stays refused, and a client
     * that reaches the machine through a dynamic-DNS domain uses the
     * address or the bare name from its own search list. Compared
     * without case (DNS names are case-insensitive), a trailing dot
     * stripped. */
    size_t n = strlen(hb);
    if (n > 1 && hb[n - 1] == '.')
        hb[--n] = '\0';
    for (size_t i = 0; hb[i]; i++)
        hb[i] = (char)tolower((unsigned char)hb[i]);
    if (!strcmp(hb, "localhost"))
        return 1;
    char self[64];
    if (gethostname(self, sizeof(self)) == 0) {
        self[sizeof(self) - 1] = '\0';
        for (size_t i = 0; self[i]; i++)
            self[i] = (char)tolower((unsigned char)self[i]);
        if (self[0] && !strcmp(hb, self))
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
    char body[256];
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
    char id[SESSION_ID_HEX + 1], peer[SESSION_ADDR_MAX];
    if (!cookie || !session_from_cookie(cookie, id, sizeof(id)))
        return 0;
    auth_peer_text(req, peer, sizeof(peer));
    return session_valid(id, peer);
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
        const char *m = ff_test_hook("GF_DEV_MARKER");     /* a host test's marker */
        dev = access(m ? m : "/etc/forgefirm-dev", F_OK) == 0;
        checked = 1;
    }
    return dev;
}

/* ------------------------------------------------------ scoped tokens */

static __thread const char *route_cap;
static __thread int route_plain;
static __thread const char *route_method, *route_path;
static __thread int judged_host;        /* the host's credential passed */
static __thread int judged_scoped;      /* an operator's scoped token passed */
static __thread int judged_setup;       /* the token passed on a setup route, no account */

void auth_route_begin(const char *cap, int plain_http)
{
    route_cap = cap;
    route_plain = plain_http;
    route_method = route_path = NULL;
    judged_host = judged_scoped = judged_setup = 0;
}

void auth_route_at(const char *method, const char *path)
{
    route_method = method;
    route_path = path;
}

void auth_route_end(void)
{
    route_cap = NULL;
    route_plain = 0;
    route_method = route_path = NULL;
    judged_host = judged_scoped = judged_setup = 0;
}

int auth_by_setup(void)
{
    return judged_setup;
}

int auth_by_host(void)
{
    return judged_host;
}

int auth_by_scoped(void)
{
    return judged_scoped;
}

/* The scoped token a request presents, or NULL: the Authorization
 * header's bearer, the panel token's own header, and on a camera route
 * the `key` parameter, where an <img> or a camera integration has no
 * header to send (*in_url says it came that way). */
static const char *scoped_presented(const struct _u_request *req, const char *cap, int *in_url)
{
    *in_url = 0;
    const char *v = u_map_get_case(req->map_header, "Authorization");
    if (v && !strncasecmp(v, "Bearer ", 7)) {
        for (v += 7; *v == ' '; v++)
            ;
        if (tokens_looks_scoped(v))
            return v;
    }
    v = u_map_get_case(req->map_header, "X-ForgeFIRM-Token");
    if (tokens_looks_scoped(v))
        return v;
    if (cap && !strncmp(cap, "camera", 6)) {
        v = u_map_get(req->map_url, "key");
        if (tokens_looks_scoped(v)) {
            *in_url = 1;
            return v;
        }
    }
    return NULL;
}

/* -1: no scoped token is presented, and the other rules decide. 1: it
 * passes. 0: it is refused, with the reason in why. */
static int scoped_judge(const struct _u_request *req, const char *cap, int plain,
                        char *why, size_t len)
{
    int in_url;
    const char *tok = scoped_presented(req, cap, &in_url);
    if (!tok)
        return -1;
    char need[32], id[TOKENS_ID_HEX + 1];
    if (cap && !strcmp(cap, "camera")) {
        const char *cam = u_map_get(req->map_url, "cam");
        snprintf(need, sizeof(need), "camera.%s", cam && !strcmp(cam, "head") ? "head" : "lid");
        cap = need;
    }
    /* The extension host's own credential is judged first and on the
     * same terms: it is a scoped credential that holds only what the
     * host may relay, and it is accepted from a loopback peer alone -
     * it never leaves this machine, so a presentation from anywhere
     * else is not the host. */
    if (tokens_host_check(tok, cap)) {
        if (!peer_is_loopback(req->client_address)) {
            fflog(LOG_WARNING, "auth: the extension host's credential was presented from off the machine");
            snprintf(why, len, "that credential is not accepted from here");
            return 0;
        }
        judged_host = 1;
        return 1;
    }
    int rc = tokens_check(tok, cap, id);
    if (rc < 0) {
        snprintf(why, len, "authentication required");
        return 0;
    }
    /* A token that crossed the network in the clear is not honored, and
     * the log says which one to revoke. */
    if (plain && !auth_peer_local(req)) {
        fflog(LOG_WARNING, "auth: the scoped token %s was presented over plain HTTP", id);
        snprintf(why, len, "a scoped token is accepted over HTTPS only");
        return 0;
    }
    /* A URL ends up in a history, a proxy's log, a referrer. A token that
     * can do more than see a camera is never taken from one. */
    if (in_url && !tokens_holds_only(tok, "camera.")) {
        fflog(LOG_WARNING, "auth: the scoped token %s, which holds more than a camera, was presented in a URL", id);
        snprintf(why, len, "a token in a URL may hold camera capabilities only");
        return 0;
    }
    if (rc == 0) {
        if (cap)
            snprintf(why, len, "this token does not hold %s", cap);
        else
            snprintf(why, len, "no scoped token reaches this route");
        return 0;
    }
    judged_scoped = 1;
    return 1;
}

int auth_local_viewer(const struct _u_request *req)
{
    const char *client = u_map_get_case(req->map_header, "X-ForgeFIRM-Client");
    if (client && !strcmp(client, "extension-host") && auth_peer_local(req))
        return 1;
    int in_url;
    if (scoped_presented(req, route_cap, &in_url) || !origin_ok(req))
        return 0;
    if (auth_session_ok(req))
        return 1;
    return token_eq(u_map_get_case(req->map_header, "X-ForgeFIRM-Token"));
}

/* The routes the setup page calls before the account exists: the
 * documents, the press that seals them (taken by the browser that asked
 * for it, wiz.c), the account step, and the log bundle the page offers
 * when a step stops (sanitized, cb_logs_export). Nothing else answers the
 * token alone in that window: not the settings, the tokens, the camera
 * key, the cameras' writes, a package, the boot slot, the factory
 * return, nor the setup's machine checks, which come after the account. */
static const char *const SETUP_ROUTES[][2] = {
    { "POST", "/wiz/advisories/accept" },
    { "POST", "/wiz/advisories/press" },
    { "POST", "/wiz/advisories/press/take" },
    { "POST", "/wiz/advisories/press/cancel" },
    { "POST", "/wiz/account" },
    { "POST", "/logs/export" },
};

static int setup_route(void)
{
    if (!route_method || !route_path)
        return 0;
    for (size_t i = 0; i < sizeof(SETUP_ROUTES) / sizeof(*SETUP_ROUTES); i++)
        if (!strcmp(route_method, SETUP_ROUTES[i][0]) && !strcmp(route_path, SETUP_ROUTES[i][1]))
            return 1;
    return 0;
}

#define FAULT_WORDS "the account record cannot be read: hold the machine's button " \
                    "through a restart to make the account again"

/* Who may change state, beyond the token: a logged-in session, this
 * host, a dev image, or, while no account exists, a setup route. *why
 * says which refusal it is (never "login" where no login can help: the
 * pages send a "login" refusal to the login page). */
static int writer_ok(const struct _u_request *req, const char **why)
{
    *why = "login required";
    if (auth_session_ok(req) || auth_peer_local(req) || auth_dev_image())
        return 1;
    if (users_exist())
        return 0;
    if (users_fault()) {
        *why = FAULT_WORDS;
        return 0;
    }
    if (setup_route()) {
        judged_setup = 1;
        return 1;
    }
    *why = "make the account first";
    return 0;
}

/* The panel token reads wherever it writes: a dev image, and a machine
 * with no account (whose page serves it to anyone). */
static int token_reader(const struct _u_request *req)
{
    if (!token_eq(u_map_get_case(req->map_header, "X-ForgeFIRM-Token")))
        return 0;
    return auth_dev_image() || (!users_exist() && !users_fault());
}

/* panel_open_reads: 1 when the file or the key is absent (the default),
 * the value when it is 0 or 1, and 0 (closed) for anything else: a file
 * that cannot be read, a value too long, or one that is neither. */
static int reads_open(void)
{
    char v[8];
    int rc = settings_get("panel_open_reads", v, sizeof(v));
    if (rc == SETTINGS_ABSENT)
        return 1;
    return rc == 0 && !strcmp(v, "1");
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
    char why[96];
    int scoped = scoped_judge(req, route_cap, route_plain, why, sizeof(why));
    if (scoped >= 0)
        return scoped ? 1 : deny(res, 403, why);
    if (camkey_presented(req))
        return 1;
    if (!origin_ok(req))
        return deny(res, 403, "request origin refused");
    if (reads_open())
        return 1;
    if (auth_session_ok(req) || auth_peer_local(req) || token_reader(req))
        return 1;
    return deny(res, 403, "login required");
}

int auth_plain_write_refused(const struct _u_request *req, struct _u_response *res)
{
    char why[96];
    if (scoped_judge(req, route_cap, 1, why, sizeof(why)) != 0)
        return 0;
    deny(res, 403, why);
    return 1;
}

int auth_write_permitted_cap(const struct _u_request *req, const char *cap)
{
    char why[96];
    /* Plain HTTP is not the sink's to know: the route, which answers the
     * request, is reached over it from this host alone. */
    int scoped = scoped_judge(req, cap, 0, why, sizeof(why));
    return scoped >= 0 ? scoped : auth_write_permitted(req);
}

int auth_write_permitted(const struct _u_request *req)
{
    int in_url;
    if (scoped_presented(req, NULL, &in_url))
        return 0;
    if (!origin_ok(req))
        return 0;
    const char *why;
    return token_eq(u_map_get_case(req->map_header, "X-ForgeFIRM-Token")) && writer_ok(req, &why);
}

int auth_write_ok(const struct _u_request *req, struct _u_response *res)
{
    char why[96];
    int scoped = scoped_judge(req, route_cap, route_plain, why, sizeof(why));
    if (scoped >= 0)
        return scoped ? 1 : deny(res, 403, why);
    if (!origin_ok(req))
        return deny(res, 403, "request origin refused");
    if (!token_eq(u_map_get_case(req->map_header, "X-ForgeFIRM-Token")))
        return deny(res, 403, "authentication required");
    const char *refusal;
    if (!writer_ok(req, &refusal))
        return deny(res, 403, refusal);
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
    /* The switch device, or the test hook GF_SWITCH_DEV in a host-test
     * run (testhook.h), as button.c reads it. */
    const char *hook = ff_test_hook("GF_SWITCH_DEV");
    int fd = open(hook ? hook : SWITCH_DEV, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 0;
    int ok = ioctl(fd, EVIOCGSW(sizeof(sw)), sw) >= 0;
    close(fd);
    return ok && (sw[SW_BIT_BUTTON / 8] & (1u << (SW_BIT_BUTTON % 8)));
}
