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
 * The document has concurrent readers (the panel and the acceptance
 * tool), so two reads can land inside one scheduler tick, where the
 * /proc/stat counters stand still: the CPU number must then repeat the
 * last percent, not drop to null. The idle field carries iowait, which
 * the kernel may lower between reads: a step backward repeats the last
 * percent too, and an idle step past the interval reads as no busy time.
 *
 * The test drives the real machine_status_json() from status.c with the
 * sysfs reader pointed at an empty temp tree (GF_SYSFS_ROOT), so every
 * machine attribute takes its fallback path. The tick cases feed the
 * sampler a stat file of the test's own (GF_PROC_STAT), with counters
 * far below any host's so the host section after it always reads a
 * forward step; /proc/meminfo is the host's own throughout.
 */
#define _GNU_SOURCE
#include "../src/status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

    /* The tick cases: the sampler over a stat file the test writes
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
    CHECK(machine_status_json(doc, sizeof(doc), "\"gates_off\":[]") == 0,
          "a second read in the priming tick renders");
    CHECK(json_is_null(doc, "cpu_pct"), "a read in the priming tick still has no percent");
    write_stat(statf, "cpu 15 0 15 160 10 0 0 0\n");          /* total +100, idle +90 */
    CHECK(machine_status_json(doc, sizeof(doc), "\"gates_off\":[]") == 0, "a later tick renders");
    CHECK(json_num(doc, "cpu_pct") == 10.0, "the interval's busy share is the percent (10.0)");
    CHECK(machine_status_json(doc, sizeof(doc), "\"gates_off\":[]") == 0, "a same-tick read renders");
    CHECK(json_num(doc, "cpu_pct") == 10.0,
          "a second read inside the same tick repeats the percent instead of null");
    write_stat(statf, "cpu 30 0 30 160 5 0 0 0\n");           /* total +25, idle+iowait 165 (backward) */
    CHECK(machine_status_json(doc, sizeof(doc), "\"gates_off\":[]") == 0, "an iowait step backward renders");
    CHECK(json_num(doc, "cpu_pct") == 10.0,
          "idle+iowait stepping backward repeats the percent instead of null");
    write_stat(statf, "cpu 30 0 30 280 5 0 0 0\n");           /* total +120, idle +120 */
    CHECK(machine_status_json(doc, sizeof(doc), "\"gates_off\":[]") == 0, "an all-idle interval renders");
    CHECK(json_num(doc, "cpu_pct") == 0.0, "an all-idle interval is 0.0");
    write_stat(statf, "cpu 0 0 0 500 5 0 0 0\n");             /* total +160, idle +220 */
    CHECK(machine_status_json(doc, sizeof(doc), "\"gates_off\":[]") == 0, "an idle step past the interval renders");
    CHECK(json_num(doc, "cpu_pct") == 0.0, "an idle step past the interval reads as no busy time");
    unlink(statf);
    unsetenv("GF_PROC_STAT");

    /* The host's own /proc/stat from here: its counters stand far above
     * the file's, so the first host read is a forward step. */
    CHECK(machine_status_json(doc, sizeof(doc), "\"gates_off\":[]") == 0,
          "first host status document renders");
    CHECK(json_num(doc, "cpu_pct") >= 0 && json_num(doc, "cpu_pct") <= 100,
          "the first host read is a percent");

    /* Let the CPU counters advance: sleep across a few ticks, then burn
     * a little user time so the busy share is nonzero. */
    struct timespec ts = { 0, 100 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    volatile unsigned long spin = 0;
    clock_t t0 = clock();
    while (clock() - t0 < CLOCKS_PER_SEC / 20)
        spin++;

    CHECK(machine_status_json(doc, sizeof(doc), "\"gates_off\":[]") == 0,
          "second status document renders");
    CHECK(json_intact(doc), "second document is structurally intact");
    double cpu = json_num(doc, "cpu_pct");
    CHECK(cpu >= 0 && cpu <= 100, "primed cpu_pct is a percent");
    mem = json_num(doc, "mem_pct");
    CHECK(mem > 0 && mem < 100, "mem_pct is a percent on the second read");
    printf("  (cpu %.1f%%, mem %.1f%%)\n", cpu, mem);

    /* The document still ends with the switch map: nothing after the
     * sys block was pushed out of the buffer. */
    CHECK(strstr(doc, "\"hv_enable\":") != NULL, "switch map survives (no truncation)");

    rmdir(dir);
    printf(failures ? "FAIL: %d check(s) failed\n"
                    : "PASS: sys block reports CPU and memory utilization\n",
           failures);
    return failures ? 1 : 0;
}
