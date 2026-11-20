/*
 * users_test.c - host test: the account and the sessions
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "../src/session.h"
#include "../src/users.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, name) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); failures++; } } while (0)

#define ME "10.0.0.9"

static time_t fake_now = 1000000;
static time_t clock_fn(void) { return fake_now; }

static int record_named;
static int named_hook(void) { return record_named; }

/* Each replay thread leaves its mark (arg: the mark's path) just before
 * it asks for the replay: the stand-in waits for both marks, so without
 * the replay's lock the two runs always meet inside the stand-in. */
static void *replay_thread(void *arg)
{
    FILE *m = fopen((const char *)arg, "w");
    if (m)
        fclose(m);
    users_replay();
    return NULL;
}

static int count_lines(const char *path, const char *word)
{
    FILE *f = fopen(path, "r");
    char line[64];
    int n = 0;
    while (f && fgets(line, sizeof(line), f))
        n += strstr(line, word) != NULL;
    if (f)
        fclose(f);
    return n;
}

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

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
    CHECK(users_check_name("nobody", reason, sizeof(reason)) == -1, "a system account's name refused (nobody, whose group is nogroup)");
    CHECK(users_check_name("adm", reason, sizeof(reason)) == -1, "a system group's name refused");
    CHECK(users_check_name("ffx12", reason, sizeof(reason)) == -1, "an extension package's account name refused");
    CHECK(users_check_name("ffxa", reason, sizeof(reason)) == 0, "ffx and a letter is a plain name");

    /* The account record in fault: there and unreadable, or missing while
     * the setup record names an account; a pending reset is no fault. */
    char upath[300];
    snprintf(upath, sizeof(upath), "%s/users", dir);
    users_record_named = named_hook;
    FILE *uf = fopen(upath, "w");
    if (uf) {
        fputs("this is no record\n", uf);
        fclose(uf);
    }
    users_init();
    CHECK(!users_exist() && users_fault(), "an unreadable record is a fault");
    unlink(upath);
    users_init();
    CHECK(!users_exist() && !users_fault(), "no record and no account named: no fault");
    record_named = 1;
    CHECK(users_fault(), "no record while the setup record names an account is a fault");
    record_named = 0;

    /* The replay: one run at a time, and the reset runs it. */
    char script[300], rlog[300];
    snprintf(script, sizeof(script), "%s/replay.sh", dir);
    snprintf(rlog, sizeof(rlog), "%s/replay.log", dir);
    FILE *sf = fopen(script, "w");
    if (sf) {
        /* Both threads' marks first (3 s at most), then the run under a
         * lock file the shell itself makes with O_EXCL (set -C): a
         * mkdir lock is no lock where mkdir takes a directory made under
         * it as its own. */
        fprintf(sf, "#!/bin/sh\nD='%s'\ni=0\nwhile { [ ! -e $D/called.1 ] || [ ! -e $D/called.2 ]; } && [ $i -lt 60 ]; do\n"
                    "    sleep 0.05; i=$((i + 1))\ndone\n"
                    "if ( set -C; : > $D/replay.lock ) 2>/dev/null; then\n"
                    "    sleep 0.3\n    rm -f $D/replay.lock\n"
                    "else\n    echo overlap >> '%s'\nfi\necho run >> '%s'\n", dir, rlog, rlog);
        fclose(sf);
    }
    chmod(script, 0755);
    setenv("FORGECTRL_HOST_TEST", "1", 1);
    setenv("GF_USERS_REPLAY", script, 1);
    char mark1[320], mark2[320];
    snprintf(mark1, sizeof(mark1), "%s/called.1", dir);
    snprintf(mark2, sizeof(mark2), "%s/called.2", dir);
    pthread_t r1, r2;
    pthread_create(&r1, NULL, replay_thread, mark1);
    pthread_create(&r2, NULL, replay_thread, mark2);
    pthread_join(r1, NULL);
    pthread_join(r2, NULL);
    CHECK(count_lines(rlog, "run") == 2 && count_lines(rlog, "overlap") == 0, "two replays at once ran one at a time");

    /* The button reset clears a fault: the record unreadable, the reset
     * pending, the account step runs again. */
    uf = fopen(upath, "w");
    if (uf) {
        fputs("this is no record\n", uf);
        fclose(uf);
    }
    users_init();
    users_mark_reset();
    CHECK(!users_exist() && !users_fault(), "the button reset clears a fault (the record unreadable, the reset pending)");
    unlink(upath);

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

    /* Reset: the account stops verifying until replaced; the replay runs. */
    int runs = count_lines(rlog, "run");
    users_mark_reset();
    CHECK(count_lines(rlog, "run") == runs + 1, "the reset runs the replay");
    CHECK(!users_fault(), "a pending reset is no fault");
    CHECK(!users_exist() && users_reset_pending(), "reset pending");
    CHECK(!users_verify("owner", "correct horse"), "the old password stops working");
    CHECK(users_create("owner", "another passphrase", reason, sizeof(reason)) == 0, "re-created");
    CHECK(users_exist() && !users_reset_pending(), "reset cleared");
    CHECK(users_verify("owner", "another passphrase"), "the new password verifies");

    /* Sessions. */
    session_set_clock(clock_fn);
    char sid[SESSION_ID_HEX + 1], cookie[256], parsed[SESSION_ID_HEX + 1];
    CHECK(session_create("owner", ME, sid, sizeof(sid)) == 0 && strlen(sid) == SESSION_ID_HEX,
          "a session is 64 hex characters");
    CHECK(session_valid(sid, ME), "the session validates");
    CHECK(!session_valid("0000", ME), "a short id fails");
    char other[SESSION_ID_HEX + 1];
    memcpy(other, sid, sizeof(other));
    other[0] = other[0] == 'a' ? 'b' : 'a';
    CHECK(!session_valid(other, ME), "a one-character difference fails");
    session_cookie_set(sid, cookie, sizeof(cookie));
    CHECK(strstr(cookie, "HttpOnly") && strstr(cookie, "Secure") && strstr(cookie, "SameSite=Strict"),
          "the cookie is HttpOnly, Secure, SameSite=Strict");
    char hdr[400];
    snprintf(hdr, sizeof(hdr), "theme=dark; " SESSION_COOKIE "=%s; other=1", sid);
    CHECK(session_from_cookie(hdr, parsed, sizeof(parsed)) && !strcmp(parsed, sid),
          "the id parses out of a cookie header");
    CHECK(!session_from_cookie("theme=dark", parsed, sizeof(parsed)), "no cookie, no id");
    fake_now += SESSION_IDLE_S + 1;
    CHECK(!session_valid(sid, ME), "an idle session expires");
    session_create("owner", ME, sid, sizeof(sid));
    session_end(sid);
    CHECK(!session_valid(sid, ME), "an ended session is gone");

    /* The store: a session survives a restart of the daemon (the file
     * read back), an ended one does not, an idle one is dropped at the
     * load, and the file is root-only. */
    char store[300];
    snprintf(store, sizeof(store), "%s/sessions", dir);
    session_set_store(store);
    CHECK(session_create("owner", ME, sid, sizeof(sid)) == 0, "a stored session is created");
    session_set_store(store);
    CHECK(session_valid(sid, ME), "the session survives a reload of the store");
    /* A store read on a clock behind its stamps is another boot's. */
    fake_now -= 100;
    session_set_store(store);
    CHECK(!session_valid(sid, ME), "a session stamped ahead of the clock (another boot's) is dropped at the load");
    fake_now += 100;
    session_set_store(store);
    CHECK(session_create("owner", ME, sid, sizeof(sid)) == 0, "a stored session is created again");
    struct stat stt;
    CHECK(stat(store, &stt) == 0 && (stt.st_mode & 0777) == 0600, "the store is mode 0600");
    session_end(sid);
    session_set_store(store);
    CHECK(!session_valid(sid, ME), "an ended session is gone from the store");
    session_create("owner", ME, sid, sizeof(sid));
    fake_now += SESSION_IDLE_S + 1;
    session_set_store(store);
    CHECK(!session_valid(sid, ME), "an idle session is dropped at the load");
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

    /* The budget over every address: one failure each from enough of
     * them locks every login for the rest of the window. */
    fake_now += LOGIN_GLOBAL_S + 1;
    char a[32];
    for (int i = 0; i < LOGIN_GLOBAL_FAILS; i++) {
        snprintf(a, sizeof(a), "10.1.%d.%d", i / 200, i % 200);
        login_failed(a);
    }
    CHECK(login_locked("10.9.9.9", &left) && left > 0 && left <= LOGIN_GLOBAL_S,
          "the failures of many addresses lock a fresh one");
    fake_now += LOGIN_GLOBAL_S;
    CHECK(!login_locked("10.9.9.9", &left), "the global lock ends with its window");

    /* A session belongs to the address that logged in; a v4-mapped peer
     * is its IPv4 address. */
    CHECK(session_create("owner", "192.0.2.7", sid, sizeof(sid)) == 0, "a session from 192.0.2.7");
    CHECK(session_valid(sid, "192.0.2.7"), "valid from its own address");
    CHECK(session_valid(sid, "::ffff:192.0.2.7"), "valid from its address as v4-mapped IPv6");
    CHECK(!session_valid(sid, "192.0.2.8"), "the same cookie from another address is no session");
    session_end(NULL);

    /* The stamps are CLOCK_MONOTONIC's: the store says so. */
    session_set_clock(NULL);
    session_set_store(store);
    CHECK(session_create("owner", ME, sid, sizeof(sid)) == 0, "a session on the real clock");
    long created = -1, seen = -1;
    FILE *sfp = fopen(store, "r");
    if (sfp) {
        char id64[80];
        if (fscanf(sfp, "%79s %ld %ld", id64, &created, &seen) != 3)
            created = -1;
        fclose(sfp);
    }
    double mono = mono_now();
    CHECK(created >= 0 && created <= (long)mono + 1 && created >= (long)mono - 5,
          "the session's stamps are on CLOCK_MONOTONIC");
    session_set_store(NULL);
    unlink(store);

    /* The longest password: one more character never verifies, though
     * the longest stands in for it in the hash. */
    char maxpw[USERS_PASSWORD_MAX + 2];
    memset(maxpw, 'x', USERS_PASSWORD_MAX + 1);
    maxpw[USERS_PASSWORD_MAX] = '\0';
    CHECK(users_create("owner", maxpw, reason, sizeof(reason)) == 0 && users_verify("owner", maxpw),
          "an account with the longest password");
    maxpw[USERS_PASSWORD_MAX] = 'x';
    maxpw[USERS_PASSWORD_MAX + 1] = '\0';
    CHECK(!users_verify("owner", maxpw), "a password one over the longest never verifies");
    unlink(rlog);
    unlink(script);

    unlink(path);
    snprintf(path, sizeof(path), "%s/users.reset", dir);
    unlink(path);
    rmdir(dir);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
