/*
 * slotcalc_test.c - host unit test for what the slot inventory says about the next boot
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A reset of every setting is a request the next boot carries out
 * (forgefirm-data), so it is only taken when the next boot comes back to the
 * system that took it: a boot whose saved environment names the slot it
 * booted, the card's as an eMMC slot's. Anything else would leave the
 * request for some later boot to find.
 */
#include "../src/slotcalc.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    char why[128];
    const char *same = "booted.root=/dev/mmcblk2p1\nslot.a.device=/dev/mmcblk2p1\nslot.a.present=yes\n"
                       "slot.a.booted=yes\nslot.a.next=yes\nslot.b.device=/dev/mmcblk2p2\nslot.b.present=yes\n";
    const char *other = "booted.root=/dev/mmcblk2p1\nslot.a.booted=yes\nslot.b.next=yes\n";
    const char *card = "booted.root=/dev/mmcblk1p1\nslot.sd.device=/dev/mmcblk1p1\nslot.sd.present=yes\n"
                       "slot.sd.booted=yes\nslot.sd.next=yes\nslot.a.device=/dev/mmcblk2p1\nslot.a.present=yes\n";
    const char *card_leaves = "booted.root=/dev/mmcblk1p1\nslot.sd.booted=yes\nslot.a.next=yes\n";
    const char *no_next = "slot.b.booted=yes\n";

    CHECK(slots_next_boot_returns(same, why, sizeof(why)), "an eMMC boot that boots its own slot next: taken");
    why[0] = '\0';
    CHECK(!slots_next_boot_returns(other, why, sizeof(why)) && strstr(why, "other slot"),
          "the next boot set to the other slot: refused, and it says so");
    CHECK(slots_next_boot_returns(card, why, sizeof(why)), "a card boot that boots the card next: taken");
    CHECK(!slots_next_boot_returns(card_leaves, why, sizeof(why)),
          "a card boot with an eMMC slot set next: refused, the request would wait on the card");
    CHECK(!slots_next_boot_returns(no_next, why, sizeof(why)), "no next slot named: refused");
    why[0] = '\0';
    CHECK(!slots_next_boot_returns("", why, sizeof(why)) && strstr(why, "cannot be read"),
          "an empty inventory (ffboot missing or failed): refused");
    CHECK(!slots_next_boot_returns("slot.a.booted=no\nslot.a.next=yes\n", why, sizeof(why)),
          "only a booted=yes line names the booted slot");

    printf(failures ? "slotcalc_test: %d FAILED\n" : "slotcalc_test: all ok\n", failures);
    return failures ? 1 : 0;
}
