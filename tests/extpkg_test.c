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
 * be run or says something that is no JSON is 502, and the status document
 * carries the host's own status only while that host is alive.
 */
#include "../src/extpkg.h"

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
    char script[2048];
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
          json_array_size(json_object_get(j, "packages")) == 1 && !strcmp(ran(), "list\n"), "the status document: %s", doc ? doc : "none");
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
    char expect[400];

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

    char cmd[340];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0)
        fails++;
    printf(fails ? "extpkg_test: %d FAILED\n" : "extpkg_test: all passed\n", fails);
    return fails ? 1 : 0;
}
