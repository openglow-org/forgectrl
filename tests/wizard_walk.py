#!/usr/bin/env python3
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
"""Host test: the first-run page, walked once in a headless browser.

The setup wizard as embed.cmake bundles it (build/ui/wizard.html, with
the panel and the login page beside it) runs against the dev server's
mock backend (tools/devserver.py), started as a first run
(GF_MOCK_FIRST_RUN=1) with the intake fans not yet counted, so the
page opens their check after the forms. The pages are served as
main.c serves them: "/" is the wizard while the first run is incomplete
and the panel after it, /setup the wizard, /login the login page, each
with the token spliced and the daemon's page policy; every other request
goes to the mock.

With --daemon the page is the one the running daemon serves: the walk's
server relays every request to the daemon's HTTPS listener as the dev
server relays them to a machine (the Host of the daemon, the browser's
origin headers dropped), so the page, its token, the routes, the
session cookie and the gate are the daemon's own. The harness that
starts the daemon (tests/host_setup.sh) presses the button and scripts
the coolant loop; the walk asserts the first run as the page shows it.

The driver is a page of the walk's own that frames "/" from the same
origin and works the frame's document as an operator would, so nothing
is added to the pages under test and the walk follows the frame from
the wizard to the panel. The steps are in STEPS below: GET /wiz carries
the first-run fields the page reads, each with its type, and the page's
required steps are the setup's; the welcome; each document read to its
end and accepted (the typed phrase where it asks for one); the press
step, which the button takes; the account; preferences; the machine;
cloud mode left off; against the mock, the intake fans check run to its
result (through a prompt whose options are text, one shaped like code,
one with quotes, each shown as written and the code-shaped one answered
as written), and against the daemon, which the harness gives its fan
count, the intake fans shown counted and passed by; the choice, where Start using the machine lands on the panel with the setup
complete; one setting saved through the panel's save bar; and the login
page, which (against the mock) reads a plain-text answer as "sign-in
failed", never "no answer from the machine", refuses a wrong password
and signs the account in, the session read back from GET /wiz.

Against the mock, the walk then starts a diagnostic on the panel's Setup
tab. The daemon refuses a start (409, "machine is not idle") until the
cooling engine's tick takes the thermal hardware back from the diagnostic
before it. The walk has the mock refuse the next starts that way. A start
refused once goes again a second later, and the page shows the diagnostic
started and never the refusal. A start refused twice shows the refusal
after the second start, and the page sends no third. A start refused
for another reason (a diagnostic running) shows the refusal at once, and
the page sends no second. A start refused once and pressed again starts
once: the press cancels the first press's second start. A start refused
once, where another client starts the diagnostic and the operator aborts
it, never goes again: the Abort alone cancels the second start, and
clears the press's "starting" text. Then the System tab's Reset all
settings, with the mock's button not held: OK leaves the dialog open,
saying the button is not held, and Cancel closes it with nothing reset;
with a job that arms while the dialog is open, OK sends nothing and
says a job is waiting for the button press; and with the job armed
before it, the button opens no dialog and says so. The fuse view shows
the mock's plain-text refusal as it came. An upload the release key did
not sign (the walk's /__walk/mock stages one and holds the button) asks
through the hold dialog once its install is refused, and OK installs it
with confirm_unsigned=1. The cooling engine's banners, in the mock's
demos: a live failure offers no Acknowledge, one that waits does, a 409
says the failure still stands and a taken one clears the banner; two
notices go with one Dismiss. The Coolant loop form saves the flow
watch's four settings, and the Cooling card then shows the watch's hold
from GET /cool/flow. Last, with the button held, a reset the mock takes
closes the dialog, says the machine restarts, and opens "/" once the
mock went down and came back (the page's timers run at a tenth of their
length there, and the mock is down half a second).

  python3 -B tests/wizard_walk.py [build/ui] [--browser chrome|firefox]
  python3 -B tests/wizard_walk.py --daemon 127.0.0.1:18455

Exit 0 when every step passed, 1 on a failed step, 2 when the walk
could not run (no pages, no browser, no verdict in time).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import types
import urllib.request
from http.server import ThreadingHTTPServer
from urllib.parse import parse_qsl, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import devserver  # noqa: E402

PAGES = ("index", "wizard", "login")

# The prompt the walk puts on the intake fans check: the options a step
# offers are text, and a package's check writes its own. One is shaped
# like the code the page would run if it built a button's handler from
# the option, one carries quotes and markup; the page shows each as
# written and posts the code-shaped one as written.
PROMPT_HOSTILE = "No'); window.__ran = 1; ('"
PROMPT_OPTIONS = ["Yes", PROMPT_HOSTILE, 'He said "go" <b>&amp;</b>']
# Seconds the mock answers nothing after a reset it took: short, so the
# page's wait for the machine to come back takes a moment.
DOWN_S = 0.5
CHECK_PROMPT = ("choice", "intake-text", "How many intake fans run? An option is text.", PROMPT_OPTIONS)
BROWSERS = {
    "chrome": [r"C:\Program Files\Google\Chrome\Application\chrome.exe", "/usr/bin/google-chrome",
               "/usr/bin/google-chrome-stable", "/usr/bin/chromium", "/usr/bin/chromium-browser"],
    "firefox": [r"C:\Program Files\Mozilla Firefox\firefox.exe", "/usr/bin/firefox"],
}

RUNNER = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>First-run walk</title>
<style>
body { margin: 0; font: 13px system-ui, sans-serif; }
#verdict { padding: 6px 10px; white-space: pre-wrap; font-weight: 600; }
iframe { display: block; width: 1280px; height: 960px; border: 0; }
</style></head><body>
<div id="verdict">walking</div>
<iframe id="f" src="/"></iframe>
<script>
var F = document.getElementById('f'), log = [];
/* The account the walk makes: the mock's login takes this password alone. */
var NAME = 'walker', PW = 'correct horse';
/* The walk's plan: the check the page opens after the forms and the prompt
 * it shows there (null against the daemon, whose fans the harness counts),
 * how long its result may take,
 * whether the panel is signed out before the login (the daemon serves
 * the login page to no session), and whether the walk starts diagnostics
 * on the panel (against the mock alone). */
var PLAN = __PLAN__, PROMPT = PLAN.check && PLAN.check.prompt;
/* The fields the first step holds GET /wiz to, each with its type
 * (document 0 is the typed one). The page reads each of them on a first
 * run, but for these:
 *   first_run, gate and why: the walk reads them itself
 *   wizards and its entries: the panel reads them when it loads
 *   documents.0.hash: no page reads it (the page posts the ETag of
 *     GET /advisories/<id>); the host tests seed their records with it
 * The page also reads fields that a first run's GET /wiz does not carry:
 *   users.extension_packages: sent only while an owner reset is pending
 *   the extensions entries' id, title, done and unanswered: sent only
 *     when a package adds a check
 *   machine.model_label: neither backend sends it */
var READS = [['documents', 'array'], ['documents.0.id', 'string'], ['documents.0.title', 'string'],
  ['documents.0.consent', 'string'], ['documents.0.phrase', 'string'], ['documents.0.hash', 'string'],
  ['documents.0.accepted', 'boolean'],
  ['advisories_complete', 'boolean'], ['acceptance_done', 'boolean'], ['completed', 'boolean'],
  ['fans_counted', 'boolean'], ['first_run', 'boolean'], ['gate', 'string'], ['why', 'string'],
  ['versions', 'object'], ['missing', 'array'], ['recommended', 'array'], ['recommended.0.id', 'string'],
  ['extensions', 'array'],
  ['wizards', 'array'], ['wizards.0.id', 'string'], ['wizards.0.title', 'string'], ['wizards.0.done', 'number'],
  ['users.exists', 'boolean'], ['users.reset_pending', 'boolean'], ['users.name', 'string'],
  ['sheet_id', 'string'], ['clock.utc', 'string'], ['clock.set', 'boolean'], ['machine.firmware', 'string'],
  ['machine.build', 'string'], ['machine.camera', 'string'], ['machine.head.present', 'boolean'],
  ['machine.head.hw_id', 'string'], ['machine.head.version', 'string']];
function d() { return F.contentDocument; }
function w() { return F.contentWindow; }
function q(s) { var x = d(); return x ? x.querySelector(s) : null; }
function path() { try { return w().location.pathname; } catch (e) { return '?'; } }
function on(id) { var e = q('#' + id); return !!(e && e.classList.contains('on')); }
function shown(sel) { var e = q(sel); return !!(e && e.style.display !== 'none'); }
function sleep(ms) { return new Promise(function (ok) { setTimeout(ok, ms); }); }
function where() {
  var x = d(), steps = [], msgs = [];
  if (x) {
    steps = [].map.call(x.querySelectorAll('.wz-step.on'), function (e) { return e.id; });
    msgs = [].map.call(x.querySelectorAll('.msg'), function (e) { return e.textContent; }).filter(Boolean);
  }
  return 'the frame is at ' + path() + ', steps on ' + JSON.stringify(steps) + ', messages ' + JSON.stringify(msgs).slice(0, 400);
}
function until(what, test, ms) {
  var end = Date.now() + (ms || 15000);
  return (function again() {
    var ok = false;
    try { ok = test(); } catch (e) { ok = false; }
    if (ok) return Promise.resolve(ok);
    if (Date.now() > end) return Promise.reject(new Error('timed out waiting for ' + what + '; ' + where()));
    return sleep(50).then(again);
  })();
}
function el(sel) { var e = q(sel); if (!e) throw new Error('the page has no ' + sel + '; ' + where()); return e; }
function click(sel) { el(sel).click(); }
function set(sel, v, ev) { var e = el(sel); e.value = v; e.dispatchEvent(new (w().Event)(ev || 'change', { bubbles: true })); }
function api(p) { return fetch(p).then(function (r) { return r.json(); }); }
function at(o, dotted) { return dotted.split('.').reduce(function (x, k) { return x == null ? x : x[k]; }, o); }
/* The mock's state as the walk sets it (the walk's /__walk/mock). */
function walkMock(qs) { return fetch('/__walk/mock?' + qs, { method: 'POST' }).then(function (r) { return r.json(); }); }
/* until() over an asynchronous read. */
function untilGot(what, get, test, ms) {
  var end = Date.now() + (ms || 10000);
  return (function again() {
    return get().then(function (v) {
      if (test(v)) return v;
      if (Date.now() > end) throw new Error('timed out waiting for ' + what + '; ' + where());
      return sleep(50).then(again);
    });
  })();
}
/* The hold dialog: shown, and wholly closed (its backdrop gone too). */
function modalOpen() { var m = q('#holdmodal'); return !!(m && m.classList.contains('show')); }
function modalGone() { return !modalOpen() && q('#holdmodal').style.display === 'none' && !q('.modal-backdrop'); }
/* The controller's report to the mock: its armed window open or closed. */
function reportCtl(mode, armed) {
  return fetch('/cool/state?mode=' + mode + '&armed=' + armed, { method: 'POST', headers: { 'X-ForgeFIRM-Report': PLAN.report } })
    .then(function (r) { if (!r.ok) throw new Error('the mock refused the report: HTTP ' + r.status); });
}
/* The page's own timers at a tenth of their length, on the frame's window
 * until a navigation replaces it: the walk's clock, as a unit test fakes one. */
function fastTimers() {
  var W = w(), st = W.setTimeout, si = W.setInterval;
  W.setTimeout = function (f, ms) { return st.call(W, f, (ms || 0) / 10); };
  W.setInterval = function (f, ms) { return si.call(W, f, (ms || 0) / 10); };
}

var STEPS = [
  ['GET /wiz carries the first-run fields the page reads, each with its type, and the page\'s required steps are the setup\'s', function () {
    return until('the page', function () { return w().REQUIRED && w().W; })
      .then(function () { return api('/wiz'); })
      .then(function (s) {
        var lacking = READS.filter(function (r) {
          var v = at(s, r[0]);
          return r[1] === 'array' ? !Array.isArray(v) : (v === null || typeof v !== r[1]);
        }).map(function (r) { return r[0] + ' (' + r[1] + ')'; });
        if (lacking.length) throw new Error('GET /wiz lacks ' + lacking.join(', '));
        /* The gate holds the advisories, the account and the fan count on
         * their own; every other required step of the page is in missing
         * on a record with none of them done, in the setup's order. */
        var req = w().REQUIRED.filter(function (id) { return ['advisories', 'account', 'airflow.intakes'].indexOf(id) < 0; });
        if (JSON.stringify(req) !== JSON.stringify(s.missing))
          throw new Error('the page requires ' + JSON.stringify(req) + ', the setup ' + JSON.stringify(s.missing)); }); }],
  ['the first run serves the wizard at / and opens on the welcome', function () {
    return until('the welcome', function () { return path() === '/' && on('st-welcome') && q('#rail li'); })
      .then(function () { click('#st-welcome [onclick="go(\'advisories\')"]'); }); }],
  ['each document is read to its end and accepted', function () {
    var seen = [], typed = 0;
    function one() {
      if (!(w().W.documents || []).some(function (x) { return !x.accepted; })) return Promise.resolve();
      var id;
      return until('a document on the page', function () {
          id = w().curDoc;
          return id && w().DOCS[id] && q('#docview').querySelector('h1, h2, h3, p, li');
        }).then(function () {
          var v = el('#docview');
          v.scrollTop = v.scrollHeight;
          v.dispatchEvent(new (w().Event)('scroll'));
          return until(id + ' read to its end', function () { return q('#consent #phrase') || q('#consent #chk'); });
        }).then(function () {
          if (!q('#consent #phrase')) return click('#chk');
          typed++;
          set('#phrase', el('#consent b').textContent, 'input');
          return until('the confirm button', function () { return !q('#phrase-go').disabled; })
            .then(function () { click('#phrase-go'); });
        }).then(function () {
          return until(id + ' accepted', function () { return q('#dt-' + id).classList.contains('done'); });
        }).then(function () { seen.push(id); return one(); });
    }
    return until('the documents', function () { return on('st-advisories') && (w().W.documents || []).length; })
      .then(one)
      .then(function () {
        var listed = w().W.documents.map(function (x) { return x.id; });
        if (seen.slice().sort().join() !== listed.slice().sort().join() || !typed)
          throw new Error('accepted ' + JSON.stringify(seen) + ' (' + typed + ' typed) of ' + JSON.stringify(listed));
        return until('the way on', function () { return !q('#docs-next').disabled; });
      }).then(function () { click('#docs-next'); }); }],
  ['the press step waits for the button and takes the press', function () {
    return until('the press step', function () { return on('st-press'); })
      .then(function () {
        if (q('#pressled').classList.contains('pressed')) throw new Error('the press reads taken before the button was pressed');
        return until('the press taken', function () { return q('#pressled').classList.contains('pressed'); }, 20000); }); }],
  ['the account is made, and the rail names it', function () {
    return until('the account step', function () { return on('st-account'); })
      .then(function () { set('#a-name', NAME, 'input'); set('#a-pw', PW, 'input'); set('#a-pw2', PW, 'input'); click('#a-go'); })
      .then(function () {
        return until('the account made', function () { return on('st-preferences') && q('#rail').textContent.indexOf(NAME) >= 0; }); }); }],
  ['preferences', function () {
    return until('the preferences read', function () { return on('st-preferences') && q('#p-units').value; })
      .then(function () { set('#p-units', 'metric'); click('[onclick="savePreferences()"]'); })
      .then(function () { return until('the machine step', function () { return on('st-machine'); }); }); }],
  ['the machine', function () {
    click('#m-models input[value="basic"]');
    click('[onclick="saveMachine()"]');
    return until('the cloud step', function () { return on('st-cloud'); }); }],
  ['cloud mode left off', function () {
    return until('the cloud choice read', function () { return q('#st-cloud input[name=cloud][value="0"]').checked; })
      .then(function () { click('[onclick="saveCloud()"]'); })
      .then(function () { return until('the step after the cloud', function () { return !on('st-cloud') && q('.wz-step.on'); }); }); }],
  [PLAN.check ? 'the intake fans not yet counted: the page opens their check'
              : 'the intake fans show counted, and the page passes them by', function () {
    if (PLAN.check)
      return until('the intake fans check', function () { return on('st-dark') && w().K.id === PLAN.check.id; });
    var title = w().TITLES['airflow.intakes'];
    var row = [].filter.call(d().querySelectorAll('#rail li'), function (e) { return e.textContent.indexOf(title) === 0; })[0];
    if (!row || !row.classList.contains('done'))
      throw new Error('the rail does not show the intake fans counted: ' + (row ? row.outerHTML : 'no such row'));
    if (on('st-dark') && w().K.id === 'airflow.intakes') throw new Error('the page opened the intake fans check'); }],
  [PLAN.check ? 'the intake fans check runs to its result' + (PROMPT ? '; its prompt shows each option as text and takes the code-shaped one as written' : '')
              : 'no check opens between the forms and the choice', function () {
    if (!PLAN.check) return;
    click('#k-start');
    return Promise.resolve()
      .then(function () {
        if (!PROMPT) return;
        return until('its prompt', function () { return shown('#k-prompt') && q('#k-pbtns button'); }, 20000)
          .then(function () {
            var bs = [].slice.call(d().querySelectorAll('#k-pbtns button'));
            var got = bs.map(function (b) { return b.textContent; });
            if (JSON.stringify(got) !== JSON.stringify(PROMPT.options)) throw new Error('the prompt offers ' + JSON.stringify(got));
            var inline = bs.filter(function (b) { return b.getAttributeNames().some(function (n) { return n.indexOf('on') === 0; }); });
            if (inline.length) throw new Error('a prompt button carries inline script: ' + inline[0].outerHTML);
            var hit = bs.filter(function (b) { return b.getAttribute('data-v') === PROMPT.hostile; })[0];
            if (!hit) throw new Error('no button carries the code-shaped option as written: ' +
              JSON.stringify(bs.map(function (b) { return b.getAttribute('data-v'); })));
            hit.click();
            if (w().__ran !== undefined) throw new Error('the code-shaped option ran as script');
            return until('the answer taken', function () { return !q('#k-pbtns button'); });
          });
      })
      .then(function () { return until('its result', function () { return shown('#k-result') && shown('#k-next'); }, PLAN.check.result_ms); })
      .then(function () {
        if (PROMPT && w().__ran !== undefined) throw new Error('the code-shaped option ran as script');
        click('#k-next'); }); }],
  ['the choice offers Start using the machine', function () {
    return until('the choice', function () { return on('st-choice'); })
      .then(function () {
        var b = el('#st-choice [onclick="finish()"]');
        if (b.textContent.trim() !== 'Start using the machine') throw new Error('the choice offers ' + b.textContent);
        b.click(); }); }],
  ['Start using the machine lands on the panel, the setup complete', function () {
    return until('the panel', function () { return path() === '/' && q('#savebar') && typeof w().saveAll === 'function'; }, 20000)
      .then(function () { return api('/wiz'); })
      .then(function (s) {
        if (s.completed !== true || s.gate !== 'open') throw new Error('the setup reads ' + JSON.stringify({ completed: s.completed, gate: s.gate, why: s.why })); }); }],
  ['the panel saves a setting through its save bar', function () {
    var to;
    /* The panel's first GET /settings fills every field and takes it as
     * the field's saved value, so an edit made before it is lost. The
     * select has a value from its first option on, so the wait is for
     * that fill. */
    return until('the settings read into the panel', function () {
      return q('#ui_units') && w().orig && w().orig.ui_units !== undefined; })
      .then(function () {
        to = q('#ui_units').value === 'imperial' ? 'metric' : 'imperial';
        set('#ui_units', to);
        return until('the save bar', function () { return q('#savebar').classList.contains('on'); }); })
      .then(function () { click('#sb-save'); return until('the save', function () { return !q('#savebar').classList.contains('on'); }); })
      .then(function () { return api('/settings'); })
      .then(function (s) { if (s.ui_units !== to) throw new Error('ui_units reads ' + s.ui_units + ' after saving ' + to); }); }],
  ['the login page ' + (PLAN.plain_login ? 'reads an answer that is not JSON as a refusal (sign-in failed, never no answer), ' : '') +
   'refuses a wrong password and signs the account in', function () {
    if (PLAN.logout) click('#logoutbtn'); else F.src = '/login?next=%2F';
    return until('the login page', function () { return path() === '/login' && q('#lg'); })
      .then(function () {
        if (!PLAN.plain_login) return;
        return walkMock('login_plain=1')
          .then(function () {
            set('#name', NAME, 'input'); set('#password', PW, 'input'); click('#go');
            return until('the refusal of a plain-text answer', function () { return q('#msg').classList.contains('bad') && !q('#go').disabled; }); })
          .then(function () {
            if (q('#msg').textContent !== 'sign-in failed')
              throw new Error('a plain-text answer reads ' + JSON.stringify(q('#msg').textContent) + ', not sign-in failed'); }); })
      .then(function () { set('#name', NAME, 'input'); set('#password', 'not the password', 'input'); click('#go'); })
      .then(function () { return until('the refusal', function () { return q('#msg').classList.contains('bad') && !q('#go').disabled; }); })
      .then(function () { set('#password', PW, 'input'); click('#go'); })
      .then(function () { return until('the panel again', function () { return path() === '/' && q('#savebar'); }); })
      /* The daemon serves the panel to a loopback peer with no session,
       * and the walk's relay is one: the session is read, not inferred. */
      .then(function () { return api('/wiz'); })
      .then(function (s) {
        if (s.session !== true) throw new Error('the panel again, but GET /wiz reads no session: ' + JSON.stringify(s.session)); }); }]
];
/* The panel's diagnostic starts, against the mock alone. The walk sets how
 * many starts the mock refuses (busy). The daemon refuses a start that way
 * until the cooling engine's tick takes the thermal hardware back. The
 * walk reads back the answer to each start the page sent, with its time in
 * seconds after the first, as the mock took them. */
var REFUSAL = 'machine is not idle', VERIFY = '#s-diag button[onclick*="aa-offset-calibrate"]';
function engineBusy(n) { return fetch('/__walk/diag?busy=' + n, { method: 'POST' }); }
function startsWere(want) {
  return api('/__walk/diag').then(function (s) {
    var got = s.starts.map(function (x) { return x[0]; });
    if (JSON.stringify(got) !== JSON.stringify(want))
      throw new Error('the starts were answered ' + JSON.stringify(got) + ', not ' + JSON.stringify(want));
    return s; });
}
function answered(want) {
  return startsWere(want).then(function (s) {
    /* "A second later": at least 0.9 s, and under the 1.5 s the
     * twice-refused case waits for a third start. */
    if (s.starts[1][1] < 0.9 || s.starts[1][1] >= 1.5)
      throw new Error('the second start came ' + s.starts[1][1] + ' s after the first'); });
}
if (PLAN.diag) STEPS = STEPS.concat([
  ['a diagnostic start the cooling engine refuses goes again a second later, and the panel shows the diagnostic started, never the refusal', function () {
    click('#t-diag');
    return until('the Setup tab, its controls free', function () { return on('s-diag') && !el(VERIFY).disabled; })
      .then(function () { return engineBusy(1); })
      .then(function () {
        click(VERIFY);
        return until('the diagnostic started, or a refusal', function () {
          return (w().D.running && shown('#diagabort')) || q('#msg-d').textContent === REFUSAL; }, 10000); })
      .then(function () {
        if (q('#msg-d').textContent) throw new Error('the panel shows ' + JSON.stringify(q('#msg-d').textContent));
        return answered([409, 202]); })
      .then(function () {
        click('#diagabort');
        return until('the diagnostic ended, the controls free', function () { return !w().D.running && !el(VERIFY).disabled; }, 10000); }); }],
  ['a start the cooling engine refuses twice shows the refusal after the second start, and the panel sends no third', function () {
    return engineBusy(2)
      .then(function () {
        click(VERIFY);
        return until('the refusal', function () { return q('#msg-d').textContent === REFUSAL; }, 10000); })
      /* A third start would go a second after the second answer. */
      .then(function () { return sleep(1500); })
      .then(function () { return answered([409, 409]); })
      .then(function () {
        if (w().D.running || q('#msg-d').textContent !== REFUSAL)
          throw new Error('after the second refusal a diagnostic runs, or the panel shows ' + JSON.stringify(q('#msg-d').textContent)); }); }],
  ['a start refused for another reason (a diagnostic running) shows the refusal at once, and the panel sends no second', function () {
    var RUNNING = 'a diagnostic is already running';
    return engineBusy(0)
      .then(function () {
        click(VERIFY);
        return until('the diagnostic started', function () { return w().D.running && shown('#diagabort'); }, 10000); })
      .then(function () { return engineBusy(0); })
      .then(function () {
        w().startDiag('aa-offset-calibrate');
        return until('the refusal', function () { return q('#msg-d').textContent === RUNNING; }, 10000); })
      .then(function () { return startsWere([409]); })
      .then(function () { return sleep(1500); })
      .then(function () { return startsWere([409]); })
      .then(function () {
        if (q('#msg-d').textContent !== RUNNING)
          throw new Error('1.5 s after the refusal the panel shows ' + JSON.stringify(q('#msg-d').textContent));
        click('#diagabort');
        return until('the diagnostic ended, the controls free', function () { return !w().D.running && !el(VERIFY).disabled; }, 10000); }); }],
  ['a refused start pressed again starts once: the press cancels the first press\'s second start', function () {
    return engineBusy(1)
      .then(function () { click(VERIFY); return sleep(300); })
      .then(function () {
        click(VERIFY);
        return until('the diagnostic started', function () { return w().D.running && shown('#diagabort'); }, 2000); })
      /* The panel's poll of /diag/status can land between the Abort and
       * the refused press's second start: the walk has it land there. */
      .then(function () { click('#diagabort'); return sleep(100); })
      .then(function () {
        w().loadDiag();
        return until('the diagnostic ended', function () { return !w().D.running; }, 2000); })
      .then(function () { return sleep(1500); })
      .then(function () { return startsWere([409, 202]); })
      .then(function () { return api('/diag/status'); })
      .then(function (s) {
        if (s.running) throw new Error('1.5 s after the Abort, ' + s.tool + ' runs'); })
      .then(function () {
        return until('the controls free', function () { return !w().D.running && !el(VERIFY).disabled; }, 10000); }); }],
  ['a refused start\'s second start is canceled by the operator\'s Abort alone: another client starts the diagnostic, the operator aborts it, nothing starts again, and the Abort clears the press\'s starting text', function () {
    var t0;
    return engineBusy(1)
      .then(function () { t0 = Date.now(); click(VERIFY); return sleep(300); })
      /* Another client starts the diagnostic the refused press would start:
       * a start the panel did not press, so the Abort is the one cancel. */
      .then(function () { return w().fx('/diag/aa-offset-calibrate', { method: 'POST' }); })
      .then(function (r) {
        if (r.status !== 202) throw new Error('the other client\'s start was answered ' + r.status);
        w().loadDiag();
        return until('the diagnostic started', function () { return w().D.running && shown('#diagabort'); }, 2000); })
      .then(function () { click('#diagabort'); return sleep(100); })
      .then(function () {
        w().loadDiag();
        return until('the diagnostic ended', function () { return !w().D.running; }, 2000); })
      /* The refused press's second start would go a second after the first. */
      .then(function () { return sleep(Math.max(0, 1700 - (Date.now() - t0))); })
      .then(function () { return startsWere([409, 202]); })
      .then(function () { return api('/diag/status'); })
      .then(function (s) {
        if (s.running) throw new Error('1.7 s after the refused press, ' + s.tool + ' runs');
        if (q('#msg-d').textContent) throw new Error('after the Abort the panel shows ' + JSON.stringify(q('#msg-d').textContent)); })
      .then(function () {
        return until('the controls free', function () { return !w().D.running && !el(VERIFY).disabled; }, 10000); }); }],
  /* The mock's button is not held (the walk clears GF_MOCK_BUTTON). A
   * reset the mock took would take it down: /status answers after Cancel
   * only if nothing was reset. */
  ['the reset dialog stays open on OK while the machine button is not held, and says so; Cancel closes it, nothing reset', function () {
    var open = function () { var m = q('#holdmodal'); return !!(m && m.classList.contains('show')); };
    /* Bootstrap ignores a hide while the dialog is still fading in. */
    var opened = false;
    click('#t-system');
    return until('the System tab', function () { return on('s-system'); })
      .then(function () {
        el('#holdmodal').addEventListener('shown.bs.modal', function () { opened = true; });
        click('button[onclick="doReset()"]');
        return until('the reset dialog', function () { return opened && open(); }); })
      .then(function () {
        click('#hold-ok');
        return until('a refusal in the dialog', function () { return shown('#hold-err') && !el('#hold-ok').disabled; }, 10000); })
      .then(function () {
        if (!open()) throw new Error('the dialog closed on the refusal');
        if (q('#hold-err').textContent !== w().HOLD_NOT_HELD)
          throw new Error('the dialog says ' + JSON.stringify(q('#hold-err').textContent) + ', not that the button is not held');
        click('#hold-cancel');
        return until('the dialog closed', function () { return !open() && q('#holdmodal').style.display === 'none'; }, 5000); })
      .then(function () {
        if (q('#msg-reset').textContent) throw new Error('after Cancel the panel shows ' + JSON.stringify(q('#msg-reset').textContent));
        return api('/status'); }); }],
  /* The dialog can stay open a long time: a job that starts waiting for
   * its press while it is open would take the hold OK asks for. The mock's
   * controller reports its armed window; OK then says a job is waiting, in
   * the page's own words (the mock's refusal has other words: the page
   * sent nothing), and the dialog stays open. */
  ['a job that arms while the reset dialog is open: OK sends nothing and says a job waits; the dialog stays open', function () {
    var open = function () { var m = q('#holdmodal'); return !!(m && m.classList.contains('show')); };
    var report = function (mode, armed) {
      return fetch('/cool/state?mode=' + mode + '&armed=' + armed, { method: 'POST', headers: { 'X-ForgeFIRM-Report': PLAN.report } })
        .then(function (r) { if (!r.ok) throw new Error('the mock refused the report: HTTP ' + r.status); }); };
    var opened = false;
    el('#holdmodal').addEventListener('shown.bs.modal', function () { opened = true; });
    /* The dialog before it is closed for the page once its backdrop has
     * faded out, a moment after the dialog itself is gone. */
    return until('the dialog before it wholly closed', function () { return !q('.modal-backdrop'); }, 5000)
      .then(function () { return sleep(300); })
      .then(function () {
        click('button[onclick="doReset()"]');
        return until('the reset dialog', function () { return opened && open(); }); })
      .then(function () { return report('run', 1); })
      .then(function () { return until('the panel reads the armed window', function () { return w().CL && w().CL.armed; }, 10000); })
      .then(function () {
        click('#hold-ok');
        return until('a refusal in the dialog', function () { return shown('#hold-err') && !el('#hold-ok').disabled; }, 10000); })
      .then(function () {
        if (!open()) throw new Error('the dialog closed on the refusal');
        if (q('#hold-err').textContent !== w().HOLD_JOB_WAITS)
          throw new Error('the dialog says ' + JSON.stringify(q('#hold-err').textContent) + ', not that a job is waiting');
        return report('idle', 0); })
      .then(function () { return until('the panel reads the window closed', function () { return w().CL && !w().CL.armed; }, 10000); })
      .then(function () {
        click('#hold-cancel');
        return until('the dialog closed', function () { return !open() && q('#holdmodal').style.display === 'none'; }, 5000); })
      .then(function () { return api('/status'); }); }],
  /* The card's button itself, with the panel reading an armed job: no
   * dialog opens at all, and the card says why. */
  ['the Reset all settings button under an armed job opens no dialog, and says a job waits', function () {
    var asked = false;
    el('#holdmodal').addEventListener('show.bs.modal', function () { asked = true; });
    return until('the dialog before it wholly closed', modalGone, 5000)
      .then(function () { return reportCtl('run', 1); })
      .then(function () { w().loadCool(); return until('the panel reads the armed window', function () { return w().CL && w().CL.armed; }, 5000); })
      .then(function () {
        click('button[onclick="doReset()"]');
        if (asked || modalOpen()) throw new Error('the reset dialog opened under an armed job');
        if (q('#msg-reset').textContent !== w().HOLD_JOB_WAITS)
          throw new Error('the card says ' + JSON.stringify(q('#msg-reset').textContent) + ', not that a job is waiting');
        return reportCtl('idle', 0); })
      .then(function () { w().loadCool(); return until('the panel reads the window closed', function () { return w().CL && !w().CL.armed; }, 5000); }); }],
  /* The mock's button is up: the daemon refuses the fuse view in plain
   * text, and the page shows that text as it came. */
  ['the fuse view shows the daemon\'s plain-text refusal as it came', function () {
    var said;
    return w().fx('/fuse-identity')
      .then(function (r) { return r.text(); })
      .then(function (t) {
        said = t;
        q('#msg-i').textContent = '';
        click('button[onclick="showFuse()"]');
        return until('the refusal', function () { return q('#msg-i').textContent; }, 5000); })
      .then(function () {
        if (!said || q('#msg-i').textContent !== said)
          throw new Error('the fuse view shows ' + JSON.stringify(q('#msg-i').textContent) + ', the daemon said ' + JSON.stringify(said)); }); }],
  /* An upload the release key did not sign: its install ends refused, the
   * page asks through the hold dialog, and OK (the button held) sends the
   * install again with confirm_unsigned=1. The walk ends each mock job at
   * once, and polls through the page's own poll (every 2 s on its own). */
  ['an archive the release key did not sign asks through the hold dialog, and OK installs it with confirm_unsigned=1', function () {
    var opened = false;
    el('#holdmodal').addEventListener('shown.bs.modal', function () { opened = true; });
    return walkMock('unsigned=1&button=1')
      .then(function () { w().loadSlots(); return until('a slot to install to', function () { return q('#applyslot') && q('#applyslot').value; }, 5000); })
      .then(function () {
        w().confirm = function () { return true; };   /* the browser's own OK */
        w().applyStaged('upload', 'the upload');
        return untilGot('the install job running', function () { return walkMock('job_end=0'); }, function (s) { return s.running; }, 5000); })
      .then(function () { return walkMock('job_end=1'); })
      .then(function () {
        return until('the hold dialog for the unsigned archive', function () { if (!opened) w().jobPoll(); return opened && modalOpen(); }, 5000); })
      .then(function () { click('#hold-ok'); return until('the dialog closed on the install', function () { return !modalOpen(); }, 5000); })
      .then(function () { return walkMock('job_end=1&button=0'); })
      .then(function (s) {
        if (!s.job || s.job.unsigned !== true || s.job.file !== 'upload' || s.running)
          throw new Error('the install went again as ' + JSON.stringify(s)); }); }],
  /* The cooling engine's attention, in the mock's demos: live, a failure
   * that still stands; ack, one that waits for its Acknowledge. */
  ['the attention banner: a live failure offers no Acknowledge; one that waits does, a refusal says the failure still stands, and a taken one clears the banner', function () {
    var b = function () { return q('#attn-banner'); }, ack;
    return walkMock('cool=live')
      .then(function () { w().loadCool(); return until('the live banner', function () { return shown('#attn-banner') && b().textContent; }, 5000); })
      .then(function () {
        if (b().querySelector('button')) throw new Error('a live failure offers the Acknowledge');
        return walkMock('cool=ack'); })
      .then(function () { w().loadCool(); return until('the Acknowledge', function () { return ack = b().querySelector('button'); }, 5000); })
      /* the cause back before the click: the mock refuses the Acknowledge, 409 */
      .then(function () { return walkMock('cool=live'); })
      .then(function () {
        q('#msg-changed').textContent = '';
        ack.click();
        return until('the refusal', function () { return q('#msg-changed').textContent; }, 5000); })
      .then(function () {
        if (q('#msg-changed').textContent.indexOf('the failure still stands') < 0)
          throw new Error('the refused Acknowledge says ' + JSON.stringify(q('#msg-changed').textContent));
        return walkMock('cool=ack'); })
      .then(function () { w().loadCool(); return until('the Acknowledge again', function () { return ack = b().querySelector('button'); }, 5000); })
      .then(function () {
        ack.click();
        return until('the banner gone, the Acknowledge taken', function () { return !shown('#attn-banner'); }, 5000); }); }],
  /* Two notices, seq 1 and 3: one Dismiss takes both, so it sends the newest. */
  ['the notices banner lists each notice, and its Dismiss takes every one shown', function () {
    return walkMock('cool=notice')
      .then(function () { w().loadCool(); return until('the notices', function () { return shown('#notice-banner') && q('#notice-banner a'); }, 5000); })
      .then(function () {
        var n = q('#notice-banner').querySelectorAll('div').length;
        if (n !== 2) throw new Error('the banner lists ' + n + ' notices, not 2');
        q('#notice-banner a').click();
        return until('the banner gone', function () { return !shown('#notice-banner'); }, 5000); })
      .then(function () { return walkMock('cool='); }); }],
  /* The Coolant loop form's four flow watch keys, saved through the save
   * bar; the watch then runs in the mock, and the Cooling card shows its
   * hold's duty (3.31 %) from GET /cool/flow. */
  ['the Coolant loop form saves the flow watch\'s four settings, and the Cooling card shows the watch from GET /cool/flow', function () {
    var want = { cool_flow_watch: '1', cool_flow_duty_min: '1', cool_flow_duty_max: '20' };
    return until('the settings read into the panel', function () { return w().orig && w().orig.cool_flow_watch !== undefined; })
      .then(function () {
        set('#cool_flow_watch', '1');
        set('#cool_flow_hold_c', '2');
        set('#cool_flow_duty_min', '1');
        set('#cool_flow_duty_max', '20');
        return until('the save bar', function () { return q('#savebar').classList.contains('on'); }); })
      .then(function () { click('#sb-save'); return until('the save', function () { return !q('#savebar').classList.contains('on'); }, 5000); })
      .then(function () { return api('/settings'); })
      .then(function (s) {
        var got = {};
        Object.keys(want).forEach(function (k) { got[k] = s[k]; });
        if (JSON.stringify(got) !== JSON.stringify(want) || !s.cool_flow_hold_c)
          throw new Error('the flow watch settings read ' + JSON.stringify(got) + ', step ' + JSON.stringify(s.cool_flow_hold_c));
        w().loadCool();
        return until('the flow watch on the Cooling card', function () { return q('#cooling').textContent.indexOf('3.31%') >= 0; }, 5000); }); }],
  /* Last, as it reloads the page: the button held, a reset the mock
   * takes. The dialog closes, the card says the machine restarts, and once
   * the mock went down and came back the page opens "/" again: a new
   * document in the frame. */
  ['a taken reset closes the dialog, says the machine restarts, and opens "/" once the machine went down and came back', function () {
    var doc, m, opened = false;
    return until('the dialog before it wholly closed', modalGone, 5000)
      .then(function () { return walkMock('button=1'); })
      .then(function () {
        doc = d();
        m = el('#holdmodal');
        m.addEventListener('shown.bs.modal', function () { opened = true; });
        fastTimers();
        click('button[onclick="doReset()"]');
        return until('the reset dialog', function () { return opened && modalOpen(); }); })
      .then(function () {
        click('#hold-ok');
        return until('the dialog closed, the card saying the machine restarts', function () {
          return !m.classList.contains('show') && doc.getElementById('msg-reset').textContent; }, 5000); })
      .then(function () { return until('"/" opened again', function () { return d() !== doc && path() === '/' && q('#savebar'); }, 10000); })
      .then(function () { return walkMock('button=0'); }); }]
]);

function run() {
  var s = Promise.resolve();
  STEPS.forEach(function (st) {
    s = s.then(function () { return Promise.resolve().then(st[1]).then(function () { log.push('PASS ' + st[0]); }); });
  });
  return s.then(function () { return true; }, function (e) { log.push('FAIL ' + ((e && e.message) || e)); return false; })
    .then(function (ok) {
      document.getElementById('verdict').textContent = (ok ? 'PASS\n' : 'FAIL\n') + log.join('\n');
      return fetch('/__walk/result', { method: 'POST', body: JSON.stringify({ ok: ok, log: log }) });
    });
}
run();
</script></body></html>
"""


class Walk(devserver.Handler):
    """The dev server's handler with main.c's page routes in front of the
    mock: the bundled pages, the walk's own page, and its verdict. With a
    daemon, its relay instead: the daemon serves its own pages."""
    pages = {}
    plan = {}
    daemon = None
    result = None
    said = []
    # The answers to the page's diagnostic starts since the walk last set
    # the mock's engine refusals: (status, monotonic seconds) each.
    starts = []
    # The next POST /login answers 502 with a plain-text body, as a proxy
    # in front of the machine might.
    login_plain = False

    def _say(self, msg):
        Walk.said.append(msg)

    def _send(self, code, headers, body):
        p = urlsplit(self.path).path
        if self.command == "POST" and p.startswith("/diag/") and p != "/diag/abort":
            Walk.starts.append((code, time.monotonic()))
        devserver.Handler._send(self, code, headers, body)

    def _mock(self, path, q):
        if self.command == "POST" and path == "/login" and Walk.login_plain:
            Walk.login_plain = False
            self._read_body()
            return self._send(502, {"Content-Type": "text/plain"}, b"bad gateway")
        devserver.Handler._mock(self, path, q)
        # A restart the mock took answers nothing for DOWN_S seconds.
        if self.command == "POST" and path == "/system/reset":
            with Walk.mock.lock:
                if Walk.mock.down_until:
                    Walk.mock.down_until = min(Walk.mock.down_until, time.time() + DOWN_S)

    def _route(self):
        u = urlsplit(self.path)
        q = dict(parse_qsl(u.query, keep_blank_values=True))
        if u.path == "/__walk/":
            page = RUNNER.replace("__PLAN__", json.dumps(Walk.plan), 1)
            return self._send(200, {"Content-Type": "text/html; charset=utf-8"}, page.encode())
        if u.path == "/__walk/result" and self.command == "POST":
            Walk.result = json.loads(self._read_body() or b"{}")
            return self._json(200, {"ok": True})
        if u.path == "/__walk/diag" and Walk.mock:
            if self.command == "POST":
                with Walk.mock.lock:
                    Walk.mock.diag_engine_busy = int(q.get("busy", "0"))
                Walk.starts = []
            t0 = Walk.starts[0][1] if Walk.starts else 0.0
            return self._json(200, {"starts": [[c, round(t - t0, 3)] for c, t in Walk.starts]})
        # The mock's state as the walk sets it: the machine button held or
        # not, the cooling demo (GF_MOCK_COOL's words, its notices undismissed),
        # an upload staged that the release key did not sign, the running
        # update job ended now (the walk's clock, not the mock's), the next
        # login answered with a body that is not JSON; and the update job,
        # read back.
        if u.path == "/__walk/mock" and Walk.mock:
            if self.command == "POST":
                with Walk.mock.lock:
                    if "button" in q:
                        Walk.mock.button = q["button"] == "1"
                    if "cool" in q:
                        Walk.mock.cool_demo = q["cool"]
                        Walk.mock.notices_seen = 0
                    if q.get("unsigned") == "1":
                        Walk.mock.slots["staged"]["upload"] = {"present": True, "bytes": 1,
                                                               "version": devserver.MOCK_RELEASE}
                        Walk.mock.upload_unsigned = True
                    if q.get("job_end") == "1" and Walk.mock.update["running"]:
                        Walk.mock._job_end()
                Walk.login_plain = Walk.login_plain or q.get("login_plain") == "1"
            return self._json(200, {"job": Walk.mock.job_args, "running": Walk.mock.update["running"]})
        if Walk.daemon:
            return self._proxy(u)
        if self.command in ("GET", "HEAD") and (u.path in ("/setup", "/login") or (u.path == "/" and "action" not in q)):
            if u.path == "/login":
                name = "login"
            elif u.path == "/setup" or not self.mock.wiz["completed"]:
                name = "wizard"
            else:
                name = "index"
            return self._send(200, dict(devserver.PAGE_HEADERS), self.pages[name])
        return devserver.Handler._route(self)


def load_pages(ui_dir):
    """The bundled pages with the token spliced where main.c splices it:
    in place of the first placeholder."""
    out = {}
    for name in PAGES:
        with open(os.path.join(ui_dir, name + ".html"), encoding="utf-8") as f:
            out[name] = f.read().replace(devserver.TOKEN_MARK, devserver.MOCK_TOKEN, 1).encode("utf-8")
    return out


def call(port, method, path, token=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), method=method,
                                 headers={"X-ForgeFIRM-Token": token} if token else {})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read() or b"{}")


def find_browser(name):
    for p in BROWSERS[name]:
        if os.path.exists(p):
            return p
    return shutil.which(name)


def end_browser(profile):
    """Ends every process of the browser run on this profile: a launcher's
    PID is not always the browser's."""
    if os.name == "nt":
        script = ("Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*%s*' } | "
                  "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"
                  % os.path.basename(profile))
        subprocess.run(["powershell", "-NoProfile", "-Command", script], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    else:
        subprocess.run(["pkill", "-f", profile], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(description="the first-run page, walked once in a headless browser")
    ap.add_argument("ui_dir", nargs="?", default=os.path.join(ROOT, "build", "ui"),
                    help="the bundled pages embed.cmake writes (default build/ui); unused with --daemon")
    ap.add_argument("--browser", default="chrome", choices=sorted(BROWSERS))
    ap.add_argument("--daemon", metavar="HOST:PORT",
                    help="walk the page a running daemon serves at its HTTPS listener, through the walk's own "
                         "relay, instead of the bundled pages against the mock")
    ap.add_argument("--port", type=int, default=8093)
    ap.add_argument("--timeout", type=float, default=0, help="seconds for the whole walk (default 150; 600 with --daemon)")
    a = ap.parse_args()
    timeout = a.timeout or (600 if a.daemon else 150)
    exe = find_browser(a.browser)
    if not exe:
        print("no %s found" % a.browser)
        return 2

    if a.daemon:
        host, _, port = a.daemon.rpartition(":")
        if not host or not port.isdigit():
            print("--daemon takes HOST:PORT, the daemon's HTTPS listener")
            return 2
        # The harness counts the daemon's intake fans, so no check opens
        # between the forms and the choice. The diagnostic starts need
        # the mock's engine refusals, which a daemon does not take.
        Walk.plan = {"check": None, "logout": True, "diag": False, "plain_login": False}
        Walk.daemon = (host, int(port))
        Walk.cfg = types.SimpleNamespace(mock=False, token="",
                                         upstream=lambda: (host, int(port), "%s:%s" % (host, port)))
        Walk.mock = None
    else:
        try:
            Walk.pages = load_pages(a.ui_dir)
        except OSError as e:
            print("cannot read the bundled pages: %s" % e)
            return 2
        # The mock as a first run, nothing held down and no step asked for again.
        for k in ("GF_MOCK_BUTTON", "GF_MOCK_MIRROR", "GF_MOCK_REQUIRED_AGAIN"):
            os.environ.pop(k, None)
        os.environ["GF_MOCK_FIRST_RUN"] = "1"
        devserver.DARK_PROMPTS["airflow.intakes"] = CHECK_PROMPT
        Walk.plan = {"check": {"id": "airflow.intakes", "prompt": {"options": PROMPT_OPTIONS,
                                                                "hostile": PROMPT_HOSTILE},
                               "result_ms": 20000}, "logout": False, "diag": True, "plain_login": True,
                     "report": devserver.MOCK_REPORT_SECRET}
        Walk.mock = devserver.Mock(devserver.MOCK_TOKEN)
        Walk.mock.call_port = 0
        Walk.cfg = types.SimpleNamespace(mock=True)
    Walk.panel = None
    Walk.bundled = True
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Walk)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    try:
        w = call(a.port, "GET", "/wiz")
    except OSError as e:
        print("GET /wiz did not answer: %s" % e)
        print("\n".join(Walk.said[-5:]))
        srv.shutdown()
        return 2
    # The daemon's harness counts its intake fans; the mock leaves them
    # for their check.
    if not (w.get("first_run") is True and w.get("fans_counted") is bool(a.daemon)
            and w.get("gate") == "closed" and not w.get("account")):
        print("the %s is not a first run with its intake fans %s: %s"
              % ("daemon" if a.daemon else "mock", "counted" if a.daemon else "not yet counted",
                 {k: w.get(k) for k in ("first_run", "fans_counted", "gate", "account")}))
        srv.shutdown()
        return 2

    prof = tempfile.mkdtemp(prefix="wizard-walk-")
    url = "http://127.0.0.1:%d/__walk/" % a.port
    if a.browser == "chrome":
        cmd = [exe, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
               "--window-size=1400,1100", "--user-data-dir=" + prof, url]
        # A local page against a local mock: a CI runner may refuse the
        # renderer sandbox its namespaces.
        if os.name != "nt":
            cmd.insert(1, "--no-sandbox")
    else:
        cmd = [exe, "-headless", "-no-remote", "-new-instance", "-profile", prof, url]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.monotonic() + timeout
    try:
        while Walk.result is None and time.monotonic() < end:
            time.sleep(0.5)
    finally:
        proc.kill()
        proc.wait()
        end_browser(prof)
        srv.shutdown()
        time.sleep(0.5)
        shutil.rmtree(prof, ignore_errors=True)
    if Walk.result is None:
        print("NORESULT: the walk did not finish in %.0f s" % timeout)
        print("\n".join(Walk.said[-40:]))
        return 2
    print("\n".join(Walk.result.get("log") or []))
    ok = bool(Walk.result.get("ok"))
    # The answer the page posted for the code-shaped option, as the mock took it.
    if Walk.mock:
        answer = (Walk.mock.dark or {}).get("answer")
        if ok and answer != PROMPT_HOSTILE:
            print("FAIL the intake prompt's answer reached the mock as %r, not the option as written" % (answer,))
            ok = False
    if not ok:
        print("--- the last requests")
        print("\n".join(Walk.said[-40:]))
    print("PASS: wizard_walk" if ok else "FAIL: wizard_walk")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
