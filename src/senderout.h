/*
 * senderout.h - an extension package keeps the Grbl sender out
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A package the operator granted sender.keep_out may ask, through the
 * extension host, to keep the Grbl sender out while it uses the machine:
 * the alignment tool moves the head away from where a job would start,
 * and a sender must not start one from there. The GRBL controller does
 * the keeping (its port's "sender out"): only on an idle machine, the
 * connected sender dropped, and every sender from the network turned away
 * until it is told otherwise; one on this host, the daemon's own job
 * runner, still connects. This module is the daemon's side of it and the
 * one authority on who keeps the sender out:
 *
 *  - One package at a time. It holds the machine lease as "ext:<id>"
 *    (LEASE_EXTENSION) for as long, so the panel's controls lock, and
 *    only the package's own jogs and jobs pass (lease_refusal_locks_for).
 *  - The claim lives while the extension host keeps it fresh: the host
 *    rewrites <run>/sender-out/<id>.json twice a second while the
 *    package's service runs and says "out", in the holds' own form
 *    ({"id", "required", "raised", "reason", "ts_mono"}; raised is the
 *    claim). A claim that goes stale ends:
 *    the service stopped, crashed, or was turned off, or the host went
 *    away. Nothing keeps the sender out past its keeper, and nothing
 *    moves the head when it ends: the notice says so to the operator.
 *  - The operator ends it from the panel at any time. The package's
 *    claim is then refused until the package itself says "in".
 *  - A controller that restarts lets every sender in, so the claim is
 *    pushed to it again. A daemon that starts takes up a fresh claim it
 *    finds, and otherwise tells the controller to let senders in.
 */
#ifndef FORGECTRL_SENDEROUT_H
#define FORGECTRL_SENDEROUT_H

#include <stddef.h>

/* The host test builds with shorter times. */
#ifndef SENDEROUT_FRESH_S
#define SENDEROUT_FRESH_S   2.0     /* a claim file older than this is stale (the holds' rule) */
#endif
#ifndef SENDEROUT_GRACE_S
#define SENDEROUT_GRACE_S   3.0     /* the host's time to write the first one after a claim */
#endif
#define SENDEROUT_TICK_S    0.5
#ifndef SENDEROUT_REPUSH_S
#define SENDEROUT_REPUSH_S  2.0     /* the controller's state is compared this often */
#endif
#define SENDEROUT_ID_MAX    63      /* a package id (forgeext) */

/* A package id as the extension host writes them. */
int senderout_id_ok(const char *id);

/* The package's own, through the host. 0, or an HTTP status with the
 * words in why: 400 for an id that is none, 409 for a refusal (another
 * package keeps the sender out, the operator let it back in on this one,
 * the machine is not idle, a sender is at work, another holds the
 * machine), 503 for a controller that did not answer. Claiming again is
 * a success that changes nothing. */
int senderout_claim(const char *id, char *why, size_t len);
int senderout_end(const char *id, char *why, size_t len);

/* The operator's, from the panel: let the sender back in, whoever keeps
 * it out; and clear the notice of one that stopped while it kept it out. */
int senderout_release(char *why, size_t len);
void senderout_notice_clear(void);

/* "sender_out":{"holder":null|{"id":..,"for_s":..},"notice":null|{"id":..,"why":..}}
 * for /status, and the same object alone for GET /motion/sender. */
int senderout_json(char *buf, size_t len, int with_key);

/* For the package: {"out":bool,"released":bool}, released when the
 * operator let the sender back in on its claim. */
int senderout_pkg_json(const char *id, char *buf, size_t len);

/* The package that keeps the sender out, 1; or 0 with id "". */
int senderout_holder(char *id, size_t len);

/* Take up a fresh claim, and start the keeper. */
void senderout_init(void);

/* One pass of the keeper, for the host test: stale claims, the
 * controller's state. The thread runs it every SENDEROUT_TICK_S. */
void senderout_tick(void);

#endif /* FORGECTRL_SENDEROUT_H */
