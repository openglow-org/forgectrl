/*
 * status_sys_test.c - host unit test for the machine status JSON sys block
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The /status document carries CPU and memory utilization next to the
 * board temperatures. The CPU number is a delta over the interval since
 * the previous status read, so the first read must report null and a
 * later read a percent in range; memory is absolute and must report at
 * once. The whole document must still fit the handler's buffer and stay
 * structurally intact (no truncation mid-key).
 *
 * The test drives the real machine_status_json() from status.c with the
 * sysfs reader pointed at an empty temp tree (GF_SYSFS_ROOT), so every
 * machine attribute takes its fallback path. The sampler reads a stat
 * file of the test's own (GF_PROC_STAT); /proc/meminfo is the host's.
 */
#define _GNU_SOURCE
#include "../src/status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* status.c's only external dependency. */
int diag_running(void) { return 0; }
/* No settings store on the host: the lens block reads its defaults. */
int settings_get(const char *key, char *val, size_t len) { (void)key; (void)val; (void)len; return -1; }
int super_grbl_running(void) { return 0; }

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* The numeric value following "key": in the document, or -1000 when the
 * key is absent or its value is null. */
static double json_num(const char *doc, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(doc, pat);
    if (!p)
        return -1000;
    p += strlen(pat);
    if (strncmp(p, "null", 4) == 0)
        return -1000;
    return strtod(p, NULL);
}

static int json_is_null(const char *doc, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(doc, pat);
    return p && strncmp(p + strlen(pat), "null", 4) == 0;
}

/* One stat file for the sampler, rewritten in place. */
static void write_stat(const char *path, const char *line)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror("stat file"); exit(2); }
    fputs(line, f);
    fclose(f);
}

/* Structural sanity: braces balance and the document closes itself. */
static int json_intact(const char *doc)
{
    int depth = 0;
    for (const char *p = doc; *p; p++) {
        if (*p == '{') depth++;
        if (*p == '}') depth--;
    }
    size_t n = strlen(doc);
    return depth == 0 && n > 2 && doc[0] == '{' && doc[n - 1] == '}';
}

int main(void)
{
    char tmpl[] = "/tmp/forgectrl-status-sys-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 2; }
    char root[280];
    snprintf(root, sizeof(root), "%s/", dir);
    setenv("GF_SYSFS_ROOT", root, 1);
    setenv("GF_SWITCH_DEV", "/nonexistent/forgectrl-switch", 1);

    printf("machine status JSON sys block:\n");
    /* Same buffer size as the /status handler in main.c. */
    char doc[3072];               /* the /status handler's body size */

    /* The sampler over a stat file the test writes
     * (cpu user nice system idle iowait irq softirq steal). */
    char statf[300];
    snprintf(statf, sizeof(statf), "%s/stat", dir);
    setenv("GF_PROC_STAT", statf, 1);
    write_stat(statf, "cpu 10 0 10 70 10 0 0 0\n");           /* total 100, idle 80 */
    CHECK(machine_status_json(doc, sizeof(doc), "\"gates_off\":[]") == 0,
          "first status document renders");
    CHECK(json_intact(doc), "first document is structurally intact");
    CHECK(strstr(doc, "\"sys\":{\"cpu_pct\":") != NULL, "sys block present");
    CHECK(json_is_null(doc, "cpu_pct"), "first read reports cpu_pct null (unprimed)");
    double mem = json_num(doc, "mem_pct");
    CHECK(mem > 0 && mem < 100, "mem_pct is a percent on the first read");
    write_stat(statf, "cpu 15 0 15 160 10 0 0 0\n");          /* total +100, idle +90 */
    CHECK(machine_status_json(doc, sizeof(doc), "\"gates_off\":[]") == 0, "a later tick renders");
    CHECK(json_intact(doc), "the later document is structurally intact");
    CHECK(json_num(doc, "cpu_pct") == 10.0, "the interval's busy share is the percent (10.0)");
    unlink(statf);

    /* The document still ends with the switch map: nothing after the
     * sys block was pushed out of the buffer. */
    CHECK(strstr(doc, "\"hv_enable\":") != NULL, "switch map survives (no truncation)");

    rmdir(dir);
    printf(failures ? "FAIL: %d check(s) failed\n"
                    : "PASS: sys block reports CPU and memory utilization\n",
           failures);
    return failures ? 1 : 0;
}
