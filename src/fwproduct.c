/*
 * fwproduct.c - the product gate on the firmware paths
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "fwproduct.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

int fwproduct_parse(const char *fwup_m, char *out, size_t len)
{
    static const char key[] = "meta-product=";
    if (len)
        out[0] = '\0';
    for (const char *line = fwup_m; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (strncmp(line, key, sizeof(key) - 1) != 0)
            continue;
        const char *v = line + sizeof(key) - 1;
        size_t n = strcspn(v, "\r\n");
        if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
            v++;
            n -= 2;
        }
        if (n >= len)
            return -1;
        memcpy(out, v, n);
        out[n] = '\0';
        return 0;
    }
    return -1;
}

const char *fwproduct_gate(const char *file, int cls)
{
    char cmd[256], raw[2048], product[64];
    raw[0] = '\0';
    if (snprintf(cmd, sizeof(cmd), "fwup -m -i %s 2>/dev/null", file) < (int)sizeof(cmd)) {
        FILE *p = popen(cmd, "r");
        if (p) {
            size_t n = fread(raw, 1, sizeof(raw) - 1, p);
            raw[n] = '\0';
            while (fgetc(p) != EOF)
                ;                               /* drain so the child can exit */
            int st = pclose(p);
            if (st < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
                raw[0] = '\0';
        }
    }
    fwproduct_parse(raw, product, sizeof(product));
    return fwproduct_refusal(product, cls);
}

const char *fwproduct_refusal(const char *product, int cls)
{
    if (product && strcmp(product, FWPRODUCT_EXTENSION) == 0)
        return "this archive is an extension package, not firmware";
    int forgefirm = product && strcmp(product, FWPRODUCT_FORGEFIRM) == 0;
    int factory = product && strcmp(product, FWPRODUCT_FACTORY) == 0;
    if (cls == FWCLASS_RELEASE)
        return forgefirm ? NULL : "this archive is signed with the ForgeFIRM release key and is not ForgeFIRM firmware";
    if (cls == FWCLASS_FACTORY)
        return factory ? NULL : "this archive is signed with a factory key and is not factory firmware";
    if (cls == FWCLASS_NONE)
        return forgefirm || factory ? NULL : "this archive is not firmware";
    return "this archive is not firmware";
}
