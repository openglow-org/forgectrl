/*
 * tokens.h - scoped API tokens: a named credential that reaches what it was granted
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The panel token authorizes every write, and needs a login session
 * behind it. A scoped token is for a client that has neither a browser
 * nor a session: a script, such as one that tells somebody a job has
 * ended. The
 * operator creates it in the panel with a name and a set of
 * capabilities, sees it once, and can revoke it; the daemon keeps its
 * SHA-256, never the token.
 *
 * The capabilities are a closed list, the ones a route serves today.
 * Nothing outside it can be granted, so no token reaches the settings,
 * the mode, the thermal hardware, an update, a wizard, the controller's
 * start and stop, or another token.
 *
 * A token is "fft_" and 32 hex digits, 128 bits from /dev/urandom. The
 * prefix keeps it apart from the panel token wherever either is read.
 */
#ifndef FORGECTRL_TOKENS_H
#define FORGECTRL_TOKENS_H

#include <stddef.h>

#define TOKENS_MAX       16
#define TOKENS_NAME_MAX  40
#define TOKENS_PREFIX    "fft_"
#define TOKENS_TEXT_LEN  36             /* the prefix and 32 hex */
#define TOKENS_ID_HEX    8

/* Load the store. Idempotent; call once at startup before serving. */
void tokens_init(void);
/* Write the last-used times that are newer than the file's. */
void tokens_flush(void);

/* Whether a presented credential claims to be a scoped token (the
 * prefix): such a request is judged as one, and as nothing else. */
int tokens_looks_scoped(const char *presented);

/* The judgment: 1 when presented is a live token that holds cap, 0 when
 * it is a live token that does not (or cap is NULL: a route no scoped
 * token reaches), -1 when it is no token of this machine's. A cap that
 * ends in ".any" is held by a token with any capability under that
 * prefix. The token's id goes to id (TOKENS_ID_HEX + 1 bytes) whenever
 * the token itself is live; a pass marks it used. */
int tokens_check(const char *presented, const char *cap, char *id);

/* Whether presented is a live token whose every capability begins with
 * prefix ("camera."): the rule for a token that arrives in a URL. */
int tokens_holds_only(const char *presented, const char *prefix);

/* Whether cap is one a token can be granted. */
int tokens_cap_known(const char *cap);

/* ---- the extension host's own credential ----
 *
 * The host relays a package's request to routes that are writes, and a
 * write takes a credential. The panel token is not it: it reaches every
 * route, so a flaw in the host or its broker would reach every route
 * too. This is a scoped credential of the same shape as the operator's
 * tokens and none of their substance: it holds only what the host may
 * relay, it is minted fresh every time the daemon starts, it is never in
 * the store and never in the operator's list (it is not theirs to
 * manage or to revoke by accident), and it dies with the daemon.
 *
 * It is written to a file only root can read. An extension account
 * cannot read it, and cannot reach a loopback listener to use it even if
 * it could.
 *
 * What it holds is deliberately small: widening it widens what a flaw in
 * the host reaches, so a capability belongs here only once the host
 * actually relays it. */
#define TOKENS_HOST_FILE "/run/forgefirm/ext-host.token"
#define TOKENS_HOST_CAPS "motion.jog,motion.job"

/* Mint it and write the file. 0, or -1 with the reason in err. */
int tokens_host_mint(char *err, size_t elen);
/* 1 when presented is this daemon's host credential and it holds cap. */
int tokens_host_check(const char *presented, const char *cap);

/* Create a token: caps is a comma-separated list, each from the closed
 * list, at least one. 0 with the token in token (TOKENS_TEXT_LEN + 1
 * bytes; this is the one time it exists outside the client) and its id,
 * or -1 with the reason in err. */
int tokens_create(const char *name, const char *caps, char *token, char *id,
                  char *err, size_t elen);
/* Revoke by id: 0, or -1 with the reason in err. */
int tokens_revoke(const char *id, char *err, size_t elen);

/* {"max":16,"caps":[...],"tokens":[{"id","name","caps":[...],"created",
 * "last_used"}]}: never a token, never a hash. 0, or -1 when it does not
 * fit. */
int tokens_json(char *buf, size_t len);

#endif
