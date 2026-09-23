/*
 * extpkg_test.c - host test: the package routes' relay to the extension host
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * A stand-in for the host's command line records the arguments it was run
 * with and answers as the host does. What is checked: a package id has the
 * form of one before it is an argument, the actions are a closed list and
 * become exactly the host's commands, nothing of a request is ever a shell
 * word, the host's refusal is passed on in its words, a host that cannot
 * be run or says something that is no JSON is 502, the status document
 * carries the host's own status only while that host is alive, a page's
 * call to its service is held to its form before the host runs, and the
 * catalog: the index fetched from its one address with curl held to https
 * and a bound, and verified by the host; a listed package fetched from
 * where the kept index says, held to its size and SHA-256 before the host
 * reads it, and staged for the upload's install.
 */
#include "../src/extpkg.h"
#include "../src/sha256.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static char dir[] = "/tmp/extpkg-test-XXXXXX";
static char stand_in[300], args_file[300], status_file[300];

static void write_file(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
        chmod(path, mode);
    }
}

/* The arguments the stand-in was last run with, one per line. */
static const char *ran(void)
{
    static char text[1024];
    FILE *f = fopen(args_file, "r");
    size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
    if (f)
        fclose(f);
    text[n] = '\0';
    return text;
}

static void host_says(const char *script_body)
{
    char script[8192];
    snprintf(script, sizeof(script), "#!/bin/sh\nfor a in \"$@\"; do echo \"$a\"; done > %s\n%s\n", args_file, script_body);
    write_file(stand_in, script, 0755);
    unlink(args_file);
}

int main(void)
{
    int status;
    char why[300];
    if (!mkdtemp(dir)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    snprintf(stand_in, sizeof(stand_in), "%s/forgeext", dir);
    snprintf(args_file, sizeof(args_file), "%s/args", dir);
    snprintf(status_file, sizeof(status_file), "%s/status.json", dir);
    setenv("FORGECTRL_FORGEEXT", stand_in, 1);
    setenv("FORGECTRL_EXT_STATUS", status_file, 1);

    /* The form of a package id. */
    static const char *const good[] = { "org.example.notify", "io.a-b.c9", "a.b" };
    static const char *const bad[] = { "", "notify", ".org.example", "org.example.", "org..example", "Org.example", "org.example/x",
                                       "org.example;reboot", "org.example x", "-rf.x", "org.example.$(id)",
                                       "a.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++)
        CHECK(extpkg_id_ok(good[i]), "%s is refused as an id", good[i]);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!extpkg_id_ok(bad[i]), "\"%s\" is taken as an id", bad[i]);
    CHECK(!extpkg_id_ok(NULL), "no id at all is taken as one");

    /* Each action is exactly the host's command. */
    static const struct { const char *action, *ran; } acts[] = {
        { "enable", "enable\norg.example.notify\n" }, { "disable", "disable\norg.example.notify\n" },
        { "remove", "remove\norg.example.notify\n" }, { "remove-keep-data", "remove\norg.example.notify\n--keep-data\n" },
        { "hold-required", "hold\norg.example.notify\nrequired\n" }, { "hold-advisory", "hold\norg.example.notify\nadvisory\n" },
    };
    for (size_t i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) {
        host_says("echo '{\"ok\": true}'");
        int rc = extpkg_action("org.example.notify", acts[i].action, &status, why, sizeof(why));
        CHECK(rc == 0 && status == 200 && !strcmp(ran(), acts[i].ran), "%s: rc %d, status %d, ran [%s]", acts[i].action, rc, status, ran());
    }

    /* What has no such form never runs the host. */
    host_says("echo '{\"ok\": true}'");
    static const char *const no_action[] = { "install", "run", "", "enable;reboot", "remove --keep-data", "ENABLE" };
    for (size_t i = 0; i < sizeof(no_action) / sizeof(no_action[0]); i++) {
        int rc = extpkg_action("org.example.notify", no_action[i], &status, why, sizeof(why));
        CHECK(rc != 0 && status == 400 && why[0] && !ran()[0], "action \"%s\": rc %d, status %d, ran [%s]", no_action[i], rc, status, ran());
    }
    CHECK(extpkg_action("org.example.notify", NULL, &status, why, sizeof(why)) != 0 && status == 400 && !ran()[0], "no action at all");
    CHECK(extpkg_action("org.example;reboot", "enable", &status, why, sizeof(why)) != 0 && status == 400 && !ran()[0],
          "an id that is none ran the host: [%s]", ran());

    /* The host's refusal, in its words; and a host that cannot be asked. */
    host_says("echo '{\"ok\": false, \"error\": \"org.example.notify has no hold: the operator did not grant it one\"}'; exit 1");
    CHECK(extpkg_action("org.example.notify", "hold-required", &status, why, sizeof(why)) != 0 && status == 409 &&
          strstr(why, "did not grant"), "the host's refusal: %d %s", status, why);
    host_says("echo 'Segmentation fault'; exit 139");
    CHECK(extpkg_action("org.example.notify", "enable", &status, why, sizeof(why)) != 0 && status == 502, "an answer that is no JSON: %d", status);
    unlink(stand_in);
    CHECK(extpkg_action("org.example.notify", "enable", &status, why, sizeof(why)) != 0 && status == 502, "no host to run: %d", status);

    /* The status document. */
    host_says("echo '{\"ok\": true, \"packages\": [{\"id\": \"org.example.notify\", \"enabled\": true, \"hold\": \"advisory\"}]}'");
    write_file(status_file, "{\"pid\": 1, \"enabled\": true, \"services\": [{\"id\": \"org.example.notify\", \"state\": \"running\"}]}", 0644);
    char *doc = extpkg_status_json(1);
    json_t *j = doc ? json_loads(doc, 0, NULL) : NULL;
    CHECK(j && json_is_true(json_object_get(j, "enabled")) && json_is_false(json_object_get(j, "safe_mode")) &&
          json_array_size(json_object_get(j, "packages")) == 1 && !strcmp(ran(), "keys\n"),
          "the status document (list first, then the keys): %s", doc ? doc : "none");
    CHECK(j && json_is_false(json_object_get(json_object_get(j, "host"), "running")) &&
          !json_object_get(json_object_get(j, "host"), "services"),
          "a status file whose pid is not a running host is a dead host's: its word is not passed on: %s", doc ? doc : "none");
    json_decref(j);
    free(doc);
    unlink(stand_in);
    doc = extpkg_status_json(0);
    j = doc ? json_loads(doc, 0, NULL) : NULL;
    CHECK(j && json_is_false(json_object_get(j, "enabled")) && json_array_size(json_object_get(j, "packages")) == 0 &&
          json_string_value(json_object_get(j, "error")), "no host to list the packages: said so: %s", doc ? doc : "none");
    json_decref(j);
    free(doc);

    /* Installing: the tier is the host's to say, and the consent is forgectrl's to take. */
    char stage[300];
    snprintf(stage, sizeof(stage), "%s/upload.ffx", dir);
    setenv("FORGECTRL_EXT_STAGE", stage, 1);
    host_says("echo '{\"ok\": true}'");
    CHECK(extpkg_install(NULL, NULL, 1, &status, why, sizeof(why)) != 0 && status == 409 && strstr(why, "staged") && !ran()[0],
          "nothing staged: %d %s, ran [%s]", status, why, ran());
    CHECK(extpkg_inspect_json(&status, why, sizeof(why)) == NULL && status == 409, "inspect with nothing staged: %d", status);

    static const struct { const char *tier, *consent; } tiers[] = {
        { "official", "login" }, { "community", "typed" }, { "unverified", "button" }, { "something-new", "button" },
    };
    for (size_t i = 0; i < sizeof(tiers) / sizeof(tiers[0]); i++) {
        char says[300];
        write_file(stage, "an archive", 0600);
        snprintf(says, sizeof(says), "echo '{\"ok\": true, \"tier\": \"%s\", \"needs_grant\": [\"hold\"]}'", tiers[i].tier);
        host_says(says);
        doc = extpkg_inspect_json(&status, why, sizeof(why));
        j = doc ? json_loads(doc, 0, NULL) : NULL;
        char want[340];
        snprintf(want, sizeof(want), "inspect\n%s\n", stage);
        CHECK(j && status == 200 && !strcmp(json_string_value(json_object_get(j, "consent")), tiers[i].consent) &&
              !strcmp(ran(), want), "tier %s asks for %s: %s, ran [%s]", tiers[i].tier, tiers[i].consent, doc ? doc : why, ran());
        json_decref(j);
        free(doc);
    }

    /* An archive the host will not take is not kept. */
    write_file(stage, "an archive", 0600);
    host_says("echo '{\"ok\": false, \"error\": \"this archive is firmware, not an extension package\"}'; exit 1");
    CHECK(extpkg_inspect_json(&status, why, sizeof(why)) == NULL && status == 400 && strstr(why, "firmware") &&
          access(stage, F_OK) != 0, "a refused archive: %d %s, staged file %s", status, why, access(stage, F_OK) ? "gone" : "kept");

    /* The stand-in answers inspect with a tier and records what install was run with. */
    char script[1200];
#define HOST_TIER(tier) do { \
    snprintf(script, sizeof(script), "if [ \"$1\" = inspect ]; then echo '{\"ok\": true, \"tier\": \"" tier "\"}'; exit 0; fi\n" \
             "for a in \"$@\"; do echo \"$a\"; done > %s.install\necho '{\"ok\": true}'", args_file); \
    host_says(script); \
    write_file(stage, "an archive", 0600); \
    snprintf(script, sizeof(script), "%s.install", args_file); \
    unlink(script); } while (0)
#define INSTALL_RAN() ({ static char t[1024]; char pth[340]; snprintf(pth, sizeof(pth), "%s.install", args_file); \
    FILE *f = fopen(pth, "r"); size_t n = f ? fread(t, 1, sizeof(t) - 1, f) : 0; if (f) fclose(f); t[n] = 0; t; })
    char expect[400], want_any[700];

    HOST_TIER("official");
    CHECK(extpkg_install("hold,job_time.run", NULL, 0, &status, why, sizeof(why)) == 0 && status == 200, "official: %d %s", status, why);
    snprintf(expect, sizeof(expect), "install\n%s\n--grant\nhold\n--grant\njob_time.run\n", stage);
    CHECK(!strcmp(INSTALL_RAN(), expect) && access(stage, F_OK) != 0, "official: the login is enough, the grants go on, no consent "
          "flag, the staged file gone: [%s]", INSTALL_RAN());

    HOST_TIER("community");
    CHECK(extpkg_install(NULL, NULL, 1, &status, why, sizeof(why)) != 0 && status == 400 && strstr(why, EXTPKG_PHRASE) &&
          !INSTALL_RAN()[0] && access(stage, F_OK) == 0, "community without the phrase: %d, ran [%s]", status, INSTALL_RAN());
    CHECK(extpkg_install(NULL, "i understand", 1, &status, why, sizeof(why)) != 0 && status == 400 && !INSTALL_RAN()[0],
          "community with the phrase in another case: %d", status);
    CHECK(extpkg_install("", EXTPKG_PHRASE, 0, &status, why, sizeof(why)) == 0, "community with the phrase: %d %s", status, why);
    snprintf(expect, sizeof(expect), "install\n%s\n--consent-community\n", stage);
    CHECK(!strcmp(INSTALL_RAN(), expect), "community: the host is told of the consent forgectrl took: [%s]", INSTALL_RAN());

    HOST_TIER("unverified");
    CHECK(extpkg_install(NULL, EXTPKG_PHRASE, 0, &status, why, sizeof(why)) != 0 && status == 409 && strstr(why, "button") &&
          !INSTALL_RAN()[0] && access(stage, F_OK) == 0, "unverified without the button (the phrase is no substitute): %d, ran [%s]",
          status, INSTALL_RAN());
    CHECK(extpkg_install("hold", NULL, 1, &status, why, sizeof(why)) == 0, "unverified with the button held: %d %s", status, why);
    snprintf(expect, sizeof(expect), "install\n%s\n--grant\nhold\n--consent-unverified\n", stage);
    CHECK(!strcmp(INSTALL_RAN(), expect), "unverified: [%s]", INSTALL_RAN());

    /* Grants that have not the form of capability names run nothing. */
    HOST_TIER("official");
    static const char *const bad_grants[] = { "hold;reboot", "--consent-unverified", "hold,,job_time.run", "HOLD", "hold job",
                                              "a,b,c,d,e,f,g,h,i", "net.outbound:evil.example:443" };
    for (size_t i = 0; i < sizeof(bad_grants) / sizeof(bad_grants[0]); i++)
        CHECK(extpkg_install(bad_grants[i], NULL, 1, &status, why, sizeof(why)) != 0 && status == 400 && !INSTALL_RAN()[0],
              "grants \"%s\": %d, ran [%s]", bad_grants[i], status, INSTALL_RAN());

    /* The host's refusal of the install, in its words; the staged file stays for another try. */
    snprintf(script, sizeof(script), "if [ \"$1\" = inspect ]; then echo '{\"ok\": true, \"tier\": \"official\"}'; exit 0; fi\n"
             "echo '{\"ok\": false, \"error\": \"only the operator grants: hold\"}'; exit 1");
    host_says(script);
    write_file(stage, "an archive", 0600);
    CHECK(extpkg_install(NULL, NULL, 0, &status, why, sizeof(why)) != 0 && status == 409 && strstr(why, "only the operator grants") &&
          access(stage, F_OK) == 0, "the host's refusal: %d %s", status, why);
    extpkg_stage_discard();
    CHECK(access(stage, F_OK) != 0, "discard left the staged file");

    /* The owner's keys: the button, the name's form, and the key through a
     * file and never an argument. */
    host_says("echo '{\"ok\": true, \"keys\": [{\"name\": \"maker\", \"key\": \"ab\"}]}'");
    CHECK(extpkg_key_add("maker", "AAAA", 4, 0, &status, why, sizeof(why)) != 0 && status == 409 && strstr(why, "button") &&
          !ran()[0], "a key without the button held: %d %s, ran [%s]", status, why, ran());
    static const char *const bad_names[] = { "", "a maker", "../../etc/passwd", ".hidden", "-rf", "maker;reboot",
                                             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaX" };
    for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++)
        CHECK(extpkg_key_add(bad_names[i], "AAAA", 4, 1, &status, why, sizeof(why)) != 0 && status == 400 && !ran()[0],
              "the name \"%s\": %d, ran [%s]", bad_names[i], status, ran());
    CHECK(extpkg_key_add("maker", "", 0, 1, &status, why, sizeof(why)) != 0 && status == 400 && !ran()[0], "an empty key");
    {
        static char big[EXTPKG_KEY_MAX + 8];
        memset(big, 'A', sizeof(big) - 1);
        CHECK(extpkg_key_add("maker", big, sizeof(big) - 1, 1, &status, why, sizeof(why)) != 0 && status == 400 && !ran()[0],
              "a key over the bound");
    }
    CHECK(extpkg_key_add("maker", "AAAA", 4, 1, &status, why, sizeof(why)) == 0 && status == 200, "the key: %d %s", status, why);
    {
        /* key-add <name> <file>: the key is in the file, never in an argument. */
        char args[1024];
        snprintf(args, sizeof(args), "%s", ran());
        CHECK(strncmp(args, "key-add\nmaker\n/tmp/forgectrl-key.", 33) == 0 && !strstr(args, "AAAA"),
              "the host was run with [%s]", args);
    }
    host_says("echo '{\"ok\": false, \"error\": \"that is no Ed25519 public key\"}'; exit 1");
    CHECK(extpkg_key_add("maker", "not a key", 9, 1, &status, why, sizeof(why)) != 0 && status == 409 &&
          strstr(why, "Ed25519"), "what the host will not take: %d %s", status, why);
    host_says("echo '{\"ok\": true, \"keys\": []}'");
    CHECK(extpkg_key_remove("maker", &status, why, sizeof(why)) == 0 && !strcmp(ran(), "key-remove\nmaker\n"),
          "key-remove: %d, ran [%s]", status, ran());
    CHECK(extpkg_key_remove("../../etc/passwd", &status, why, sizeof(why)) != 0 && status == 400, "removing a path");
    /* and no key file is left behind */
    {
        int left = system("ls /tmp/forgectrl-key.* >/dev/null 2>&1");
        CHECK(left != 0, "a staged key file was left in /tmp");
    }
    /* the status document carries them */
    host_says("if [ \"$1\" = keys ]; then echo '{\"ok\": true, \"keys\": [{\"name\": \"maker\", \"key\": \"ab\"}]}'; exit 0; fi\n"
              "echo '{\"ok\": true, \"packages\": []}'");
    doc = extpkg_status_json(1);
    j = doc ? json_loads(doc, 0, NULL) : NULL;
    CHECK(j && json_array_size(json_object_get(j, "keys")) == 1, "the status document carries the keys: %s", doc ? doc : "none");
    json_decref(j);
    free(doc);

    /* A package's page and its settings. The host exits 1 when it refuses,
     * and the refusal still reaches the panel in its words, with the
     * route's own status; only a host that gives no answer is 502. */
    host_says("echo '{\"ok\": true, \"id\": \"org.example.notify\", \"bytes\": 2, \"html\": \"hi\"}'");
    doc = extpkg_ui_json("org.example.notify", &status, why, sizeof(why));
    CHECK(doc && status == 200 && !strcmp(ran(), "ui\norg.example.notify\n"), "the page: %d %s, ran [%s]", status,
          doc ? doc : why, ran());
    free(doc);
    host_says("echo '{\"ok\": false, \"error\": \"this package is disabled: its interface is not served\"}'; exit 1");
    doc = extpkg_ui_json("org.example.notify", &status, why, sizeof(why));
    CHECK(!doc && status == 404 && strstr(why, "disabled"), "a disabled package's page: %d %s", status, why);
    free(doc);
    host_says("echo 'Segmentation fault'; exit 139");
    doc = extpkg_ui_json("org.example.notify", &status, why, sizeof(why));
    CHECK(!doc && status == 502, "a page from a host that gives no JSON: %d %s", status, why);
    free(doc);
    host_says("echo '{\"ok\": true}'");
    doc = extpkg_ui_json("org.example;reboot", &status, why, sizeof(why));
    CHECK(!doc && status == 400 && !ran()[0], "the page of an id that is none: %d, ran [%s]", status, ran());
    free(doc);

    host_says("echo '{\"ok\": true, \"id\": \"org.example.notify\", \"settings\": {\"threshold\": 40}}'");
    doc = extpkg_settings_json("org.example.notify", NULL, &status, why, sizeof(why));
    CHECK(doc && status == 200 && !strcmp(ran(), "settings\norg.example.notify\n"), "the settings: %d %s, ran [%s]", status,
          doc ? doc : why, ran());
    free(doc);
    host_says("echo '{\"ok\": false, \"error\": \"threshold is at most 100\"}'; exit 1");
    doc = extpkg_settings_json("org.example.notify", "{\"threshold\": 101}", &status, why, sizeof(why));
    CHECK(!doc && status == 400 && strstr(why, "at most 100") && !strcmp(ran(), "settings\norg.example.notify\n{\"threshold\": 101}\n"),
          "a patch the host refuses: %d %s, ran [%s]", status, why, ran());
    free(doc);
    host_says("exit 1");
    doc = extpkg_settings_json("org.example.notify", NULL, &status, why, sizeof(why));
    CHECK(!doc && status == 502, "settings from a host that says nothing: %d %s", status, why);
    free(doc);

    /* A page's call to its own service: exactly the host's command, the
     * form held before the host runs, the host's refusal in its words. */
    host_says("echo '{\"ok\": true, \"id\": \"org.example.notify\", \"status\": 200, \"body\": {\"a\": 1}}'");
    doc = extpkg_call_json("org.example.notify", "POST", "/rules", "{\"a\": 1}", &status, why, sizeof(why));
    CHECK(doc && status == 200 && !strcmp(ran(), "call\norg.example.notify\nPOST\n/rules\n{\"a\": 1}\n"),
          "a call: %d %s, ran [%s]", status, doc ? doc : why, ran());
    free(doc);
    doc = extpkg_call_json("org.example.notify", "GET", "/rules", NULL, &status, why, sizeof(why));
    CHECK(doc && status == 200 && !strcmp(ran(), "call\norg.example.notify\nGET\n/rules\n"), "a GET: %d, ran [%s]", status, ran());
    free(doc);
    static const struct { const char *id, *method, *path, *body; } bad_calls[] = {
        { "org.example;reboot", "GET", "/rules", NULL }, { "org.example.notify", "PUT", "/rules", NULL },
        { "org.example.notify", NULL, "/rules", NULL }, { "org.example.notify", "GET", "--call-dir", NULL },
        { "org.example.notify", "GET", NULL, NULL }, { "org.example.notify", "POST", "/rules", "--call-dir" },
        { "org.example.notify", "POST", "/rules", "[1]" }, { "org.example.notify", "GET", "/rules", "{}" },
    };
    for (size_t i = 0; i < sizeof(bad_calls) / sizeof(bad_calls[0]); i++) {
        host_says("echo '{\"ok\": true}'");
        doc = extpkg_call_json(bad_calls[i].id, bad_calls[i].method, bad_calls[i].path, bad_calls[i].body, &status, why,
                               sizeof(why));
        CHECK(!doc && status == 400 && why[0] && !ran()[0], "call %zu out of form: %d %s, ran [%s]", i, status, why, ran());
        free(doc);
    }
    static char big[EXTPKG_CALL_BODY_MAX + 8];
    memset(big, 'x', sizeof(big) - 1);
    big[0] = '{';
    doc = extpkg_call_json("org.example.notify", "POST", "/rules", big, &status, why, sizeof(why));
    CHECK(!doc && status == 400 && !ran()[0], "a body past the limit: %d, ran [%s]", status, ran());
    free(doc);
    host_says("echo '{\"ok\": false, \"error\": \"its service is frozen while a job is armed\"}'; exit 1");
    doc = extpkg_call_json("org.example.notify", "GET", "/rules", NULL, &status, why, sizeof(why));
    CHECK(!doc && status == 409 && !strcmp(why, "its service is frozen while a job is armed"), "the host's refusal: %d %s", status, why);
    free(doc);
    host_says("echo 'Segmentation fault'; exit 139");
    doc = extpkg_call_json("org.example.notify", "GET", "/rules", NULL, &status, why, sizeof(why));
    CHECK(!doc && status == 502, "a call through a host that gives no JSON: %d %s", status, why);
    free(doc);

    /* A destination the operator names: exactly the host's command, the
     * form held before it runs, the host's refusal in its words. */
    host_says("echo '{\"ok\": true, \"id\": \"org.example.notify\", \"destinations\": [\"plug.lan:80\"]}'");
    CHECK(extpkg_dest("org.example.notify", "add", "plug.lan:80", &status, why, sizeof(why)) == 0 && status == 200
          && !strcmp(ran(), "dest\norg.example.notify\nadd\nplug.lan:80\n"), "a destination added: %d %s, ran [%s]", status,
          why, ran());
    CHECK(extpkg_dest("org.example.notify", "remove", "[2001:db8::7]:1883", &status, why, sizeof(why)) == 0
          && !strcmp(ran(), "dest\norg.example.notify\nremove\n[2001:db8::7]:1883\n"), "a destination removed, ran [%s]", ran());
    static const struct { const char *id, *action, *dest; } bad_dests[] = {
        { "org.example;x", "add", "plug.lan:80" }, { "org.example.notify", "move", "plug.lan:80" },
        { "org.example.notify", NULL, "plug.lan:80" }, { "org.example.notify", "add", NULL },
        { "org.example.notify", "add", "" }, { "org.example.notify", "add", "--root" },
        { "org.example.notify", "add", "plug.lan:80 --root" }, { "org.example.notify", "add", "Plug.lan:80" },
    };
    for (size_t i = 0; i < sizeof(bad_dests) / sizeof(bad_dests[0]); i++) {
        host_says("echo '{\"ok\": true}'");
        CHECK(extpkg_dest(bad_dests[i].id, bad_dests[i].action, bad_dests[i].dest, &status, why, sizeof(why)) != 0
              && status == 400 && why[0] && !ran()[0], "destination %zu out of form: %d %s, ran [%s]", i, status, why, ran());
    }
    host_says("echo '{\"ok\": false, \"error\": \"127.0.0.1 is this machine: no package reaches the machine\"}'; exit 1");
    CHECK(extpkg_dest("org.example.notify", "add", "127.0.0.1:80", &status, why, sizeof(why)) != 0 && status == 409
          && strstr(why, "is this machine"), "the host's refusal: %d %s", status, why);
    host_says("echo 'Segmentation fault'; exit 139");
    CHECK(extpkg_dest("org.example.notify", "add", "plug.lan:80", &status, why, sizeof(why)) != 0 && status == 502,
          "a host that gives no JSON: %d", status);

    /* The catalog. A stand-in for curl records its arguments, and writes
     * the -o file from curl.body, or fails in curl's words from curl.fail. */
    char curl[300], curl_args[300], curl_body[300], curl_fail[300], idx_stage[340], text[1024], cscript[4096];
    snprintf(curl, sizeof(curl), "%s/curl", dir);
    snprintf(curl_args, sizeof(curl_args), "%s/curl.args", dir);
    snprintf(curl_body, sizeof(curl_body), "%s/curl.body", dir);
    snprintf(curl_fail, sizeof(curl_fail), "%s/curl.fail", dir);
    snprintf(idx_stage, sizeof(idx_stage), "%s/ext-index.ffi", dir);
    snprintf(cscript, sizeof(cscript),
             "#!/bin/sh\nfor a in \"$@\"; do echo \"$a\"; done > %s\nout=; prev=\n"
             "for a in \"$@\"; do [ \"$prev\" = -o ] && out=\"$a\"; prev=\"$a\"; done\n"
             "if [ -f %s ]; then cat %s >&2; exit 22; fi\ncp %s \"$out\"\n", curl_args, curl_fail, curl_fail, curl_body);
    write_file(curl, cscript, 0755);
    setenv("FORGECTRL_CURL", curl, 1);
    setenv("FORGECTRL_EXT_INDEX_URL", "https://example.org/index.ffi", 1);
#define CURL_RAN() ({ static char t[1024]; FILE *f = fopen(curl_args, "r"); size_t n = f ? fread(t, 1, sizeof(t) - 1, f) : 0; \
    if (f) fclose(f); t[n] = 0; t; })

    host_says("echo '{\"ok\": true, \"index\": null}'");
    doc = extpkg_catalog_json(&status, why, sizeof(why));
    j = doc ? json_loads(doc, 0, NULL) : NULL;
    CHECK(j && status == 200 && json_is_null(json_object_get(j, "index")) && !json_object_get(j, "ok") &&
          !strcmp(json_string_value(json_object_get(j, "url")), "https://example.org/index.ffi") && !strcmp(ran(), "index\n"),
          "no index kept: %s, ran [%s]", doc ? doc : why, ran());
    json_decref(j);
    free(doc);

    /* The fetch: curl, https alone and bounded, into the file beside the
     * staged package, which the host verifies and which is not kept. */
    write_file(curl_body, "an index", 0644);
    snprintf(cscript, sizeof(cscript), "[ \"$1\" = index-verify ] && [ \"$(cat \"$2\")\" = 'an index' ] && "
             "echo '{\"ok\": true, \"version\": \"2026.9.23\", \"packages\": 2}' && exit 0\n"
             "echo '{\"ok\": false, \"error\": \"not what was fetched\"}'; exit 1");
    host_says(cscript);
    int n = -1;
    char version[40];
    int rc = extpkg_catalog_refresh(&n, version, sizeof(version), &status, why, sizeof(why));
    snprintf(want_any, sizeof(want_any), "-fsS\n-L\n--proto\n=https\n--proto-redir\n=https\n--max-redirs\n5\n--max-time\n30\n"
             "--max-filesize\n2097152\n-o\n%s\nhttps://example.org/index.ffi\n", idx_stage);
    CHECK(rc == 0 && status == 200 && n == 2 && !strcmp(version, "2026.9.23"), "the catalog fetched and kept: %d %s", status, why);
    CHECK(!strcmp(CURL_RAN(), want_any), "curl ran with [%s]", CURL_RAN());
    snprintf(want_any, sizeof(want_any), "index-verify\n%s\n", idx_stage);
    CHECK(!strcmp(ran(), want_any) && access(idx_stage, F_OK) != 0, "the host verified the fetched file, and it is gone: [%s]",
          ran());

    write_file(curl_fail, "curl: (22) The requested URL returned error: 404", 0644);
    host_says("echo '{\"ok\": true}'");
    rc = extpkg_catalog_refresh(&n, version, sizeof(version), &status, why, sizeof(why));
    CHECK(rc != 0 && status == 502 && strstr(why, "could not be fetched") && strstr(why, "404") && !ran()[0] &&
          access(idx_stage, F_OK) != 0, "a fetch that fails: %d %s, ran [%s]", status, why, ran());
    unlink(curl_fail);
    host_says("echo '{\"ok\": false, \"error\": \"the index is not signed with the OpenGlow extension key\"}'; exit 1");
    rc = extpkg_catalog_refresh(&n, version, sizeof(version), &status, why, sizeof(why));
    CHECK(rc != 0 && status == 409 && strstr(why, "OpenGlow extension key") && access(idx_stage, F_OK) != 0,
          "an index the host refuses: %d %s", status, why);

    /* A listed package: where it is and what it is are the kept index's. */
    static const char body[] = "the package's bytes";
    unsigned char dg[SHA256_LEN];
    char hex[2 * SHA256_LEN + 1];
    sha256(body, strlen(body), dg);
    sha256_hex(dg, SHA256_LEN, hex);
    write_file(curl_body, body, 0644);
#define HOST_INDEX(url, sha, size, pid) do { \
    snprintf(text, sizeof(text), "{\"ok\": true, \"index\": {\"version\": \"1\", \"packages\": [{\"id\": \"org.example.other\"}, " \
             "{\"id\": \"org.example.listed\", \"url\": \"%s\", \"sha256\": \"%s\", \"size\": %d}]}}", url, sha, (int)(size)); \
    snprintf(cscript, sizeof(cscript), "if [ \"$1\" = index ]; then echo '%s'; exit 0; fi\n" \
             "echo '{\"ok\": true, \"tier\": \"community\", \"endorsed\": true, \"package\": {\"id\": \"%s\"}}'", text, pid); \
    host_says(cscript); unlink(curl_args); extpkg_stage_discard(); } while (0)

    HOST_INDEX("https://example.org/listed.ffx", hex, strlen(body), "org.example.listed");
    doc = extpkg_catalog_get("org.example.listed", &status, why, sizeof(why));
    j = doc ? json_loads(doc, 0, NULL) : NULL;
    CHECK(j && status == 200 && !strcmp(json_string_value(json_object_get(j, "consent")), "typed") &&
          json_is_true(json_object_get(j, "catalog")), "a listed package staged: %s", doc ? doc : why);
    json_decref(j);
    free(doc);
    snprintf(want_any, sizeof(want_any), "-fsS\n-L\n--proto\n=https\n--proto-redir\n=https\n--max-redirs\n5\n--max-time\n240\n"
             "--max-filesize\n%d\n-o\n%s\nhttps://example.org/listed.ffx\n", (int)strlen(body), stage);
    CHECK(!strcmp(CURL_RAN(), want_any), "curl for the package, bounded to its size: [%s]", CURL_RAN());
    snprintf(want_any, sizeof(want_any), "inspect\n%s\n", stage);
    CHECK(!strcmp(ran(), want_any) && access(stage, F_OK) == 0, "the host inspected the staged file, which stays: [%s]", ran());
    host_says("if [ \"$1\" = inspect ]; then echo '{\"ok\": true, \"tier\": \"community\", \"endorsed\": true}'; exit 0; fi\n"
              "echo '{\"ok\": true}'");
    CHECK(extpkg_install(NULL, NULL, 1, &status, why, sizeof(why)) != 0 && status == 400 && strstr(why, "catalog names") &&
          strstr(why, EXTPKG_PHRASE), "its install takes the phrase, in the catalog's words: %d %s", status, why);
    extpkg_stage_discard();

    static const struct { const char *what, *url; int sha_ok, size_delta; const char *pid; int status; const char *words; int curl; } gets[] = {
        { "bytes that are not the listed ones", "https://example.org/listed.ffx", 0, 0, "org.example.listed", 409, "SHA-256", 1 },
        { "bytes of another size", "https://example.org/listed.ffx", 1, 1, "org.example.listed", 409, "size", 1 },
        { "an archive of another package", "https://example.org/listed.ffx", 1, 0, "org.example.other", 409, "another package", 1 },
        { "an entry to fetch over http", "http://example.org/listed.ffx", 1, 0, "org.example.listed", 502, "out of form", 0 },
    };
    for (size_t k = 0; k < sizeof(gets) / sizeof(gets[0]); k++) {
        HOST_INDEX(gets[k].url, gets[k].sha_ok ? hex : "0000000000000000000000000000000000000000000000000000000000000000",
                   strlen(body) + gets[k].size_delta, gets[k].pid);
        doc = extpkg_catalog_get("org.example.listed", &status, why, sizeof(why));
        CHECK(!doc && status == gets[k].status && strstr(why, gets[k].words) && access(stage, F_OK) != 0 &&
              (access(curl_args, F_OK) == 0) == gets[k].curl, "%s: %d %s, curl %s", gets[k].what, status, why,
              access(curl_args, F_OK) == 0 ? "ran" : "did not run");
        free(doc);
    }
    HOST_INDEX("https://example.org/listed.ffx", hex, strlen(body), "org.example.listed");
    write_file(curl_fail, "curl: (6) Could not resolve host: example.org", 0644);
    doc = extpkg_catalog_get("org.example.listed", &status, why, sizeof(why));
    CHECK(!doc && status == 502 && strstr(why, "Could not resolve") && access(stage, F_OK) != 0, "a fetch that fails: %d %s", status, why);
    free(doc);
    unlink(curl_fail);
    unlink(curl_args);
    doc = extpkg_catalog_get("org.example.unlisted", &status, why, sizeof(why));
    CHECK(!doc && status == 404 && strstr(why, "not in the catalog") && access(curl_args, F_OK) != 0, "an id it does not list: %d %s",
          status, why);
    free(doc);
    doc = extpkg_catalog_get("org.example;reboot", &status, why, sizeof(why));
    CHECK(!doc && status == 400, "an id that is none: %d", status);
    free(doc);
    host_says("echo '{\"ok\": true, \"index\": null}'");
    doc = extpkg_catalog_get("org.example.listed", &status, why, sizeof(why));
    CHECK(!doc && status == 409 && strstr(why, "fetch it first") && access(curl_args, F_OK) != 0, "no index kept: %d %s", status, why);
    free(doc);

    char cmd[340];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0)
        fails++;
    printf(fails ? "extpkg_test: %d FAILED\n" : "extpkg_test: all passed\n", fails);
    return fails ? 1 : 0;
}
