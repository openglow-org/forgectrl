/*
 * mcode.h - the M-codes extension packages answer: the relay between the GRBL controller and the extension host
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * M160 to M179 belong to extension packages: a package that holds
 * mcode:<n> answers M<n> in a job. The GRBL controller parses one only
 * while the daemon says something answers it (the port's mcodes), so a
 * job that names one nothing answers stops where the line is parsed. One
 * that is answered is a barrier: the controller drains the planner, puts
 * the M-code in its port's state, and waits (grblHAL-glowforge's
 * glowforge_mcode.c).
 *
 * This relay is the one thread that talks to both sides. While the GRBL
 * controller runs it tells the controller which numbers the extension
 * host's running services answer, and while something answers one it
 * watches the port's state: an M-code waiting there goes to the host
 * (forgeext mcode), and the service's answer goes back to the controller
 * (mcode_result). A 2xx from the service lets the job go on; anything else,
 * the host's refusal included, holds the job with the words. The relay
 * sends nothing a request could steer: both operations are the daemon
 * set's, which no route reaches.
 */
#ifndef FORGECTRL_MCODE_H
#define FORGECTRL_MCODE_H

#include <stddef.h>

#define MCODE_POLL_S    0.1     /* while something answers an M-code and a sender is connected */
#define MCODE_IDLE_S    1.0     /* otherwise */
#define MCODE_REPUSH_S  2.0     /* the table again, for a controller that restarted */
#define MCODE_TEXT_MAX  96      /* what the controller takes as an answer's words */

/* Start the relay. */
void mcode_init(void);

/* The port's mcode_result argument for the M-code under seq: from the
 * host's answer (doc, its JSON: status and body), or, with doc NULL, from
 * the host's refusal (why). A 2xx status is ok with the body's "message";
 * anything else is fail with the body's "error" (or its "message", or the
 * status). The words are held to the port's form: printable, brackets
 * turned into parentheses, at most MCODE_TEXT_MAX bytes. */
void mcode_answer_line(unsigned seq, const char *doc, const char *why, char *out, size_t len);

#endif
