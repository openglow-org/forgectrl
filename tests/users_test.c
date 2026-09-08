/*
 * users_test.c - host test: the account and the sessions
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "../src/session.h"
#include "../src/users.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); failures++; } } while (0)

static time_t fake_now = 1000000;
static time_t clock_fn(void) { return fake_now; }

int main(void)
{
    char dir[] = "/tmp/users-test-XXXXXX";
    if (!mkdtemp(dir)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    setenv("FORGECTRL_DATA_DIR", dir, 1);
    char reason[128];

    /* Rules. */
    CHECK(users_check_name("owner", reason, sizeof(reason)) == 0, "a plain name passes");
    CHECK(users_check_name("Owner", reason, sizeof(reason)) == -1, "uppercase refused");
    CHECK(users_check_name("1st", reason, sizeof(reason)) == -1, "leading digit refused");
    CHECK(users_check_name("root", reason, sizeof(reason)) == -1, "root refused");
    CHECK(users_check_name("a", reason, sizeof(reason)) == -1, "one character refused");
    CHECK(users_check_name("with space", reason, sizeof(reason)) == -1, "space refused");
    CHECK(users_check_password("owner", "short", reason, sizeof(reason)) == -1, "short password refused");
    CHECK(users_check_password("owner", "owner", reason, sizeof(reason)) == -1, "name as password refused");
    CHECK(users_check_password("owner", "correct horse", reason, sizeof(reason)) == 0, "a passphrase passes");
    CHECK(users_check_password("owner", "has:colon1", reason, sizeof(reason)) == -1, "colon refused");

    /* No account yet. */
    users_init();
    CHECK(!users_exist(), "no account at first");
    CHECK(!users_verify("owner", "correct horse"), "nothing verifies without an account");

    /* Create, verify, wrong password, wrong name. */
    CHECK(users_create("owner", "correct horse", reason, sizeof(reason)) == 0, "account created");
    CHECK(users_exist(), "account exists");
    CHECK(users_verify("owner", "correct horse"), "the password verifies");
    CHECK(!users_verify("owner", "correct horsf"), "a wrong password fails");
    CHECK(!users_verify("owner2", "correct horse"), "a wrong name fails");
    char name[USERS_NAME_MAX + 1];
    CHECK(users_name(name, sizeof(name)) == 0 && !strcmp(name, "owner"), "the name reads back");

    /* The record: name:hash:uid, a sha512-crypt hash, mode 0600. */
    char path[300], line[256] = "";
    snprintf(path, sizeof(path), "%s/users", dir);
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(line, sizeof(line), f))
            line[0] = '\0';
        fclose(f);
    }
    CHECK(!strncmp(line, "owner:$6$", 9) && strstr(line, ":1000"), "record is name:$6$...:1000");

    /* Reload from the record. */
    users_init();
    CHECK(users_exist() && users_verify("owner", "correct horse"), "the record survives a reload");

    /* Reset: the account stops verifying until replaced. */
    users_mark_reset();
    CHECK(!users_exist() && users_reset_pending(), "reset pending");
    CHECK(!users_verify("owner", "correct horse"), "the old password stops working");
    CHECK(users_create("owner", "another passphrase", reason, sizeof(reason)) == 0, "re-created");
    CHECK(users_exist() && !users_reset_pending(), "reset cleared");
    CHECK(users_verify("owner", "another passphrase"), "the new password verifies");

    /* Sessions. */
    session_set_clock(clock_fn);
    char sid[SESSION_ID_HEX + 1], cookie[256], parsed[SESSION_ID_HEX + 1];
    CHECK(session_create("owner", sid, sizeof(sid)) == 0 && strlen(sid) == SESSION_ID_HEX,
          "a session is 64 hex characters");
    CHECK(session_valid(sid), "the session validates");
    CHECK(!session_valid("0000"), "a short id fails");
    char other[SESSION_ID_HEX + 1];
    memcpy(other, sid, sizeof(other));
    other[0] = other[0] == 'a' ? 'b' : 'a';
    CHECK(!session_valid(other), "a one-character difference fails");
    session_cookie_set(sid, cookie, sizeof(cookie));
    CHECK(strstr(cookie, "HttpOnly") && strstr(cookie, "Secure") && strstr(cookie, "SameSite=Strict"),
          "the cookie is HttpOnly, Secure, SameSite=Strict");
    char hdr[400];
    snprintf(hdr, sizeof(hdr), "theme=dark; " SESSION_COOKIE "=%s; other=1", sid);
    CHECK(session_from_cookie(hdr, parsed, sizeof(parsed)) && !strcmp(parsed, sid),
          "the id parses out of a cookie header");
    CHECK(!session_from_cookie("theme=dark", parsed, sizeof(parsed)), "no cookie, no id");
    fake_now += SESSION_IDLE_S + 1;
    CHECK(!session_valid(sid), "an idle session expires");
    session_create("owner", sid, sizeof(sid));
    session_end(sid);
    CHECK(!session_valid(sid), "an ended session is gone");

    /* The store: a session survives a restart of the daemon (the file
     * read back), an ended one does not, an idle one is dropped at the
     * load, and the file is root-only. */
    char store[300];
    snprintf(store, sizeof(store), "%s/sessions", dir);
    session_set_store(store);
    CHECK(session_create("owner", sid, sizeof(sid)) == 0, "a stored session is created");
    session_set_store(store);
    CHECK(session_valid(sid), "the session survives a reload of the store");
    struct stat stt;
    CHECK(stat(store, &stt) == 0 && (stt.st_mode & 0777) == 0600, "the store is mode 0600");
    session_end(sid);
    session_set_store(store);
    CHECK(!session_valid(sid), "an ended session is gone from the store");
    session_create("owner", sid, sizeof(sid));
    fake_now += SESSION_IDLE_S + 1;
    session_set_store(store);
    CHECK(!session_valid(sid), "an idle session is dropped at the load");
    session_set_store(NULL);
    unlink(store);

    /* The login throttle. */
    int left = 0;
    CHECK(!login_locked("10.0.0.5", &left), "fresh address not locked");
    for (int i = 0; i < LOGIN_FAILS; i++)
        login_failed("10.0.0.5");
    CHECK(login_locked("10.0.0.5", &left) && left > 0 && left <= LOGIN_LOCK_S,
          "five failures lock the address");
    CHECK(!login_locked("10.0.0.6", &left), "another address is not locked");
    fake_now += LOGIN_LOCK_S + 1;
    CHECK(!login_locked("10.0.0.5", &left), "the lock expires");
    login_failed("10.0.0.5");
    login_succeeded("10.0.0.5");
    for (int i = 0; i < LOGIN_FAILS - 1; i++)
        login_failed("10.0.0.5");
    CHECK(!login_locked("10.0.0.5", &left), "a success resets the count");

    unlink(path);
    snprintf(path, sizeof(path), "%s/users.reset", dir);
    unlink(path);
    rmdir(dir);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
