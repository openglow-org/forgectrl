/*
 * settings_dir_test.c - host test: the data directory opened for search
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * An extension package's account walks through the data directory to its
 * own files. settings_dir_searchable() adds the search bit for group and
 * others to a directory an installer made closed, takes nothing away,
 * makes nothing listable, and leaves an open directory as it is.
 */
#include "../src/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static unsigned mode_of(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (unsigned)(st.st_mode & 07777) : 0xffffu;
}

int main(void)
{
    char dir[] = "/tmp/settings-dir-XXXXXX", conf[300];
    if (!mkdtemp(dir)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    snprintf(conf, sizeof(conf), "%s/forgefirm.conf", dir);
    setenv("FORGECTRL_CONF", conf, 1);

    /* As an installer under a strict umask leaves it. */
    chmod(dir, 0700);
    CHECK(settings_dir_searchable() == 0, "a closed directory could not be opened for search");
    CHECK(mode_of(dir) == 0711, "0700 became %04o, expected 0711", mode_of(dir));
    CHECK(settings_dir_searchable() == 0 && mode_of(dir) == 0711, "a second call changed it: %04o", mode_of(dir));

    /* An open one is left alone, and nothing is ever taken away. */
    chmod(dir, 0755);
    CHECK(settings_dir_searchable() == 0 && mode_of(dir) == 0755, "0755 became %04o", mode_of(dir));
    chmod(dir, 0750);
    CHECK(settings_dir_searchable() == 0 && mode_of(dir) == 0751, "0750 became %04o, expected 0751", mode_of(dir));

    /* No directory: said, not made. */
    rmdir(dir);
    CHECK(settings_dir_searchable() == -1, "a missing directory was reported searchable");
    CHECK(mode_of(dir) == 0xffffu, "the call made the directory");

    printf(fails ? "settings_dir_test: %d FAILED\n" : "settings_dir_test: all passed\n", fails);
    return fails ? 1 : 0;
}
