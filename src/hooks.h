/*
 * hooks.h - functions main.c provides to the other modules
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_HOOKS_H
#define FORGECTRL_HOOKS_H
#include <stddef.h>

struct _u_request;

/* Validate one settings key and value against the settings table (the
 * same validators POST /settings runs). Returns 1 when key is known and
 * the value is legal, 0 otherwise; an empty value (clear) is legal for
 * every known key. */
int setting_valid(const char *key, const char *val);
/* Apply the wifi_country setting to the radio (set_region = 1 to push a
 * region change; 0 for the startup pass). */
void apply_wifi(int set_region);
/* The fuse serial (0 when unreadable) and the firmware version string. */
unsigned long fuse_serial(void);
void read_fw_version(char *buf, size_t len);
/* The dev bar for a page with its own styles (it carries them inline), or
 * "" on a release image. */
const char *page_dev_bar_inline(void);
/* Whether a job would take a press of the machine button as its own: a
 * job armed and waiting for its press (a GRBL arm, a cloud print) or
 * inside its armed window, or a GRBL job running or paused, where the
 * button is the pause and resume; and a live GRBL controller whose state
 * file cannot be read or is not current (jobstream.h), which could hide
 * any of those. Every route that takes a press of the button as proof of
 * presence (panel_consent_take) asks this first and answers 409 with @why,
 * so the press it would take is never a job's. Returns 1 with the refusal
 * in @why, 0 with none. */
int job_takes_button(char *why, size_t len);
/* The press the request's consent was given (btnres.h, the panel's
 * consent), taken once: the request names its dialog's consent
 * (consent=<nonce>), registered through POST /button/reserve for @kind and
 * for what the request itself names (consent_subject in main.c: an
 * install's digest, a key's name and text, the firmware's file, slot,
 * SHA-256 and what it skips, the factory return's slot),
 * by the same session. 0 when it is taken; 403 with @why when no consent is
 * named or its button was not pressed yet (the operator may still press);
 * 409 with @why for one that is not standing, has ended, or names another
 * kind, subject or holder (which ends it, and is logged with the peer); 400
 * for a kind no consent has. The kinds are main.c's consent_subject: a
 * route that takes a press for a new action adds its kind there. */
int panel_consent_take(const struct _u_request *req, const char *kind, char *why, size_t len);
#endif
