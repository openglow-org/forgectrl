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
#endif
