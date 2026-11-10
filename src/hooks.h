/*
 * hooks.h - functions main.c provides to the other modules
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_HOOKS_H
#define FORGECTRL_HOOKS_H
#include <stddef.h>

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
 * button is the pause and resume. Every route that asks the operator to
 * hold the button as proof of presence (operator_present, auth.h) asks
 * this first and answers 409 with @why, so the hold it would ask for is
 * never a job's press. Returns 1 with the refusal in @why, 0 with none. */
int job_takes_button(char *why, size_t len);
#endif
