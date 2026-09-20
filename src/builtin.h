/*
 * builtin.h - the built-in extensions: what the image itself contributes
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * An extension is something the operator turns on: it has an enable, a
 * consent, a place in the panel, settings of its own, and roles it
 * provides (a way to home, a controller). Cloud mode has been all of that
 * since it was written, with each part in a different file. This table
 * says it in one place, so the panel can list it and the settings route
 * and the cloud step can ask it, and so that a package, when there are
 * packages, meets a model the image already lives by.
 *
 * A built-in is image content: compiled in, under release acceptance,
 * and its panel tab is panel code. The table moves no behavior. The
 * enable is still the settings key the cloud step writes, the consent is
 * still that step's typed phrase, and turning it off still takes down
 * whatever pointed at it.
 *
 * The provider kinds are compile-time words with no spelling a package
 * could use: runner-fd (the driver forks a runner that inherits the pulse
 * device) and supervised (the supervisor's own controller). A provider
 * that belongs to no entry here is core and always exists: homing none
 * and manual, the grbl controller.
 */
#ifndef FORGECTRL_BUILTIN_H
#define FORGECTRL_BUILTIN_H

#include <stddef.h>

typedef struct {
    const char *role;           /* "homing", "controller" */
    const char *provider;       /* the role's value that this extension provides */
    const char *kind;           /* "runner-fd", "supervised" */
    const char *select_key;     /* the settings key that selects the role's provider */
    const char *fallback;       /* what that key falls back to when the extension goes off */
    const char *refusal;        /* selecting the provider while the extension is off, in words */
} builtin_role_t;

typedef struct {
    const char *id;
    const char *name;
    const char *summary;
    const char *enable_key;     /* the settings key that is the enable */
    const char *consent;        /* how it is turned on, in words */
    const char *setup_step;     /* the setup step that turns it on and off */
    const char *tab;            /* the panel tab it owns (panel code), or NULL */
    const char *const *settings;/* its settings: a key, or a prefix ending in '*'; NULL-terminated */
    const builtin_role_t *roles;
    size_t nroles;
} builtin_ext_t;

size_t builtin_count(void);
const builtin_ext_t *builtin_at(size_t i);
const builtin_ext_t *builtin_find(const char *id);

/* Whether the extension is on: its enable key reads 1. */
int builtin_enabled(const builtin_ext_t *e);

/* A settings request, as the table sees it: the value the request gives a
 * key, or NULL when it does not name the key. */
typedef const char *(*builtin_param_fn)(void *ctx, const char *key);

/* 1 with the refusal when the request selects a provider whose extension
 * is off (the request's own value of the enable counts, else the stored
 * one); 0 when nothing in it points at an extension that is off. */
int builtin_request_refused(builtin_param_fn param, void *ctx, const char **why);

/* What a request that turns an extension off takes down with it: for
 * each role of that extension whose provider is the one selected now, and
 * which the request does not set itself, the select key and its fallback.
 * Fills keys and vals up to max and returns how many. */
size_t builtin_request_sweep(builtin_param_fn param, void *ctx, const char **keys,
                             const char **vals, size_t max);

/* {"extensions":[{...}]} for GET /extensions: each entry with whether it
 * is enabled and, per role, whether its provider is the one selected. 0,
 * or -1 when it does not fit. */
int builtin_json(char *buf, size_t len);

#endif
