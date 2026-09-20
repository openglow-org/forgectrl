/*
 * grblport.h - the daemon's client of the GRBL controller's port
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The controller port is a Unix socket the GRBL controller serves beside
 * the Grbl socket: it jogs and reads state without displacing the sender.
 * The daemon is its only client, and holds one connection for as long as
 * the controller lives, because the port treats its client as the jog's
 * dead-man: a client that goes away cancels the jog in flight.
 *
 * The port's operations come in two sets, and the caller names the set
 * it speaks for before anything else looks at the request. The package
 * set (state, jog, cancel) is what the panel's Jog card, a scoped token,
 * or an extension may reach. The panel set adds the operations that belong to the
 * operator's own panel alone: the motor release, the energize, and the
 * manual home. An operation outside the caller's set is refused here,
 * with nothing written to the socket.
 */
#ifndef FORGECTRL_GRBLPORT_H
#define FORGECTRL_GRBLPORT_H

#include <stddef.h>

typedef enum {
    GRBLPORT_SET_PACKAGE = 0,
    GRBLPORT_SET_PANEL
} grblport_set_t;

typedef enum {
    GRBLPORT_STATE = 0,
    GRBLPORT_JOG,               /* arg: the tail of a $J= line (grblport_jog_words) */
    GRBLPORT_CANCEL,
    GRBLPORT_RELEASE,           /* panel set only, and the two below */
    GRBLPORT_ENERGIZE,
    GRBLPORT_HOME
} grblport_op_t;

#define GRBLPORT_OK          0  /* the port answered: reply holds its line */
#define GRBLPORT_UNREACHABLE (-1)   /* no controller on the port, or no answer: reply says which */
#define GRBLPORT_FORBIDDEN   (-2)   /* the operation is not in the caller's set: nothing was sent */

/* One request, one reply line (without its newline). Serialized: the
 * port takes one request at a time. */
int grblport_request(grblport_set_t set, grblport_op_t op, const char *arg,
                     char *reply, size_t len);

/* The most one jog request may ask for, which is the jog's dead-man: a
 * client that vanishes leaves at most this much motion behind it. */
#define GRBLPORT_JOG_MAX_XY_MM 100.0
#define GRBLPORT_JOG_MAX_Z_MM  5.0
#define GRBLPORT_JOG_FEED_MIN  10.0
#define GRBLPORT_JOG_FEED_MAX  12000.0

/* The words of a relative, metric jog. An axis whose increment is zero
 * is left out. Returns 0, or -1 with the reason in err: no increment at
 * all, an increment past its bound, a feed outside its range, or a
 * number that is not finite. */
int grblport_jog_words(double dx, double dy, double dz, double feed,
                       char *buf, size_t len, char *err, size_t elen);

/* What a port reply means to an HTTP client: 0 for ok, else the status
 * (409 or 502) with the reason in words. */
int grblport_explain(const char *reply, char *why, size_t len);

/* Drops the connection; the next request connects again. */
void grblport_close(void);

#endif
