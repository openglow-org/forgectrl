/*
 * wizard.js - forgectrl: the commissioning wizard
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The first run of the panel, and the same steps on demand later. The
 * state comes from GET /wiz (the record's view: which documents are
 * accepted, whether the press happened, whether an account exists,
 * which wizards completed at which version); the page shows the first
 * step that is not done and moves on as each completes. Every write
 * goes through the wizard routes with the token (TOK, spliced by the
 * daemon) and, once an account exists, the session cookie the account
 * step set.
 *
 * Steps: welcome, advisories (the four documents, then the press),
 * account, preferences, machine, cloud, the checks (the dark wizards),
 * the sheet (the live wizards: one press per card), done. The footer's
 * exit runs the factory return and shows its progress.
 */
var TOK = '__FFTOKEN__';
var W = null,
  IMPERIAL = false,
  DOCS = {},
  curDoc = null,
  pressTimer = null,
  returnTimer = null;
var DARK_IDS = ['switches', 'sensors', 'airflow', 'motion', 'cameras', 'cooling.aa-offset',
  'cooling.flow', 'cooling.tec', 'cooling.flow-verify', 'cloud.header'];
/* The sheet: the live wizards, one press per card, run on the same
 * check section with a preview of the burn. */
var LIVE_IDS = ['sheet.place', 'sheet.frame', 'laser.focus', 'laser.floor', 'laser.dose-curve',
  'laser.corner', 'cooling.flow-load'];
var CHECK_IDS = DARK_IDS.concat(LIVE_IDS);
/* The checks a machine may not need: the TEC without one, the cloud
 * header with cloud mode off. They show only while required or done. */
var CONDITIONAL = ['cooling.tec', 'cloud.header'];
var ORDER = ['welcome', 'advisories', 'account', 'preferences', 'machine', 'cloud',
  'switches', 'sensors', 'airflow', 'motion', 'cameras', 'cooling.aa-offset', 'cooling.flow',
  'cooling.tec', 'cloud.header', 'sheet.place', 'sheet.frame', 'laser.focus', 'laser.floor',
  'laser.dose-curve', 'laser.corner', 'cooling.flow-load', 'done'];
var TITLES = {
  welcome: 'Welcome',
  advisories: 'Advisories',
  account: 'Your account',
  preferences: 'Preferences',
  machine: 'Your machine',
  cloud: 'Cloud mode',
  switches: 'Switches',
  sensors: 'Sensors',
  airflow: 'Airflow',
  motion: 'Motion',
  cameras: 'Cameras',
  'cooling.aa-offset': 'Coolant offset',
  'cooling.flow': 'Coolant flow',
  'cooling.tec': 'TEC',
  'cooling.flow-verify': 'Flow check',
  'cloud.header': 'Cloud header',
  'sheet.place': 'Place the sheet',
  'sheet.frame': 'First fire',
  'laser.focus': 'Focus',
  'laser.floor': 'Laser floor',
  'laser.dose-curve': 'Dose curve',
  'laser.corner': 'Corner rolloff',
  'cooling.flow-load': 'Flow under load',
  done: 'Ready'
};
/* What each dark check does, in a sentence or two. The machine stays
 * dark: the laser latch is locked throughout. */
var DARK_TEXT = {
  switches: 'Open and close the lid, press the button, and on a Pro open and close the ' +
    'interlock loop. The machine watches each switch change. Nothing moves and the laser ' +
    'stays locked. About three minutes.',
  sensors: 'Ten seconds of readings at rest: both coolant temperatures, the chassis and the ' +
    'processor, the lid infrared sensors, the accelerometer, the laser supply, and the fans ' +
    'at idle. If you have a room thermometer, one reading gives the coolant sensors a ' +
    'per-machine offset. About one minute.',
  airflow: 'The fans run at the cut profile for 35 seconds. Each fan speed and its spin-up ' +
    'time set the floor the airflow gate watches. The purge fan current is measured on and ' +
    'off. The motion controller stops for the run. About one minute.',
  motion: 'The rail comes up and the liveness probe runs, the lens finds its reference on the ' +
    'hall sensor, then the head jogs 50 mm each way on X and on Y with the accelerometer as ' +
    'the witness and the crash watch armed. Keep the bed clear. About three minutes.',
  cameras: 'With the lid closed, one snapshot from the lid camera and one from the head ' +
    'camera. You confirm each view. About one minute.',
  'cooling.aa-offset': 'The loop settles, then the air-assist fan is switched while the coolant ' +
    'sensors are read: the ground shift it causes becomes the offset. About six minutes; ' +
    'the pump and the heater run, the motion controller stops.',
  'cooling.flow': 'Three heater windows with the pump on and three with it off give the flow ' +
    'bands; the threshold sits between them. If the bands are too close, the heater duty ' +
    'steps up and the trial repeats. Fifteen to twenty-five minutes.',
  'cooling.tec': 'Sixty seconds of TEC drive with the fans on. The cold side must fall by one ' +
    'degree relative to the coolant. About two minutes.',
  'cooling.flow-verify': 'One heater window with the pump on and one with it off: the ' +
    'threshold must separate them. About three minutes.',
  'cloud.header': 'The machine starts in cloud mode. You place any small design in the ' +
    'Glowforge app and press Print there. The wizard takes the numbers the service sends ' +
    'for this machine, cancels the print before it arms, and returns to GRBL mode. Nothing ' +
    'is written to the settings. About five minutes.',
  'sheet.place': 'The datum for the sheet. The lens finds its reference, then the arrows ' +
    'move the head to the back-left where the machine normally homes. Open the lid, push ' +
    'the sheet as far left as it goes with its top edge on the top of the cut area, close ' +
    'the lid, and set the origin. Wood, 200 x 150 mm (8 x 6 in) or larger, any thickness ' +
    'you can measure. Nothing fires. About three minutes.',
  'sheet.frame': 'The first emission. Eye protection on, exhaust hose connected and vented, ' +
    'extinguisher in reach, the area clear. The frame (180 x 130 mm) and the header burn ' +
    'at the mark dose after your press; the tube current, the head thermopile, and the ' +
    'kernel must all see the beam. You confirm the frame sits on the sheet, square, and ' +
    'visible, or ask for it lighter or darker. About three minutes.',
  'laser.focus': 'The lens finds its reference on the edge of its hall sensor, then its two ' +
    'stops by the head accelerometer, one half-step at a time and without a slip (when they ' +
    'cannot be found, a reduced travel is kept and the page says so). Then twelve lines ' +
    'burn heavy, the lens stepped from the top of its travel to the bottom, numbered 1 to ' +
    '12. Every line marks; you pick the narrowest dark one by its number (every adjacent ' +
    'line that looks the same, and the middle of the run is taken) and confirm the sheet ' +
    'thickness; the focus height with the lens on its reference and the stops are ' +
    'written. One press. About four minutes.',
  'laser.floor': 'Twelve rungs across the card from 2 to 24 percent pulse density with the ' +
    'floor and the dose curve off for this one job. You pick the faintest rung that shows a ' +
    'continuous line; the floor is written two percent above it. One press. About three ' +
    'minutes.',
  'laser.dose-curve': 'Seven 100 mm rungs from 10 to 100 percent density with the curve off. ' +
    'The head thermopile reads each rung and the curve fits itself; a monotonic fit is ' +
    'written. One press. About three minutes.',
  'laser.corner': 'The same corner-heavy pattern five times at rolloff exponents 1.0 to 2.0 ' +
    'under velocity-scaled power at 30 percent. You pick the most even one by eye. One ' +
    'press. About four minutes.',
  'cooling.flow-load': 'The coolant loop settles first, the page counting. Then one press: ' +
    'the box and a 60 x 15 mm patch at 60 percent keep the tube lit for about a minute ' +
    'while both coolant sensors are read. The tube\'s heat coefficient is written. ' +
    'About six minutes.'
};

function $(id) {
  return document.getElementById(id);
}
function esc(s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}
function fx(u, o) {
  o = o || {};
  o.headers = o.headers || {};
  o.headers['X-ForgeFIRM-Token'] = TOK;
  return fetch(u, o);
}
function post(u, params) {
  var body = new URLSearchParams();
  Object.keys(params || {}).forEach(function (k) {
    if (params[k] !== undefined && params[k] !== null) body.set(k, params[k]);
  });
  return fx(u, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: body.toString()
  }).then(function (r) {
    return r
      .json()
      .catch(function () {
        return {};
      })
      .then(function (j) {
        if (r.status === 403 && j && /login/.test(j.error || '')) {
          location.replace(loginUrl());
          throw 'login required';
        }
        if (!r.ok) throw (j && j.error) || 'request failed (' + r.status + ')';
        return j;
      });
  });
}
function toast(title, body, ok) {
  var el = document.createElement('div');
  el.className = 'toast ' + (ok ? 't-ok' : 't-bad');
  el.setAttribute('role', 'status');
  el.innerHTML = "<div class='toast-body'><b>" + esc(title) + '</b>' + (body ? '<br>' + esc(body) : '') + '</div>';
  $('toasts').appendChild(el);
  el.addEventListener('hidden.bs.toast', function () {
    el.remove();
  });
  new bootstrap.Toast(el, { delay: ok ? 3500 : 8000 }).show();
}
function setMsg(id, text, cls) {
  var el = $(id);
  el.textContent = text || '';
  el.className = 'msg' + (cls ? ' ' + cls : '');
}

/* ---- Theme (the same resolution as the panel) ------------------------ */
var THEMES = ['auto', 'light', 'dark'];
function themeChoice() {
  try {
    var s = localStorage.getItem('ff_theme');
    return s === 'light' || s === 'dark' ? s : 'auto';
  } catch (e) {
    return 'auto';
  }
}
function applyTheme() {
  var c = themeChoice(),
    t = c;
  if (c === 'auto')
    t = window.matchMedia && matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  document.documentElement.setAttribute('data-bs-theme', t);
  $('themebtn').textContent = c === 'auto' ? '◐ Auto' : c === 'dark' ? '☾ Dark' : '☀ Light';
}
function cycleTheme() {
  var next = THEMES[(THEMES.indexOf(themeChoice()) + 1) % THEMES.length];
  try {
    if (next === 'auto') localStorage.removeItem('ff_theme');
    else localStorage.setItem('ff_theme', next);
  } catch (e) {}
  applyTheme();
}
applyTheme();

/* ---- State and navigation ------------------------------------------- */
/* A dark check that this machine does not need (the TEC on a machine
 * without one) is neither required nor listed. */
function darkApplicable(id) {
  if (CONDITIONAL.indexOf(id) < 0 || !W) return true;
  return (W.required || []).indexOf(id) >= 0 || !!(W.versions && W.versions[id]);
}
function stepDone(id) {
  if (!W) return false;
  if (id === 'welcome') return true;
  /* Done only while every document is accepted at its current hash AND
   * the press stands: a document changed by an update leaves the press
   * recorded but the step open, and the page must show it. */
  if (id === 'advisories') return !!(W.advisories_complete && W.acceptance_done && W.versions && W.versions.advisories);
  if (id === 'account') return !!(W.users && W.users.exists && !W.users.reset_pending);
  if (id === 'done') return !!W.completed;
  if (CHECK_IDS.indexOf(id) >= 0 && !darkApplicable(id)) return true;
  if (W.versions && W.versions[id] && (W.required || []).indexOf(id) >= 0) return false;
  return !!(W.versions && W.versions[id]);
}
function firstOpen() {
  for (var i = 0; i < ORDER.length; i++) if (!stepDone(ORDER[i])) return ORDER[i];
  return 'done';
}
function railResult(id) {
  if (!W) return '';
  if (id === 'account' && W.users && W.users.exists) return W.users.name;
  if (id === 'machine' && W.machine && W.machine.model_label) return W.machine.model_label;
  return '';
}
/* After the first run, the form steps and the checks run again on demand
 * from the rail. */
var RERUN = ['preferences', 'machine', 'cloud'].concat(CHECK_IDS);
function renderRail(active) {
  var h = '';
  var done = !!(W && W.completed);
  ORDER.forEach(function (id) {
    if (CHECK_IDS.indexOf(id) >= 0 && !darkApplicable(id)) return;
    var cls = stepDone(id) ? 'done' : '';
    if (id === active || (active === 'press' && id === 'advisories')) cls += ' on';
    /* On a completed setup the rail is the navigation: a re-runnable
     * step, and any step the record has opened again (the advisories
     * after a document changed, the account after a reset), is a link. */
    var again = done && (RERUN.indexOf(id) >= 0 || !stepDone(id));
    if (again) cls += ' link';
    h += '<li class="' + cls + '"' + (again ? ' onclick="go(\'' + id + '\')"' : '') + '>' + esc(TITLES[id]);
    var r = railResult(id);
    if (r) h += '<span class="wz-result">' + esc(r) + '</span>';
    h += '</li>';
  });
  $('rail').innerHTML = h;
  $('estimate').textContent = done ? 'Choose a step to run it again.' : 'About ninety minutes.';
}
function show(id) {
  var steps = document.querySelectorAll('.wz-step');
  for (var i = 0; i < steps.length; i++) steps[i].className = 'wz-step';
  var dark = CHECK_IDS.indexOf(id) >= 0;
  $(dark ? 'st-dark' : 'st-' + id).className = 'wz-step on';
  renderRail(id);
  window.scrollTo(0, 0);
  if (dark) initDark(id);
  if (id === 'advisories') initDocs();
  if (id === 'press') startPress();
  if (id === 'preferences') initPreferences();
  if (id === 'machine') initMachine();
  if (id === 'cloud') initCloud();
  if (id === 'done') initDone();
  $('exitlink').style.display = W && W.completed ? 'none' : '';
  /* The address follows the step, so a reload comes back to it (and to
   * nothing else): ?step= names a re-runnable step of a completed setup. */
  var rerun = W && W.completed && RERUN.indexOf(id) >= 0;
  var want = location.pathname + (rerun ? '?step=' + encodeURIComponent(id) : '');
  if (history.replaceState && location.pathname + location.search !== want)
    history.replaceState(null, '', want);
}
/* The user's units: lengths in a check's result show in them. */
function loadUnits() {
  fetch('/settings')
    .then(function (r) { return r.json(); })
    .then(function (s) { IMPERIAL = s.ui_units === 'imperial'; })
    .catch(function () {});
}
/* The login, told where to come back to. */
function loginUrl() {
  return '/login?next=' + encodeURIComponent(location.pathname + location.search + location.hash);
}
function go(id) {
  if (id === 'advisories' && stepDone('advisories')) id = firstOpen();
  show(id);
}
function load(then) {
  return fetch('/wiz')
    .then(function (r) {
      if (r.status === 403) {
        location.replace(loginUrl());
        throw 'login';
      }
      return r.json();
    })
    .then(function (w) {
      W = w;
      $('host').textContent = (w.machine && w.machine.firmware) || '';
      loadUnits();
      if (then) then();
    })
    .catch(function () {
      if (!W) $('rail').innerHTML = '<li class="on">Waiting for the machine…</li>';
    });
}

/* ---- Welcome ---------------------------------------------------------- */
function initWelcome() {
  fetch('/settings')
    .then(function (r) {
      return r.json();
    })
    .then(function (s) {
      $('w-host').textContent = s.machine_id || '?';
      $('host').textContent = s.machine_id || '';
      $('w-fw').textContent = s.version || '?';
    })
    .catch(function () {});
}

/* ---- Advisories ------------------------------------------------------- */
function docState(id) {
  var d = null;
  (W.documents || []).forEach(function (x) {
    if (x.id === id) d = x;
  });
  return d;
}
function initDocs() {
  var tabs = '';
  (W.documents || []).forEach(function (d, i) {
    tabs += '<button type="button" id="dt-' + d.id + '" class="' + (d.accepted ? 'done' : '') + '" onclick="openDoc(\'' + d.id + '\')">' + esc(d.title) + '</button>';
  });
  $('doctabs').innerHTML = tabs;
  var first = null;
  (W.documents || []).forEach(function (d) {
    if (!first && !d.accepted) first = d.id;
  });
  openDoc(first || (W.documents && W.documents[0] && W.documents[0].id));
  refreshDocsNext();
}
function refreshDocsNext() {
  var all = (W.documents || []).every(function (d) {
    return d.accepted;
  });
  $('docs-next').disabled = !all;
  (W.documents || []).forEach(function (d) {
    var t = $('dt-' + d.id);
    if (t) t.className = (d.accepted ? 'done' : '') + (curDoc === d.id ? ' on' : '');
  });
}
function openDoc(id) {
  curDoc = id;
  var view = $('docview');
  view.innerHTML = '<p>Loading…</p>';
  refreshDocsNext();
  function render(text, hash) {
    DOCS[id] = { text: text, hash: hash, read: false };
    view.innerHTML = mdRender(text);
    view.scrollTop = 0;
    renderConsent(id);
    checkRead();
  }
  if (DOCS[id]) {
    render(DOCS[id].text, DOCS[id].hash);
    return;
  }
  fetch('/advisories/' + id)
    .then(function (r) {
      var hash = (r.headers.get('ETag') || '').replace(/"/g, '');
      return r.text().then(function (t) {
        render(t, hash);
      });
    })
    .catch(function () {
      view.innerHTML = '<p>The document could not be loaded.</p>';
    });
}
function checkRead() {
  var v = $('docview'),
    d = DOCS[curDoc];
  if (!d) return;
  if (v.scrollTop + v.clientHeight >= v.scrollHeight - 12) {
    if (!d.read) {
      d.read = true;
      renderConsent(curDoc);
    }
  }
}
$('docview').addEventListener('scroll', checkRead);
function renderConsent(id) {
  var d = docState(id),
    c = $('consent'),
    st = DOCS[id] || {};
  if (!d) return;
  if (d.accepted) {
    c.className = 'wz-consent done';
    c.innerHTML = '✓ Confirmed.';
    return;
  }
  c.className = 'wz-consent';
  if (!st.read) {
    c.innerHTML = 'Read to the end of the document to confirm it.';
    return;
  }
  if (d.consent === 'typed') {
    c.innerHTML =
      'Type <b>' + esc(d.phrase) + '</b> to confirm you read and accept this document: ' +
      '<input type="text" class="form-control form-control-sm" id="phrase" autocapitalize="characters" autocomplete="off" spellcheck="false" /> ' +
      '<button type="button" class="btn btn-sm btn-primary" id="phrase-go" disabled onclick="acceptDoc(\'' + id + '\')">Confirm</button>';
    $('phrase').addEventListener('input', function () {
      $('phrase-go').disabled = $('phrase').value !== d.phrase;
    });
    $('phrase').focus();
  } else {
    c.innerHTML =
      '<label class="wz-check"><input type="checkbox" id="chk" onchange="if(this.checked)acceptDoc(\'' + id + '\')" /> ' +
      '<span>I have read this document.</span></label>';
  }
}
function acceptDoc(id) {
  var d = docState(id),
    st = DOCS[id];
  var params = { doc: id, hash: st.hash };
  if (d.consent === 'typed') params.phrase = $('phrase').value;
  post('/wiz/advisories/accept', params)
    .then(function (w) {
      W = w;
      renderConsent(id);
      refreshDocsNext();
      var next = null;
      (W.documents || []).forEach(function (x) {
        if (!next && !x.accepted) next = x.id;
      });
      if (next) openDoc(next);
      renderRail('advisories');
    })
    .catch(function (e) {
      setMsg('docs-msg', String(e), 'bad');
    });
}
function showPress() {
  show('press');
}
function startPress() {
  $('pressled').className = 'wz-button';
  $('pressmsg').textContent = 'Waiting for your press…';
  $('press-retry').style.display = 'none';
  post('/wiz/advisories/press', {})
    .then(function () {
      pollPress();
    })
    .catch(function (e) {
      $('pressmsg').textContent = String(e);
    });
}
function pollPress() {
  if (pressTimer) clearTimeout(pressTimer);
  fetch('/wiz/advisories/press')
    .then(function (r) {
      return r.json();
    })
    .then(function (j) {
      if (j.accepted) {
        $('pressled').className = 'wz-button pressed';
        $('pressmsg').textContent = 'Thank you. Accepted.';
        setTimeout(function () {
          load(function () {
            show(firstOpen());
          });
        }, 1200);
        return;
      }
      if (j.button === 'timeout' || j.button === 'cancelled') {
        $('pressled').className = 'wz-button off';
        $('pressmsg').textContent =
          j.button === 'timeout' ? 'No press within ten minutes.' : 'The wait was cancelled.';
        $('press-retry').style.display = '';
        return;
      }
      pressTimer = setTimeout(pollPress, 800);
    })
    .catch(function () {
      pressTimer = setTimeout(pollPress, 2000);
    });
}
function cancelPress() {
  if (pressTimer) clearTimeout(pressTimer);
  post('/wiz/advisories/press/cancel', {}).catch(function () {});
  show('advisories');
}

/* ---- Account ---------------------------------------------------------- */
$('acct').addEventListener('submit', function (e) {
  e.preventDefault();
  var name = $('a-name').value.trim(),
    pw = $('a-pw').value,
    pw2 = $('a-pw2').value;
  if (pw !== pw2) {
    setMsg('a-msg', 'the two passwords differ', 'bad');
    return;
  }
  $('a-go').disabled = true;
  setMsg('a-msg', 'creating…');
  post('/wiz/account', { name: name, password: pw })
    .then(function () {
      setMsg('a-msg', 'done', 'ok');
      load(function () {
        show(firstOpen());
      });
    })
    .catch(function (e) {
      setMsg('a-msg', String(e), 'bad');
    })
    .then(function () {
      $('a-go').disabled = false;
    });
});

/* ---- Preferences ------------------------------------------------------ */
var prefsInit = false;
function initPreferences() {
  if (!prefsInit) {
    fillRegions($('p-region'));
    prefsInit = true;
  }
  fetch('/settings')
    .then(function (r) {
      return r.json();
    })
    .then(function (s) {
      $('p-units').value = s.ui_units || 'metric';
      $('p-region').value = s.wifi_country || '00';
    })
    .catch(function () {});
  renderClock();
}
function renderClock() {
  var c = W && W.clock;
  if (!c) return;
  if (c.set) {
    $('p-clock').textContent = 'The machine clock reads ' + c.utc + ' (UTC), from the network.';
    $('p-clockbtn').style.display = 'none';
  } else {
    $('p-clock').textContent =
      'The machine clock is not set: it reads ' + c.utc + '. The machine sets it from the network when it can. You can set it from this device now.';
    $('p-clockbtn').style.display = '';
  }
}
function setClock() {
  var now = Math.floor(Date.now() / 1000);
  post('/wiz/preferences', {
    ui_units: $('p-units').value,
    wifi_country: $('p-region').value,
    clock: String(now)
  })
    .then(function (w) {
      W = w;
      renderClock();
      toast('Clock set', W.clock.utc, true);
    })
    .catch(function (e) {
      setMsg('p-msg', String(e), 'bad');
    });
}
function checkUpdate() {
  $('p-updbtn').disabled = true;
  $('p-upd').textContent = 'Checking…';
  fx('/update/check', { method: 'POST' })
    .then(function (r) {
      return r.json();
    })
    .then(function (j) {
      if (j.error) {
        $('p-upd').textContent = 'The check did not complete: ' + j.error;
        return;
      }
      if (j.available && j.version && j.version !== j.current)
        $('p-upd').textContent =
          'A newer release exists: ' + j.version + ' (this machine runs ' + (j.current || '?') + '). You can install it from the panel’s System tab after setup; the setup state survives an update.';
      else $('p-upd').textContent = 'This machine runs the latest release' + (j.current ? ' (' + j.current + ')' : '') + '.';
    })
    .catch(function () {
      $('p-upd').textContent = 'The check did not complete.';
    })
    .then(function () {
      $('p-updbtn').disabled = false;
    });
}
function savePreferences() {
  setMsg('p-msg', 'saving…');
  post('/wiz/preferences', { ui_units: $('p-units').value, wifi_country: $('p-region').value })
    .then(function (w) {
      W = w;
      IMPERIAL = $('p-units').value === 'imperial';
      show(firstOpen());
    })
    .catch(function (e) {
      setMsg('p-msg', String(e), 'bad');
    });
}

/* ---- Machine ---------------------------------------------------------- */
function initMachine() {
  var m = (W && W.machine) || {};
  var head = m.head || {};
  var g = '';
  g += '<span>Firmware</span><span>' + esc(m.firmware || '?') + (m.build === 'dev' ? ' (development build)' : '') + '</span>';
  g += '<span>Camera</span><span>' + esc(m.camera || 'unknown') + '</span>';
  g += '<span>Head</span><span>' + (head.present ? 'present, hardware ' + esc(head.hw_id) + ', firmware ' + esc(head.version) : 'not detected') + '</span>';
  $('m-facts').innerHTML = g;
  $('m-sheet').textContent = W.sheet_id || 'not available';
  var radios = document.querySelectorAll('#m-models input');
  for (var i = 0; i < radios.length; i++)
    radios[i].onchange = function () {
      var labels = document.querySelectorAll('#m-models .wz-choice');
      for (var k = 0; k < labels.length; k++) labels[k].className = 'wz-choice';
      this.parentNode.className = 'wz-choice on';
      $('m-tecrow').style.display = this.value === 'pro' ? '' : 'none';
      if (this.value === 'pro') $('m-tec').checked = true;
    };
}
function saveMachine() {
  var sel = document.querySelector('#m-models input:checked');
  if (!sel) {
    setMsg('m-msg', 'choose the model', 'bad');
    return;
  }
  setMsg('m-msg', 'saving…');
  post('/wiz/machine', { model: sel.value, tec: sel.value === 'pro' && $('m-tec').checked ? '1' : '0' })
    .then(function (w) {
      W = w;
      show(firstOpen());
    })
    .catch(function (e) {
      setMsg('m-msg', String(e), 'bad');
    });
}

/* ---- Cloud ------------------------------------------------------------ */
function showCloudChoice(on) {
  $('c-off').className = 'wz-choice' + (on ? '' : ' on');
  $('c-on').className = 'wz-choice' + (on ? ' on' : '');
  $('c-onbox').style.display = on ? '' : 'none';
}
function initCloud() {
  var radios = document.querySelectorAll('#st-cloud input[name=cloud]');
  for (var i = 0; i < radios.length; i++)
    radios[i].onchange = function () {
      showCloudChoice(this.value === '1');
    };
  /* The current choice, so a re-run starts from what is set. */
  fetch('/settings')
    .then(function (r) { return r.json(); })
    .then(function (s) {
      var on = s.cloud_enabled === '1';
      for (var i = 0; i < radios.length; i++) radios[i].checked = radios[i].value === (on ? '1' : '0');
      showCloudChoice(on);
      if (on && (s.homing_mode === 'gfcloud' || s.homing_mode === 'none')) $('c-homing').value = s.homing_mode;
    })
    .catch(function () {});
}
function saveCloud() {
  var on = document.querySelector('#st-cloud input[name=cloud]:checked').value === '1';
  var params = { enabled: on ? '1' : '0' };
  if (on) {
    params.phrase = $('c-phrase').value.trim();
    params.homing_mode = $('c-homing').value;
    if ($('c-serial').value.trim()) params.gf_serial = $('c-serial').value.trim();
    if ($('c-pw').value) params.gf_password = $('c-pw').value;
  }
  setMsg('c-msg', 'saving…');
  post('/wiz/cloud', params)
    .then(function (w) {
      W = w;
      show(firstOpen());
    })
    .catch(function (e) {
      setMsg('c-msg', String(e), 'bad');
    });
}

/* ---- Done ------------------------------------------------------------- */
function initDone() {
  fetch('/settings')
    .then(function (r) {
      return r.json();
    })
    .then(function (s) {
      var g = '';
      g += '<span>Account</span><span>' + esc((W.users && W.users.name) || '?') + '</span>';
      g += '<span>Cloud mode</span><span>' + (s.cloud_enabled === '1' ? 'on' : 'off') + '</span>';
      g += '<span>Sheet id</span><span class="mono">' + esc(W.sheet_id || '') + '</span>';
      $('d-facts').innerHTML = g;
    })
    .catch(function () {});
  if (W.completed) {
    $('d-go').textContent = 'Open the control panel';
  }
}
function finish() {
  if (W.completed) {
    location.replace('/');
    return;
  }
  setMsg('d-msg', 'finishing…');
  post('/wiz/complete', {})
    .then(function () {
      location.replace('/');
    })
    .catch(function (e) {
      setMsg('d-msg', String(e), 'bad');
    });
}

/* ---- The exit: back to the factory firmware --------------------------- */
function askReturn() {
  $('ret-msg').textContent = '';
  $('ret-go').disabled = false;
  new bootstrap.Modal($('retmodal')).show();
}
function doReturn() {
  $('ret-go').disabled = true;
  $('ret-msg').textContent = 'starting…';
  post('/restore/factory-return?confirm=1', {})
    .then(function () {
      bootstrap.Modal.getInstance($('retmodal')).hide();
      show('return');
      $('exitlink').style.display = 'none';
      pollReturn();
    })
    .catch(function (e) {
      $('ret-msg').textContent = String(e);
      $('ret-go').disabled = false;
    });
}
function pollReturn() {
  if (returnTimer) clearTimeout(returnTimer);
  fetch('/update/status')
    .then(function (r) {
      return r.json();
    })
    .then(function (j) {
      if (j.running) {
        $('r-phase').textContent = j.phase || 'working…';
        returnTimer = setTimeout(pollReturn, 1500);
        return;
      }
      var res = j.result || {};
      if (res.ok) {
        $('r-lead').textContent = 'The machine is restarting as a Glowforge. This page will stop responding.';
        $('r-phase').textContent = res.restored
          ? 'The factory firmware ' + (res.factory_version || '') + ' was restored and selected for the next boot.'
          : 'The factory firmware was still on the machine and is selected for the next boot.';
      } else {
        $('r-lead').textContent = 'The return did not complete.';
        $('r-phase').textContent = (res.error || 'unknown error') + (res.detail ? ': ' + res.detail : '');
        $('exitlink').style.display = '';
      }
    })
    .catch(function () {
      $('r-lead').textContent = 'The machine is restarting as a Glowforge. This page will stop responding.';
    });
}

/* ---- Boot ------------------------------------------------------------- */
/* ---- The dark checks --------------------------------------------------
 * One section serves every check: the page starts the wizard, polls
 * GET /wiz/dark once a second, shows the phase with the time so far,
 * the log, and the open prompt (with how long it waits for an answer),
 * answers the prompt with what the operator chose, and at the end
 * shows the result in plain words with the facts and the settings
 * written under a fold. */
var K = { id: null, timer: null, running: false, promptSeq: 0, mirror: false };
/* Seconds as m:ss. */
function mmss(s) {
  s = Math.max(0, Math.floor(s || 0));
  var m = Math.floor(s / 60);
  return m + ':' + (s % 60 < 10 ? '0' : '') + (s % 60);
}
function initDark(id) {
  K.id = id;
  if (K.timer) clearTimeout(K.timer);
  K.timer = null;
  $('k-title').textContent = TITLES[id] || id;
  $('k-lead').textContent = DARK_TEXT[id] || '';
  /* A live wizard shows what it burns: the preview from the record's
   * facts, the same drawing the daemon streams. The placement shows the
   * whole sheet with every card; it burns nothing, so no program. */
  var live = LIVE_IDS.indexOf(id) >= 0, burns = live && id !== 'sheet.place';
  $('k-preview').style.display = live ? '' : 'none';
  if (live) $('k-preview').src = '/wiz/sheet.svg?card=' + encodeURIComponent(id) + '&t=' + Date.now();
  $('k-gcode').style.display = burns ? '' : 'none';
  if (burns) $('k-gcode').href = '/wiz/sheet.gcode?card=' + encodeURIComponent(id);
  $('k-fill').style.width = '0';
  $('k-phase').textContent = 'Ready to start.';
  $('k-phase').className = 'wz-phase';
  $('k-elapsed').textContent = '';
  $('k-log').textContent = '';
  $('k-prompt').style.display = 'none';
  $('k-result').style.display = 'none';
  $('k-result').innerHTML = '';
  $('k-start').style.display = '';
  $('k-start').textContent = stepDone(id) ? 'Run again' : 'Start';
  $('k-abort').style.display = 'none';
  $('k-next').style.display = stepDone(id) ? '' : 'none';
  setMsg('k-msg', '');
  /* A run of this check already in flight (a reload mid-check) resumes. */
  fetch('/wiz/dark')
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d && d.id === id && (d.running || d.result || d.error)) darkRender(d);
      if (d && d.running && d.id === id) darkPoll();
    })
    .catch(function () {});
}
function darkStart() {
  setMsg('k-msg', 'starting…');
  $('k-result').style.display = 'none';
  $('k-log').textContent = '';
  post('/wiz/' + K.id + '/start', {})
    .then(function () {
      setMsg('k-msg', '');
      darkPoll();
    })
    .catch(function (e) {
      setMsg('k-msg', String(e), 'bad');
    });
}
function darkAbort() {
  post('/wiz/' + K.id + '/abort', {}).catch(function (e) { setMsg('k-msg', String(e), 'bad'); });
}
/* A second browser follows a running step and can take it over: its
 * login becomes the run's owner, and the first browser follows. */
function darkTakeOver() {
  post('/wiz/' + K.id + '/takeover', {})
    .then(function () { darkPoll(); })
    .catch(function (e) { setMsg('k-msg', String(e), 'bad'); });
}
function darkNext() {
  if (K.timer) clearTimeout(K.timer);
  K.timer = null;
  load(function () {
    go(firstOpen());
  });
}
function darkPoll() {
  if (K.timer) clearTimeout(K.timer);
  K.timer = null;
  fetch('/wiz/dark')
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (!d || d.id !== K.id) return;
      darkRender(d);
      if (d.running) K.timer = setTimeout(darkPoll, 1000);
    })
    .catch(function () {
      K.timer = setTimeout(darkPoll, 2000);
    });
}
function darkAnswer(seq, value) {
  var jog = /^[XY][-+]\d+$/.test(value);
  if (!jog) $('k-pbtns').innerHTML = '';
  post('/wiz/' + K.id + '/answer', { seq: seq, value: value })
    .then(function () {
      if (jog) setTimeout(jogRefresh, 1500);
      darkPoll();
    })
    .catch(function (e) { setMsg('k-msg', String(e), 'bad'); });
}
/* A key that names a length in mm (not a per-mm ratio). */
function isLenKey(k) {
  return /_mm$/.test(k || '') && !/per_mm$/.test(k);
}
function lenFact(mm) {
  return IMPERIAL ? String(parseFloat((mm / 25.4).toFixed(3))) + ' in' : String(parseFloat(mm.toFixed(2))) + ' mm';
}
function darkFact(v, key) {
  if (v === null || v === undefined) return '';
  var len = isLenKey(key);
  if (typeof v === 'object') {
    var parts = [];
    Object.keys(v).forEach(function (k) {
      var x = v[k];
      var t = typeof x === 'object' && x !== null ? JSON.stringify(x)
        : typeof x === 'number' && (len || isLenKey(k)) ? lenFact(x) : String(x);
      parts.push(k.replace(/_mm$/, '').replace(/_/g, ' ') + ' ' + t);
    });
    return parts.join('; ');
  }
  if (typeof v === 'boolean') return v ? 'yes' : 'no';
  if (typeof v === 'number' && len) return lenFact(v);
  return String(v);
}
function darkRender(d) {
  var running = !!d.running;
  /* Another browser's run: this page mirrors it, with nothing to press
   * but the takeover. The prompt is redrawn when the ownership changes. */
  var mirror = running && !!d.owned && !d.mine;
  if (K.mirror !== mirror) {
    K.mirror = mirror;
    K.promptSeq = 0;
  }
  $('k-mirror').style.display = mirror ? '' : 'none';
  K.running = running;
  $('k-fill').style.width = (d.progress || 0) + '%';
  $('k-log').textContent = (d.log || []).join('\n');
  $('k-start').style.display = running ? 'none' : '';
  $('k-abort').style.display = running && !mirror ? '' : 'none';
  if (running) $('k-next').style.display = 'none';
  var p = d.prompt;
  if (running && p) {
    /* The prompt is drawn once per prompt: a redraw on every poll would
     * reload the snapshot and move the buttons under the pointer. */
    if (K.promptSeq !== p.seq) {
      K.promptSeq = p.seq;
      $('k-prompt').style.display = '';
      $('k-ptext').textContent = p.text || '';
      var shot = p.id === 'lid-view' ? 'lid' : p.id === 'head-view' ? 'head' : null;
      var jog = p.kind === 'jog';
      $('k-shot').style.display = shot || jog ? '' : 'none';
      if (shot) $('k-shot').src = '/wiz/shot?cam=' + shot + '&t=' + Date.now();
      if (jog) $('k-shot').src = '/cam/snapshot?cam=lid&res=half&t=' + Date.now();
      var number = p.kind === 'number';
      $('k-pinput').style.display = number ? '' : 'none';
      if (number) $('k-pvalue').value = '';
      var b = '';
      if (p.kind === 'wait') {
        b = '<span class="msg">' + (p.id === 'press' ? 'Waiting for your press…' : 'Waiting for the machine…') + '</span>';
      } else if (mirror) {
        b = '<span class="msg">The other browser answers this.</span>';
      } else if (jog) {
        b = jogButtons(p);
      } else {
        var multi = p.kind === 'multichoice';
        (p.options || ['Continue']).forEach(function (o, i) {
          var val = number && i === 0 ? '__value__' : o;
          var primary = i === 0 && p.kind !== 'choice' && !multi;
          var click = multi ? 'darkToggle(this)' : 'darkChoose(' + p.seq + ', \'' + esc(val) + '\')';
          b += '<button class="btn btn-sm ' + (primary ? 'btn-primary' : 'btn-outline-secondary') +
            '" data-v="' + esc(val) + '" onclick="' + click + '">' + esc(o) + '</button>';
        });
        if (multi) b += '<button class="btn btn-sm btn-primary" onclick="darkChooseMulti(' + p.seq + ')">Done</button>';
      }
      $('k-pbtns').innerHTML = b;
    }
    /* A wait prompt is the machine's own: the phase counts it. An
     * answer prompt waits for the operator, and says how long. */
    if (p.kind === 'wait') $('k-phase').textContent = d.phase || 'Waiting for the machine.';
    else {
      var left = p.timeout_s ? p.timeout_s - (p.since_s || 0) : 0;
      $('k-phase').textContent = 'Waiting for you.' + (p.timeout_s ? ' This step waits ' + mmss(left) + ' more for your answer.' : '');
    }
  } else {
    K.promptSeq = 0;
    $('k-prompt').style.display = 'none';
    $('k-phase').textContent = running ? d.phase || 'working…' : d.error ? d.error : d.result ? 'Complete.' : '';
  }
  $('k-phase').className = 'wz-phase' + (!running && d.error ? ' bad' : '');
  $('k-elapsed').textContent = d.elapsed_s ? mmss(d.elapsed_s) + (running ? '' : ' in all') : '';
  if (!running) {
    if (d.error) {
      $('k-start').textContent = 'Try again';
      $('k-next').style.display = 'none';
      $('k-result').style.display = 'none';
    } else if (d.result) {
      $('k-result').innerHTML = darkResult(d.result, d.applied);
      $('k-result').style.display = '';
      $('k-start').textContent = 'Run again';
      $('k-next').style.display = '';
      if (LIVE_IDS.indexOf(K.id) >= 0)
        $('k-preview').src = '/wiz/sheet.svg?card=' + encodeURIComponent(K.id) + '&t=' + Date.now();
      load(function () { renderRail(K.id); });
    }
  }
}
/* The result: the wizard's own sentence first, when it wrote one; the
 * dose chart; the settings it wrote, each with the value before; and
 * the facts of the result under a fold, lengths in the user's units. */
function darkResult(result, applied) {
  var g = '';
  if (result.summary) g += '<p class="wz-summary">' + esc(result.summary) + '</p>';
  if (result.points) g += '<div class="wz-facts">' + doseChart(result.points) + '</div>';
  var keys = Object.keys(applied || {});
  if (keys.length) {
    g += '<div class="wz-written"><b>Written to the settings</b>';
    keys.forEach(function (k) {
      var a = applied[k] || {};
      g += '<div><span class="mono">' + esc(k) + '</span> = ' + esc(a.to) +
        (a.from !== undefined && a.from !== '' && a.from !== a.to ? ' <span class="wz-was">(was ' + esc(a.from) + ')</span>' : '') + '</div>';
    });
    g += '</div>';
  }
  var facts = '';
  Object.keys(result).forEach(function (k) {
    if (k === 'points' || k === 'summary') return;
    facts += '<span>' + esc(k.replace(/_mm$/, '').replace(/_/g, ' ')) + '</span><span>' + esc(darkFact(result[k], k)) + '</span>';
  });
  if (facts) {
    if (result.summary) g += '<details class="wz-details"><summary>The numbers</summary><div class="wz-facts">' + facts + '</div></details>';
    else g += '<div class="wz-facts">' + facts + '</div>';
  }
  return g;
}
/* The placement's jog pad: the arrows (10 mm, and 1 mm on the inner
 * ring), the lid view refreshed after each move, and Set origin. The
 * head moves only with the lid closed; the wizard says so in its log. */
function jogButtons(p) {
  var opts = p.options || [];
  function btn(label, val, cls) {
    if (opts.indexOf(val) < 0) return '';
    return '<button class="btn btn-sm ' + (cls || 'btn-outline-secondary') + '" onclick="darkChoose(' +
      p.seq + ', \'' + esc(val) + '\')">' + label + '</button>';
  }
  var g = '<div class="wz-jog">';
  g += '<span></span>' + btn('▲ 10', 'Y-10') + '<span></span>';
  g += btn('◀ 10', 'X-10') + btn('▲ 1', 'Y-1') + btn('▶ 10', 'X+10');
  g += btn('◀ 1', 'X-1') + btn('▼ 1', 'Y+1') + btn('▶ 1', 'X+1');
  g += '<span></span>' + btn('▼ 10', 'Y+10') + '<span></span>';
  g += '</div>';
  g += btn('Set origin', 'Set origin', 'btn-primary');
  g += '<button class="btn btn-sm btn-outline-secondary" onclick="jogRefresh()">Refresh the view</button>';
  return g;
}
function jogRefresh() {
  $('k-shot').src = '/cam/snapshot?cam=lid&res=half&t=' + Date.now();
}
/* The dose curve's fitted points as a small chart: light against
 * density, both in percent, the identity as a reference line. */
function doseChart(points) {
  if (!points || !points.length) return '';
  var w = 220, h = 140, m = 24;
  var s = '<svg class="wz-chart" viewBox="0 0 ' + w + ' ' + h + '" width="' + w + '" height="' + h + '">';
  s += '<rect x="' + m + '" y="8" width="' + (w - m - 8) + '" height="' + (h - m - 8) + '" fill="none" stroke="currentColor" opacity=".4"/>';
  s += '<line x1="' + m + '" y1="' + (h - m) + '" x2="' + (w - 8) + '" y2="8" stroke="currentColor" opacity=".25" stroke-dasharray="3 3"/>';
  var d = '';
  points.forEach(function (p, i) {
    var x = m + (p.density / 100) * (w - m - 8);
    var y = h - m - (p.light / 100) * (h - m - 8);
    d += (i ? ' L' : 'M') + x.toFixed(1) + ' ' + y.toFixed(1);
    s += '<circle cx="' + x.toFixed(1) + '" cy="' + y.toFixed(1) + '" r="2.5" fill="currentColor"/>';
  });
  s += '<path d="' + d + '" fill="none" stroke="currentColor" stroke-width="1.5"/>';
  s += '<text x="' + (w / 2) + '" y="' + (h - 6) + '" font-size="9" text-anchor="middle" fill="currentColor">density %</text>';
  s += '<text x="8" y="' + (h / 2) + '" font-size="9" text-anchor="middle" fill="currentColor" transform="rotate(-90 8 ' + (h / 2) + ')">light %</text>';
  s += '</svg>';
  return '<span>curve</span><span>' + s + '</span>';
}
/* A multichoice: the options toggle, Done posts the chosen ones joined
 * by commas. */
function darkToggle(el) {
  el.classList.toggle('active');
}
function darkChooseMulti(seq) {
  var on = [];
  var bs = $('k-pbtns').querySelectorAll('button.active');
  for (var i = 0; i < bs.length; i++) on.push(bs[i].getAttribute('data-v'));
  if (!on.length) {
    setMsg('k-msg', 'choose at least one', 'bad');
    return;
  }
  setMsg('k-msg', '');
  darkAnswer(seq, on.join(','));
}
function darkChoose(seq, val) {
  if (val === '__value__') {
    var v = $('k-pvalue').value.trim();
    if (!v) {
      setMsg('k-msg', 'enter a value, or skip', 'bad');
      return;
    }
    val = v;
  }
  setMsg('k-msg', '');
  darkAnswer(seq, val);
}

initWelcome();
load(function () {
  var first = firstOpen();
  /* /setup?step=<id> opens one step on a completed setup (the panel's
   * Commissioning tab links there). */
  var want = new URLSearchParams(location.search).get('step');
  if (want && W.completed && ((RERUN.indexOf(want) >= 0 && darkApplicable(want)) ||
      (ORDER.indexOf(want) >= 0 && !stepDone(want)))) {
    show(want);
    return;
  }
  show(first === 'advisories' && !W.acceptance_done && !stepDone('advisories') ? 'welcome' : first);
});
