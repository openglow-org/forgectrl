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
 *
 * And settings_get's answers, which every reader of the file shares: a
 * value whole or refused (too long for the buffer or the reader), an
 * absent key, and a file that cannot be read, told apart.
 *
 * And the data directory's override: one with a shell character is not
 * taken (it goes into shell lines), a plain path is.
 */
#include "../src/settings.h"
#include "../src/paths.h"

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

    /* The data directory override goes into shell lines: one with a shell
     * character is not taken; a plain path is. */
    setenv("FORGECTRL_DATA_DIR", "/tmp/a;b", 1);
    CHECK(!strcmp(ff_data_dir(), "/data/forgefirm"), "a data directory with a ';' was taken: %s", ff_data_dir());
    setenv("FORGECTRL_DATA_DIR", dir, 1);
    CHECK(!strcmp(ff_data_dir(), dir), "a plain data directory was not taken: %s", ff_data_dir());
    unsetenv("FORGECTRL_DATA_DIR");

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

    /* The read: a value whole or not at all. A value longer than the
     * caller's buffer, or on a line longer than the reader's, is refused
     * and leaves the buffer as it was, never a prefix of it; a file that
     * cannot be read is told apart from an absent key. */
    {
        char v[8] = "keep", big[700];
        FILE *f = fopen(conf, "w");
        memset(big, '0', sizeof(big));
        big[sizeof(big) - 1] = '\0';
        if (f) {
            fprintf(f, "short = 12\nlong = 123456789\nlines = %s1\nafter = 7\n", big);
            fclose(f);
        }
        CHECK(settings_get("short", v, sizeof(v)) == 0 && !strcmp(v, "12"), "a value that fits: read");
        strcpy(v, "keep");
        CHECK(settings_get("long", v, sizeof(v)) == SETTINGS_TOO_LONG && !strcmp(v, "keep"),
              "a value longer than the buffer: refused, the buffer untouched (\"%s\")", v);
        char wide[1024] = "keep";
        CHECK(settings_get("lines", wide, sizeof(wide)) == SETTINGS_TOO_LONG && !strcmp(wide, "keep"),
              "a value on a line longer than the reader's: refused");
        CHECK(settings_get("after", v, sizeof(v)) == 0 && !strcmp(v, "7"),
              "the line after a long one reads as its own");
        CHECK(settings_get("absent", v, sizeof(v)) == SETTINGS_ABSENT, "an absent key: absent");
        unlink(conf);
        CHECK(settings_get("short", v, sizeof(v)) == SETTINGS_ABSENT, "no file: absent");
        mkdir(conf, 0755);              /* a path that opens and does not read */
        CHECK(settings_get("short", v, sizeof(v)) == SETTINGS_UNREADABLE, "a file that cannot be read: unreadable");
        rmdir(conf);
        /* A path that cannot be opened for another reason than its absence
         * (here ENOTDIR, which root meets too): unreadable. */
        char under[320];
        f = fopen(conf, "w");
        if (f)
            fclose(f);
        snprintf(under, sizeof(under), "%s/conf", conf);
        setenv("FORGECTRL_CONF", under, 1);
        CHECK(settings_get("short", v, sizeof(v)) == SETTINGS_UNREADABLE, "a file that cannot be opened: unreadable");
        setenv("FORGECTRL_CONF", conf, 1);
        unlink(conf);
    }

    /* No directory: said, not made. */
    rmdir(dir);
    CHECK(settings_dir_searchable() == -1, "a missing directory was reported searchable");
    CHECK(mode_of(dir) == 0xffffu, "the call made the directory");

    printf(fails ? "settings_dir_test: %d FAILED\n" : "settings_dir_test: all passed\n", fails);
    return fails ? 1 : 0;
}
