/*
 * extpkg.h - the operator's door to extension packages: forgectrl asks the extension host
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * forgectrl is the machine's one front door (TLS, the login, the origin
 * checks), and the extension host (forgeext) is the only program that
 * reads what a package brought. So the panel's package routes are a relay:
 * forgectrl runs the host's command line, which answers in JSON, takes the
 * host's lock around every change, and is what root at the console runs
 * too. The host's daemon picks a change up on its next turn.
 *
 * Nothing of a request reaches a shell: the command is an argument vector,
 * a package id is checked against the form of one before it is an
 * argument, and the actions are a closed list.
 */
#ifndef FORGECTRL_EXTPKG_H
#define FORGECTRL_EXTPKG_H

#include <stddef.h>

#define EXTPKG_BIN_DEFAULT      "/usr/bin/forgeext"
#define EXTPKG_STATUS_FILE      "/run/forgefirm/ext/status.json"
#define EXTPKG_SAFE_FILE        "/run/forgefirm/ext-safe"
#define EXTPKG_OUT_MAX          (256 * 1024)
#define EXTPKG_TIMEOUT_S        60

/* Lowercase reverse-DNS, as the host installs them: 1 when id has the form. */
int extpkg_id_ok(const char *id);

/* Run the host's command line with these arguments (argv[0] is filled in).
 * Its standard output, NUL-ended, into *out (the caller frees it). The
 * exit status, or -1 when it could not be run, did not end in time, or
 * said more than EXTPKG_OUT_MAX. */
int extpkg_cli(const char *const argv[], char **out);

/* The document of GET /ext/status, malloc'd: whether extensions are on,
 * safe mode, the host's own status file (or that no host is running), and
 * the installed packages as the host lists them. NULL when out of memory. */
char *extpkg_status_json(int ext_enabled);

/* One action on one package: enable, disable, remove, remove-keep-data,
 * hold-required, hold-advisory. 0 when the host did it. Otherwise the HTTP
 * status to refuse with (400 for a request that has no such form, 409 for
 * the host's refusal, 502 when the host cannot be asked) and the words. */
int extpkg_action(const char *id, const char *action, int *status, char *why, size_t wlen);

#endif
