/*
 * slotcalc.c - forgectrl: what the slot inventory says about the next boot (see slotcalc.h)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "slotcalc.h"

#include <stdio.h>
#include <string.h>

/* The slot whose line `slot.<name>.<what>=yes` stands in the inventory, or
 * 0 with none. */
static int slot_with(const char *list, const char *what, char *name, size_t len)
{
    char suffix[24];
    snprintf(suffix, sizeof(suffix), ".%s=yes", what);
    for (const char *p = list; p && *p; ) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        size_t ls = strlen(suffix);
        if (n > 5 + ls && !strncmp(p, "slot.", 5) && !strncmp(p + n - ls, suffix, ls)) {
            size_t k = n - 5 - ls;
            if (k < len) {
                memcpy(name, p + 5, k);
                name[k] = '\0';
                return 1;
            }
        }
        p = eol ? eol + 1 : NULL;
    }
    return 0;
}

int slots_next_boot_returns(const char *ffboot_list, char *why, size_t len)
{
    char booted[32], next[32];
    if (!slot_with(ffboot_list, "booted", booted, sizeof(booted))) {
        snprintf(why, len, "the next boot cannot be read");
        return 0;
    }
    if (!slot_with(ffboot_list, "next", next, sizeof(next)) || strcmp(next, booted)) {
        snprintf(why, len, "the next boot is set to the other slot: set it back to this one first");
        return 0;
    }
    return 1;
}
