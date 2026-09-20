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
 */
#define _GNU_SOURCE
#include "../src/auth.h"
#include "../src/tokens.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- what auth.c asks of the rest of the daemon ---- */
static int open_reads = 1;
int users_exist(void) { return 1; }
int session_valid(const char *id) { (void)id; return 0; }
int session_from_cookie(const char *cookie, char *id, size_t len) { (void)cookie; (void)id; (void)len; return 0; }
int camkey_valid(const char *key) { (void)key; return 0; }
int settings_get_bool(const char *key, int def) { return strcmp(key, "panel_open_reads") ? def : open_reads; }

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

#define LAN "192.168.1.50"

int main(void)
{
    char root[128], hub[TOKENS_TEXT_LEN + 1], pend[TOKENS_TEXT_LEN + 1], view[TOKENS_TEXT_LEN + 1];
    char id[TOKENS_ID_HEX + 1], err[160];
    snprintf(root, sizeof(root), "/tmp/auth_scoped_test.%d", (int)getpid());
    mkdir(root, 0755);
    setenv("FORGECTRL_DATA_DIR", root, 1);
    tokens_init();
    if (tokens_create("hub", "machine.read,camera.lid", hub, id, err, sizeof(err)) != 0 ||
        tokens_create("pendant", "motion.jog,motion.job", pend, id, err, sizeof(err)) != 0 ||
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
    open_reads = 0;
    fresh(LAN);
    CHECK(!on_route("machine.read", 0, 0) && strstr(body(), "login required"), "closed reads, nobody: %s", body());
    fresh(LAN); bearer(hub);
    CHECK(on_route("machine.read", 0, 0), "closed reads, machine.read: %s", body());
    fresh(LAN); bearer(pend);
    CHECK(!on_route("machine.read", 0, 0), "closed reads, a token without it: %s", body());

    ulfius_clean_request(&req);
    ulfius_clean_response(&res);
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) { }
    printf(fails ? "auth_scoped_test: %d FAILED\n" : "auth_scoped_test: all passed\n", fails);
    return fails ? 1 : 0;
}
