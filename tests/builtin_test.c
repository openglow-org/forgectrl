/*
 * builtin_test.c - host test for the table of built-in extensions
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The table over a real settings file. Cases: the cloud entry is whole,
 * and its refusals are the words the settings route has always said; a
 * request that selects a cloud provider is refused while the cloud is
 * off, by the stored enable or by the request's own, and a core provider
 * never is; turning the cloud off takes down exactly what pointed at it
 * and nothing the request sets itself, and nothing else turns it off; the
 * list says what is on and which provider is selected, never a provider
 * of an extension that is off.
 */
#include "../src/builtin.h"
#include "../src/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* A request: key, value pairs. */
typedef struct { const char *kv[8]; } request_t;
static const char *param(void *ctx, const char *key)
{
    const request_t *r = ctx;
    for (int i = 0; i + 1 < 8 && r->kv[i]; i += 2)
        if (!strcmp(r->kv[i], key))
            return r->kv[i + 1];
    return NULL;
}

static int refused(request_t r, const char **why)
{
    *why = "";
    return builtin_request_refused(param, &r, why);
}

static const char *sweep(request_t r)
{
    static char text[160];
    const char *keys[4], *vals[4];
    size_t n = builtin_request_sweep(param, &r, keys, vals, 4), off = 0;
    text[0] = '\0';
    for (size_t i = 0; i < n; i++)
        off += (size_t)snprintf(text + off, sizeof(text) - off, "%s%s=%s", i ? " " : "", keys[i], vals[i]);
    return text;
}

static const char *json(void)
{
    static char body[2048];
    if (builtin_json(body, sizeof(body)) != 0)
        snprintf(body, sizeof(body), "(does not fit)");
    return body;
}

int main(void)
{
    char conf[128];
    const char *why;
    snprintf(conf, sizeof(conf), "/tmp/builtin_test.%d.conf", (int)getpid());
    setenv("FORGECTRL_CONF", conf, 1);
    unlink(conf);

    /* The table. */
    const builtin_ext_t *cloud = builtin_find("cloud");
    CHECK(builtin_count() >= 1 && cloud && builtin_at(0) == cloud && !builtin_find("nope") && !builtin_find(NULL) &&
          !builtin_at(builtin_count()), "the table's lookups");
    CHECK(cloud && !strcmp(cloud->enable_key, "cloud_enabled") && cloud->nroles == 2 && cloud->settings[0],
          "the cloud entry");
    for (size_t i = 0; i < builtin_count(); i++) {
        const builtin_ext_t *e = builtin_at(i);
        CHECK(e->id[0] && e->name[0] && e->summary[0] && e->enable_key[0] && e->consent[0] && e->setup_step[0],
              "entry %zu has an empty field", i);
        for (size_t r = 0; r < e->nroles; r++) {
            const builtin_role_t *ro = &e->roles[r];
            CHECK(ro->role[0] && ro->provider[0] && ro->kind[0] && ro->select_key[0] && ro->fallback[0] &&
                  ro->refusal[0] && strcmp(ro->provider, ro->fallback), "%s role %zu", e->id, r);
        }
    }

    /* Off, with nothing stored. */
    CHECK(!builtin_enabled(cloud), "the cloud reads on with no settings file");
    CHECK(refused((request_t){{ "homing_mode", "gfcloud" }}, &why) &&
          !strcmp(why, "cloud homing needs cloud mode enabled"), "gfcloud homing while off: \"%s\"", why);
    CHECK(refused((request_t){{ "controller_mode", "cloud" }}, &why) &&
          !strcmp(why, "cloud mode is not enabled on this machine"), "the cloud controller while off: \"%s\"", why);
    CHECK(!refused((request_t){{ "homing_mode", "manual" }}, &why) &&
          !refused((request_t){{ "homing_mode", "none" }}, &why) &&
          !refused((request_t){{ "controller_mode", "grbl" }}, &why) &&
          !refused((request_t){{ "ui_units", "metric" }}, &why), "a core provider was refused: \"%s\"", why);
    /* The request's own enable counts: on with it, the selection stands
     * (the typed phrase is the route's to ask for). */
    CHECK(!refused((request_t){{ "cloud_enabled", "1", "homing_mode", "gfcloud" }}, &why), "on in the same request");
    CHECK(strstr(json(), "\"enabled\":false") && !strstr(json(), "\"active\":true"), "the list while off: %s", json());

    /* On, with both roles pointing at it. */
    settings_set("cloud_enabled", "1");
    settings_set("homing_mode", "gfcloud");
    settings_set("controller_mode", "cloud");
    CHECK(builtin_enabled(cloud), "the cloud reads off with cloud_enabled=1");
    CHECK(!refused((request_t){{ "homing_mode", "gfcloud" }}, &why), "gfcloud homing while on: \"%s\"", why);
    CHECK(refused((request_t){{ "cloud_enabled", "0", "controller_mode", "cloud" }}, &why),
          "off in the same request as a cloud selection");
    /* An empty value clears the key; the stored enable is what counts for the check, as it always was. */
    CHECK(!refused((request_t){{ "cloud_enabled", "", "homing_mode", "gfcloud" }}, &why), "an empty enable: \"%s\"", why);
    CHECK(strstr(json(), "\"enabled\":true") &&
          strstr(json(), "\"provider\":\"gfcloud\",\"kind\":\"runner-fd\",\"select_key\":\"homing_mode\","
                         "\"fallback\":\"none\",\"active\":true") &&
          strstr(json(), "\"provider\":\"cloud\",\"kind\":\"supervised\",\"select_key\":\"controller_mode\","
                         "\"fallback\":\"grbl\",\"active\":true"), "the list while on: %s", json());

    /* The sweep. */
    CHECK(!strcmp(sweep((request_t){{ "cloud_enabled", "0" }}), "homing_mode=none controller_mode=grbl"),
          "off sweeps \"%s\"", sweep((request_t){{ "cloud_enabled", "0" }}));
    CHECK(!strcmp(sweep((request_t){{ "cloud_enabled", "0", "homing_mode", "manual" }}), "controller_mode=grbl"),
          "a key the request sets was swept: \"%s\"", sweep((request_t){{ "cloud_enabled", "0", "homing_mode", "manual" }}));
    CHECK(!sweep((request_t){{ "cloud_enabled", "1" }})[0] && !sweep((request_t){{ "cloud_enabled", "" }})[0] &&
          !sweep((request_t){{ "ui_units", "metric" }})[0], "something other than off swept");
    settings_set("homing_mode", "manual");
    CHECK(!strcmp(sweep((request_t){{ "cloud_enabled", "0" }}), "controller_mode=grbl"),
          "manual homing was swept: \"%s\"", sweep((request_t){{ "cloud_enabled", "0" }}));
    CHECK(strstr(json(), "\"select_key\":\"homing_mode\",\"fallback\":\"none\",\"active\":false"),
          "gfcloud reads active under manual homing: %s", json());

    /* Off again with a stale selection on file: nothing of it reads active. */
    settings_set("cloud_enabled", "0");
    CHECK(!strstr(json(), "\"active\":true"), "a provider of an extension that is off reads active: %s", json());
    char small[64];
    CHECK(builtin_json(small, sizeof(small)) == -1, "a list cut short was served");

    unlink(conf);
    printf(fails ? "builtin_test: %d FAILED\n" : "builtin_test: all passed\n", fails);
    return fails ? 1 : 0;
}
