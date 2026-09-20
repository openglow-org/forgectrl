/*
 * builtin.c - the built-in extensions: what the image itself contributes
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * See builtin.h. The table is static and written so that the panel's dev
 * mock can read it out of this file: one entry, one roles array, plain
 * string literals.
 */
#include "builtin.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>

static const builtin_role_t cloud_roles[] = {
    { "homing", "gfcloud", "runner-fd", "homing_mode", "none",
      "cloud homing needs cloud mode enabled" },
    { "controller", "cloud", "supervised", "controller_mode", "grbl",
      "cloud mode is not enabled on this machine" },
};

static const char *const cloud_settings[] = {
    "cloud_*", "gfcloud_*", "gf_serial", "gf_password", "log_gfcloud_*", NULL,
};

static const builtin_ext_t builtin_ext[] = {
    { "cloud", "Glowforge cloud mode",
      "Runs the factory experience: jobs from the Glowforge web service, and its camera homing.",
      "cloud_enabled",
      "The cloud step of the setup, with its typed acknowledgment.",
      "cloud", "gfcloud", cloud_settings,
      cloud_roles, sizeof(cloud_roles) / sizeof(cloud_roles[0]) },
};

size_t builtin_count(void)
{
    return sizeof(builtin_ext) / sizeof(builtin_ext[0]);
}

const builtin_ext_t *builtin_at(size_t i)
{
    return i < builtin_count() ? &builtin_ext[i] : NULL;
}

const builtin_ext_t *builtin_find(const char *id)
{
    for (size_t i = 0; id && i < builtin_count(); i++)
        if (!strcmp(builtin_ext[i].id, id))
            return &builtin_ext[i];
    return NULL;
}

int builtin_enabled(const builtin_ext_t *e)
{
    return e && settings_get_bool(e->enable_key, 0);
}

/* The enable as the request sees it: its own value when it gives the key
 * one, else the stored one. */
static int enabled_after(const builtin_ext_t *e, builtin_param_fn param, void *ctx)
{
    const char *v = param(ctx, e->enable_key);
    return v && v[0] ? !strcmp(v, "1") : builtin_enabled(e);
}

int builtin_request_refused(builtin_param_fn param, void *ctx, const char **why)
{
    for (size_t i = 0; i < builtin_count(); i++) {
        const builtin_ext_t *e = &builtin_ext[i];
        if (enabled_after(e, param, ctx))
            continue;
        for (size_t r = 0; r < e->nroles; r++) {
            const char *sel = param(ctx, e->roles[r].select_key);
            if (sel && !strcmp(sel, e->roles[r].provider)) {
                *why = e->roles[r].refusal;
                return 1;
            }
        }
    }
    return 0;
}

size_t builtin_request_sweep(builtin_param_fn param, void *ctx, const char **keys,
                             const char **vals, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < builtin_count(); i++) {
        const builtin_ext_t *e = &builtin_ext[i];
        const char *v = param(ctx, e->enable_key);
        if (!v || strcmp(v, "0"))
            continue;                   /* the request does not turn it off */
        for (size_t r = 0; r < e->nroles && n < max; r++) {
            char cur[32];
            if (param(ctx, e->roles[r].select_key))
                continue;               /* set by the request itself, and checked as sent */
            if (settings_get(e->roles[r].select_key, cur, sizeof(cur)) != 0 ||
                strcmp(cur, e->roles[r].provider))
                continue;
            keys[n] = e->roles[r].select_key;
            vals[n] = e->roles[r].fallback;
            n++;
        }
    }
    return n;
}

int builtin_json(char *buf, size_t len)
{
    size_t off = 0;
#define PUT(...) do { if (off < len) off += (size_t)snprintf(buf + off, len - off, __VA_ARGS__); } while (0)
    PUT("{\"extensions\":[");
    for (size_t i = 0; i < builtin_count(); i++) {
        const builtin_ext_t *e = &builtin_ext[i];
        int on = builtin_enabled(e);
        PUT("%s{\"id\":\"%s\",\"name\":\"%s\",\"summary\":\"%s\",\"builtin\":true,\"enabled\":%s,"
            "\"enable_key\":\"%s\",\"consent\":\"%s\",\"setup_step\":\"%s\",",
            i ? "," : "", e->id, e->name, e->summary, on ? "true" : "false", e->enable_key, e->consent,
            e->setup_step);
        if (e->tab)
            PUT("\"tab\":\"%s\",", e->tab);
        PUT("\"settings\":[");
        for (size_t k = 0; e->settings[k]; k++)
            PUT("%s\"%s\"", k ? "," : "", e->settings[k]);
        PUT("],\"roles\":[");
        for (size_t r = 0; r < e->nroles; r++) {
            char cur[32] = "";
            int active = on && settings_get(e->roles[r].select_key, cur, sizeof(cur)) == 0 &&
                         !strcmp(cur, e->roles[r].provider);
            PUT("%s{\"role\":\"%s\",\"provider\":\"%s\",\"kind\":\"%s\",\"select_key\":\"%s\","
                "\"fallback\":\"%s\",\"active\":%s}",
                r ? "," : "", e->roles[r].role, e->roles[r].provider, e->roles[r].kind,
                e->roles[r].select_key, e->roles[r].fallback, active ? "true" : "false");
        }
        PUT("]}");
    }
    PUT("]}");
#undef PUT
    return off < len ? 0 : -1;
}
