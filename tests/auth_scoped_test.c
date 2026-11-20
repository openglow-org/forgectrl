/*
 * auth_scoped_test.c - host test for the guards' judgment of a scoped token
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * auth.c with the real token store and real ulfius requests; the
 * account, the sessions, the camera key, and the settings are stubs that
 * say the hard case: an account exists, nobody is logged in, and the
 * client is on the LAN. Cases: a token that holds the route's capability
 * passes a read and a write with no session and whatever Host it sends; a
 * token that does not is refused with the capability named; a route with
 * no capability refuses every token; the route's capability is gone once
 * the route's callback is over; plain HTTP refuses a token from the LAN
 * and takes it from this host; a camera route asks for the camera the
 * request names, and takes the token as ?key=, which no other route
 * does, and only from a token that holds cameras and nothing else; a
 * token that is not this machine's is refused and never falls through to
 * the open reads; the upload sink's silent form; closed reads open to
 * machine.read alone.
 *
 * The panel token: with an account and no session, a LAN write with the
 * right token is refused as a login, by the guard and by the sink's
 * form; a token one short, its first character, or one character long
 * is refused where the right one passes; it is never taken from the URL.
 * The extension host's credential and its viewer header from the LAN
 * are not the host. With no account, the token alone writes on the
 * setup page's routes and on no other (this host spared, and the sink's
 * form, which runs on no route, refused); an account record in fault
 * lets it write nowhere. The token reads where it writes, whatever
 * panel_open_reads says; an unreadable or unparsable panel_open_reads
 * closes the reads, an absent one leaves them open. A scoped token
 * presented to a write route on the plain listener from the LAN is
 * answered there (refused), and a request with none is not.
 *
 * The loopback peer check behind auth.c's loopback-only rules (peer.c):
 * a dual-stack listener reports an IPv4 client as a v4-mapped
 * IPv6 peer, so 127/8, ::1 and mapped 127 pass, and a LAN address in
 * either family, a link-local, ffff and 127 without the twelve-byte
 * mapped prefix, another family, NULL, and a sockaddr_in6
 * whose address bytes were lost to a sizeof(struct sockaddr) copy (what
 * an unpatched ulfius hands over) fail closed.
 */
#define _GNU_SOURCE
#include "../src/auth.h"
#include "../src/peer.h"
#include "../src/settings.h"
#include "../src/tokens.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* ---- what auth.c asks of the rest of the daemon ---- */
static int reads_rc = SETTINGS_ABSENT;      /* panel_open_reads: settings_get's answer */
static const char *reads_v = "1";
static int have_account = 1, account_fault;
int users_exist(void) { return have_account; }
int users_fault(void) { return account_fault; }
int session_valid(const char *id, const char *addr) { (void)id; (void)addr; return 0; }
int session_from_cookie(const char *cookie, char *id, size_t len) { (void)cookie; (void)id; (void)len; return 0; }
int camkey_valid(const char *key) { (void)key; return 0; }
int settings_get(const char *key, char *val, size_t len)
{
    if (strcmp(key, "panel_open_reads"))
        return SETTINGS_ABSENT;
    if (reads_rc == 0)
        snprintf(val, len, "%s", reads_v);
    return reads_rc;
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static struct _u_request req;
static struct _u_response res;

static void fresh(const char *peer)
{
    ulfius_clean_request(&req);
    ulfius_clean_response(&res);
    ulfius_init_request(&req);
    ulfius_init_response(&res);
    struct sockaddr_in *sa = calloc(1, sizeof(*sa));
    sa->sin_family = AF_INET;
    inet_pton(AF_INET, peer, &sa->sin_addr);
    req.client_address = (struct sockaddr *)sa;
    u_map_put(req.map_header, "Host", "192.168.1.20");
}

static void bearer(const char *token)
{
    char v[96];
    snprintf(v, sizeof(v), "Bearer %s", token);
    u_map_put(req.map_header, "Authorization", v);
}

static const char *body(void)
{
    static char b[256];
    snprintf(b, sizeof(b), "%u %.*s", (unsigned)res.status, (int)res.binary_body_length,
             res.binary_body ? (const char *)res.binary_body : "");
    return b;
}

/* One guarded route, the way main.c's wrapper calls it. */
static int on_route(const char *cap, int plain, int write)
{
    auth_route_begin(cap, plain);
    int ok = write ? auth_write_ok(&req, &res) : auth_read_ok(&req, &res);
    auth_route_end();
    return ok;
}

/* A write on a named route, the wrapper's way: 1 when it passed; *setup
 * says whether it passed as the setup page's no-account write. */
static int write_at(const char *method, const char *path, int *setup)
{
    auth_route_begin(NULL, 0);
    auth_route_at(method, path);
    int ok = auth_write_ok(&req, &res);
    if (setup)
        *setup = auth_by_setup();
    auth_route_end();
    return ok;
}

static void panel_token(const char *t)
{
    u_map_put(req.map_header, "X-ForgeFIRM-Token", t);
}

#define LAN "192.168.1.50"

static int peer_v4(const char *ip)
{
    struct sockaddr_in s = { .sin_family = AF_INET };
    inet_pton(AF_INET, ip, &s.sin_addr);
    return peer_is_loopback((const struct sockaddr *)&s);
}

static int peer_v6(const char *ip)
{
    struct sockaddr_in6 s = { .sin6_family = AF_INET6 };
    inet_pton(AF_INET6, ip, &s.sin6_addr);
    return peer_is_loopback((const struct sockaddr *)&s);
}

static void test_peer(void)
{
    CHECK(peer_v4("127.255.0.3"), "127.255.0.3 (all of 127/8) is not this host");
    CHECK(peer_v6("::1"), "::1 is not this host");
    CHECK(peer_v6("::ffff:127.0.0.1"), "::ffff:127.0.0.1 (v4-mapped, a dual-stack listener) is not this host");
    CHECK(!peer_v4("192.0.2.5"), "192.0.2.5 (LAN) passed");
    CHECK(!peer_v6("::ffff:192.0.2.5"), "::ffff:192.0.2.5 (mapped LAN) passed");
    CHECK(!peer_v6("fe80::1"), "fe80::1 (link-local) passed");
    CHECK(!peer_v6("1::ffff:7f00:1"), "1::ffff:7f00:1 (ffff and 127 without the mapped prefix) passed");
    struct sockaddr_un un = { .sun_family = AF_UNIX };
    CHECK(!peer_is_loopback((const struct sockaddr *)&un), "AF_UNIX passed");
    CHECK(!peer_is_loopback(NULL), "NULL passed");

    /* A copy of sizeof(struct sockaddr) keeps the family and the first
     * eight address bytes; the rest reads as zero here (it is heap
     * garbage in the unpatched library). The check must fail closed. */
    struct sockaddr_in6 full = { .sin6_family = AF_INET6 };
    inet_pton(AF_INET6, "::ffff:127.0.0.1", &full.sin6_addr);
    struct sockaddr_in6 cut;
    memset(&cut, 0, sizeof(cut));
    memcpy(&cut, &full, sizeof(struct sockaddr));
    CHECK(!peer_is_loopback((const struct sockaddr *)&cut),
          "::ffff:127.0.0.1 cut to sizeof(struct sockaddr) passed");
}

int main(void)
{
    char root[128], hub[TOKENS_TEXT_LEN + 1], pend[TOKENS_TEXT_LEN + 1], view[TOKENS_TEXT_LEN + 1];
    char id[TOKENS_ID_HEX + 1], err[160];
    test_peer();

    snprintf(root, sizeof(root), "/tmp/auth_scoped_test.%d", (int)getpid());
    mkdir(root, 0755);
    setenv("FORGECTRL_DATA_DIR", root, 1);
    tokens_init();
    if (tokens_create("hub", "machine.read,camera.lid", hub, id, err, sizeof(err)) != 0 ||
        tokens_create("jogger", "motion.jog,motion.job", pend, id, err, sizeof(err)) != 0 ||
        tokens_create("viewer", "camera.lid", view, id, err, sizeof(err)) != 0) {
        printf("cannot create the tokens: %s\n", err);
        return 1;
    }
    ulfius_init_request(&req);
    ulfius_init_response(&res);

    /* The hard case without a token: a write from the LAN is refused. */
    fresh(LAN);
    CHECK(!on_route("motion.jog", 0, 1), "a write with no credential passed");

    /* A held capability: no session, no panel token, and it passes. */
    fresh(LAN); bearer(hub);
    CHECK(on_route("machine.read", 0, 0), "a held read: %s", body());
    fresh(LAN); bearer(pend);
    CHECK(on_route("motion.jog", 0, 1), "a held write: %s", body());
    fresh(LAN); u_map_put(req.map_header, "X-ForgeFIRM-Token", pend);
    CHECK(on_route("motion.job", 0, 1), "the panel token's header carries a scoped token too: %s", body());
    /* The origin checks are a browser's; a token's client sends what Host it likes. */
    fresh(LAN); bearer(hub); u_map_put(req.map_header, "Host", "forge.example.net");
    CHECK(on_route("machine.read", 0, 0), "a token behind a DNS name: %s", body());
    fresh(LAN); u_map_put(req.map_header, "Host", "forge.example.net");
    CHECK(!on_route("machine.read", 0, 0) && strstr(body(), "origin"), "a DNS name with no token: %s", body());

    /* Not held, and no capability at all. */
    fresh(LAN); bearer(hub);
    CHECK(!on_route("motion.jog", 0, 1) && strstr(body(), "403") && strstr(body(), "does not hold motion.jog"),
          "a capability not held: %s", body());
    fresh(LAN); bearer(pend);
    CHECK(!on_route(NULL, 0, 1) && strstr(body(), "no scoped token reaches this route"), "a route with none: %s", body());
    fresh(LAN); bearer(hub);
    CHECK(!on_route(NULL, 0, 0) && strstr(body(), "no scoped token reaches"), "an open read with none: %s", body());
    /* Outside a route's callback there is no capability to hold: the
     * one the last route had is gone with it (a connection is kept alive
     * across requests, on one thread). */
    fresh(LAN); bearer(hub);
    CHECK(on_route("machine.read", 0, 0), "a held read: %s", body());
    fresh(LAN); bearer(hub);
    CHECK(!auth_read_ok(&req, &res), "a guard outside any route passed a token");

    /* Plain HTTP. */
    fresh(LAN); bearer(hub);
    CHECK(!on_route("machine.read", 1, 0) && strstr(body(), "HTTPS only"), "plain HTTP from the LAN: %s", body());
    fresh("127.0.0.1"); bearer(hub);
    CHECK(on_route("machine.read", 1, 0), "plain HTTP from this host: %s", body());

    /* The cameras. */
    fresh(LAN); bearer(hub);
    CHECK(on_route("camera", 0, 0), "the lid camera by default: %s", body());
    fresh(LAN); bearer(hub); u_map_put(req.map_url, "cam", "head");
    CHECK(!on_route("camera", 0, 0) && strstr(body(), "does not hold camera.head"), "the head camera: %s", body());
    fresh(LAN); bearer(hub);
    CHECK(on_route("camera.any", 0, 0), "either camera: %s", body());
    fresh(LAN); bearer(pend);
    CHECK(!on_route("camera.any", 0, 0), "a token with no camera: %s", body());
    /* In a URL: a camera-only token, on a camera route, and nothing else. */
    fresh(LAN); u_map_put(req.map_url, "key", view);
    CHECK(on_route("camera", 0, 0), "?key= on a camera route: %s", body());
    fresh(LAN); u_map_put(req.map_url, "key", view); u_map_put(req.map_url, "cam", "head");
    CHECK(!on_route("camera", 0, 0) && strstr(body(), "does not hold camera.head"), "?key= for the other camera: %s", body());
    fresh(LAN); u_map_put(req.map_url, "key", hub);
    CHECK(!on_route("camera", 0, 0) && strstr(body(), "in a URL may hold camera capabilities only"),
          "a token with more than a camera, in a URL: %s", body());
    fresh(LAN); bearer(hub);
    CHECK(on_route("camera", 0, 0), "the same token in a header: %s", body());
    fresh(LAN); u_map_put(req.map_url, "key", pend);
    CHECK(!on_route("motion.jog", 0, 1), "?key= on a write route passed: %s", body());
    fresh(LAN); u_map_put(req.map_url, "token", pend);
    CHECK(!on_route("motion.jog", 0, 1), "?token= carried a scoped token: %s", body());

    /* Not this machine's: refused, and not by falling through to the open reads. */
    fresh(LAN); bearer("fft_00000000000000000000000000000000");
    CHECK(!on_route("machine.read", 0, 0) && strstr(body(), "authentication required"), "a stranger's token: %s", body());

    /* The sink's silent form. */
    fresh(LAN); bearer(pend);
    CHECK(auth_write_permitted_cap(&req, "motion.job") == 1, "the job's sink refused the job's token");
    CHECK(auth_write_permitted(&req) == 0, "a sink with no capability took a token");
    fresh(LAN); bearer(hub);
    CHECK(auth_write_permitted_cap(&req, "motion.job") == 0, "the job's sink took a token without motion.job");

    /* Reads closed to the LAN open to machine.read, and to nothing else. */
    reads_rc = 0; reads_v = "0";
    fresh(LAN);
    CHECK(!on_route("machine.read", 0, 0) && strstr(body(), "login required"), "closed reads, nobody: %s", body());
    fresh(LAN); bearer(hub);
    CHECK(on_route("machine.read", 0, 0), "closed reads, machine.read: %s", body());
    fresh(LAN); bearer(pend);
    CHECK(!on_route("machine.read", 0, 0), "closed reads, a token without it: %s", body());

    /* panel_open_reads: unreadable or neither 0 nor 1 closes; absent opens. */
    reads_rc = SETTINGS_UNREADABLE;
    fresh(LAN);
    CHECK(!on_route("machine.read", 0, 0), "an unreadable panel_open_reads left the reads open");
    reads_rc = 0; reads_v = "yes";
    fresh(LAN);
    CHECK(!on_route("machine.read", 0, 0), "panel_open_reads=yes left the reads open");
    reads_rc = SETTINGS_ABSENT;
    fresh(LAN);
    CHECK(on_route("machine.read", 0, 0), "an absent panel_open_reads closed the reads: %s", body());

    /* The panel token, an account and no session, from the LAN. */
    auth_init();
    const char *tok = auth_token();
    char shorter[64], first[2], longer[64];
    snprintf(shorter, sizeof(shorter), "%.*s", (int)strlen(tok) - 1, tok);
    snprintf(first, sizeof(first), "%c", tok[0]);
    snprintf(longer, sizeof(longer), "%s0", tok);
    fresh(LAN); panel_token(tok);
    CHECK(!on_route(NULL, 0, 1) && strstr(body(), "login required"), "the panel token wrote from the LAN with no session: %s", body());
    CHECK(auth_write_permitted(&req) == 0, "the sink's form took the panel token from the LAN with no session");
    fresh("127.0.0.1"); panel_token(tok);
    CHECK(on_route(NULL, 0, 1), "the right token from this host: %s", body());
    fresh("127.0.0.1"); panel_token(shorter);
    CHECK(!on_route(NULL, 0, 1), "a token one character short passed");
    fresh("127.0.0.1"); panel_token(first);
    CHECK(!on_route(NULL, 0, 1), "the token's first character passed");
    fresh("127.0.0.1"); panel_token(longer);
    CHECK(!on_route(NULL, 0, 1), "the token with one character more passed");
    fresh("127.0.0.1"); u_map_put(req.map_url, "token", tok);
    CHECK(!on_route(NULL, 0, 1) && strstr(body(), "authentication required"), "the panel token was taken from ?token=: %s", body());
    CHECK(auth_write_permitted(&req) == 0, "the sink's form took the panel token from ?token=");

    /* The extension host's credential and its viewer header, from the LAN. */
    char hostfile[160], hosttok[TOKENS_TEXT_LEN + 2] = "";
    snprintf(hostfile, sizeof(hostfile), "%s/host.token", root);
    setenv("FORGECTRL_EXT_HOST_TOKEN", hostfile, 1);
    FILE *hf = tokens_host_mint(err, sizeof(err)) == 0 ? fopen(hostfile, "r") : NULL;
    if (hf) {
        if (!fgets(hosttok, sizeof(hosttok), hf))
            hosttok[0] = '\0';
        fclose(hf);
    }
    hosttok[strcspn(hosttok, "\n")] = '\0';
    CHECK(hosttok[0], "no host credential was made: %s", err);
    fresh(LAN); bearer(hosttok);
    CHECK(!on_route("motion.jog", 0, 1) && strstr(body(), "not accepted from here"), "the host's credential from the LAN: %s", body());
    fresh("127.0.0.1"); bearer(hosttok);
    CHECK(on_route("motion.jog", 0, 1), "the host's credential from this host: %s", body());
    fresh("127.0.0.1"); bearer(hosttok);
    CHECK(on_route("host", 0, 1), "the host's own route took no host: %s", body());
    fresh(LAN); bearer(pend);
    CHECK(!on_route("host", 0, 1) && strstr(body(), "does not hold host"), "an operator's token on the host's own route: %s", body());
    fresh(LAN); u_map_put(req.map_header, "X-ForgeFIRM-Client", "extension-host");
    CHECK(!auth_local_viewer(&req), "the host's viewer header from the LAN made a local viewer");

    /* No account: the setup page's routes, and no other. */
    have_account = 0;
    int setup = 0;
    fresh(LAN); panel_token(tok);
    CHECK(write_at("POST", "/wiz/account", &setup) && setup, "no account: the account step refused the token: %s", body());
    fresh(LAN); panel_token(tok);
    CHECK(write_at("POST", "/wiz/advisories/press/take", NULL), "no account: the press's take refused the token: %s", body());
    fresh(LAN); panel_token(tok);
    CHECK(write_at("POST", "/logs/export", &setup) && setup, "no account: the log bundle refused the token: %s", body());
    fresh(LAN); panel_token(tok);
    CHECK(!write_at("POST", "/settings", NULL) && strstr(body(), "make the account first"),
          "no account: a settings write passed on the token: %s", body());
    fresh(LAN); panel_token(tok);
    CHECK(!write_at("POST", "/tokens", NULL), "no account: a scoped token was made on the panel token");
    fresh(LAN); panel_token(tok);
    CHECK(!write_at("GET", "/system/camera-key", NULL), "no account: the camera key was read on the panel token");
    fresh(LAN); panel_token(tok);
    CHECK(!write_at("POST", "/restore/factory-return", NULL), "no account: the factory return passed on the token");
    fresh(LAN); panel_token(tok);
    CHECK(!write_at("POST", "/wiz/motion/start", NULL), "no account: a machine check started on the token");
    fresh(LAN); panel_token(tok);
    CHECK(auth_write_permitted(&req) == 0, "no account: an upload sink took the panel token");
    fresh("127.0.0.1"); panel_token(tok);
    CHECK(write_at("POST", "/settings", &setup) && !setup, "no account: this host's settings write refused: %s", body());
    /* The token reads where it writes, the reads closed. */
    reads_rc = 0; reads_v = "0";
    fresh(LAN); panel_token(tok);
    CHECK(on_route(NULL, 0, 0), "no account, reads closed: the panel token could not read: %s", body());
    fresh(LAN);
    CHECK(!on_route(NULL, 0, 0), "no account, reads closed: a read with no token passed");
    have_account = 1;
    fresh(LAN); panel_token(tok);
    CHECK(!on_route(NULL, 0, 0), "an account, reads closed: the panel token read with no session");
    /* An account record in fault: nothing on the token alone. */
    have_account = 0; account_fault = 1;
    fresh(LAN); panel_token(tok);
    CHECK(!write_at("POST", "/wiz/account", NULL) && strstr(body(), "cannot be read"),
          "a record in fault: the account step passed on the token: %s", body());
    fresh(LAN); panel_token(tok);
    CHECK(!on_route(NULL, 0, 0), "a record in fault, reads closed: the panel token read");
    fresh("127.0.0.1"); panel_token(tok);
    CHECK(write_at("POST", "/wiz/account", NULL), "a record in fault: this host refused: %s", body());
    have_account = 1; account_fault = 0;
    reads_rc = SETTINGS_ABSENT;

    /* A write route on the plain listener, from the LAN. */
    fresh(LAN); bearer(pend);
    auth_route_begin("motion.job", 1);
    int answered = auth_plain_write_refused(&req, &res);
    auth_route_end();
    CHECK(answered && strstr(body(), "403") && strstr(body(), "HTTPS only"), "a scoped token on plain HTTP: %d %s", answered, body());
    fresh(LAN); panel_token(tok);
    auth_route_begin("motion.job", 1);
    answered = auth_plain_write_refused(&req, &res);
    auth_route_end();
    CHECK(!answered, "a request with no scoped token was answered on the plain listener");

    ulfius_clean_request(&req);
    ulfius_clean_response(&res);
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) { }
    printf(fails ? "auth_scoped_test: %d FAILED\n" : "auth_scoped_test: all passed\n", fails);
    return fails ? 1 : 0;
}
