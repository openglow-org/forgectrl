/*
 * slotcalc.h - forgectrl: what the slot inventory says about the next boot
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * Pure, so the host tests run it: the input is the text `ffboot -l` prints
 * (key=value lines, slot.<name>.booted=yes and slot.<name>.next=yes among
 * them).
 */
#ifndef FORGECTRL_SLOTCALC_H
#define FORGECTRL_SLOTCALC_H

#include <stddef.h>

/* Whether the next boot comes back to the system running now, so a request
 * left for it (a reset of every setting) is carried out by this image on
 * this data folder. The saved environment picks every boot, the card's
 * included, so a boot comes back only when the environment names the slot
 * it booted: a card boot with an eMMC slot set next leaves the card. An
 * inventory that names no booted slot is no: the next boot cannot be told.
 * On no, @why says why. */
int slots_next_boot_returns(const char *ffboot_list, char *why, size_t len);

#endif
