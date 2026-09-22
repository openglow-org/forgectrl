/*
 * ext.js - forgectrl panel: the Extension packages card
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The operator's door to extension packages: whether extensions are on
 * (turned on over their advisory), what is installed and how each is
 * doing, the switch and the hold's kind and the removal of one package,
 * and the install of a new one with the consent its tier takes. Every
 * decision is forgectrl's and the extension host's: this card shows what
 * GET /ext/status says and sends what the operator chose.
 */
var EXT_CAPS = {
  'machine.read': "Read the machine's status, cooling status, mode, and position",
  events: "Follow the machine's event stream",
  'settings.own': 'Keep settings of its own',
  'camera.lid': 'Take pictures with the lid camera',
  'camera.head': 'Take pictures with the head camera',
  'motion.jog': 'Jog the head, laser off, inside the jog bounds',
  'motion.job': "Run a program as the machine's one sender",
  hold: 'Hold a job until it clears the hold',
  'job_time.run': 'Keep running while a job is armed',
  ui: 'Show a tab or cards of its own in this panel',
  'net.outbound': 'Connect to',
  'net.listen': 'Listen on port',
  storage: 'Keep data on the machine, in MiB up to'
};
var EXT_TIERS = {
  official: ['Official', 'b-ok', 'Signed with the OpenGlow extension key.'],
  community: ['Community', 'b-warn', 'Signed with a key you added to this machine, not by OpenGlow.'],
  unverified: ['Unverified', 'b-bad', 'Signed by nobody this machine trusts, or not signed at all.']
};
var EXT_DOC = 'extensions';             /* the advisory turning extensions on agrees to */
var extStaged = null,
  extAdvisory = null;

function extCap(c) {
  var i = c.indexOf(':'),
    k = i > 0 ? c.slice(0, i) : c,
    arg = i > 0 ? c.slice(i + 1) : '';
  return esc(EXT_CAPS[k] || k) + (arg ? " <span class='mono'>" + esc(arg) + '</span>' : '');
}
function extTier(t) {
  var d = EXT_TIERS[t] || [t, 'b-bad', ''];
  return "<span class='" + d[1] + "' title='" + esc(d[2]) + "'>" + esc(d[0]) + '</span>';
}
function extSay(id, text) {
  $(id).textContent = text || '';
}
/* One answer: the JSON when the request went well, else its words. */
function extAnswer(r) {
  return r.text().then(function (t) {
    var j = null;
    try {
      j = JSON.parse(t);
    } catch (e) {
      j = null;
    }
    if (!r.ok) throw (j && j.error) || t || 'no answer';
    return j;
  });
}
function extStateOf(j, p) {
  var svc = null,
    list = (j.host && j.host.services) || [],
    i;
  for (i = 0; i < list.length; i++) if (list[i].id === p.id) svc = list[i];
  if (!p.enabled) return 'turned off';
  if (p.quarantined) return "<span class='b-bad'>set aside</span>: it kept ending. Turn it off and on to try it again.";
  if (!j.enabled) return 'not running: extensions are off';
  if (!j.host || !j.host.running) return "<span class='b-bad'>not running</span>: the extension host is not running";
  if (!svc) return 'installed';
  if (svc.state === 'running')
    return (
      "<span class='b-ok'>running</span>" +
      (svc.frozen ? ', frozen while a job is armed' : '') +
      (svc.job_limited ? ', under job-time limits' : '')
    );
  return esc(svc.state) + (svc.reason ? ': ' + esc(svc.reason) : '');
}
/* An open frame goes when its package does. Disabling a package, or
 * removing it, is the operator's way out of everything it does, and a
 * frame left open would go on asking the bridge for as long as the panel
 * stayed on the page. */
function extFramesFollow(list) {
  if (!EXT_FRAMES.length) return;
  var id = EXT_FRAMES[0].id,
    i,
    still = false;
  for (i = 0; i < list.length; i++) if (list[i].id === id && list[i].enabled) still = true;
  if (still) return;
  var h = $('extframe');
  if (h) h.innerHTML = '';
  EXT_FRAMES = [];
}

function renderExt(j) {
  var g = '',
    list = j.packages || [],
    i,
    c;
  extFramesFollow(list);
  g += kv(
    'Extensions',
    (j.enabled ? "<span class='b-ok'>on</span>" : 'off') +
      (j.safe_mode ? " <span class='b-warn'>safe mode</span>: no package runs until the next reboot" : '') +
      (j.enabled && j.host && j.host.running && j.host.not_ready ? '<br>Waiting: ' + esc(j.host.not_ready) : '') +
      (j.host && !j.host.running ? "<br><span class='b-bad'>The extension host is not running.</span>" : '') +
      (j.error ? '<br>' + esc(j.error) : '')
  );
  for (i = 0; i < list.length; i++) {
    var p = list[i],
      m = p.package || {},
      caps = [];
    for (c = 0; c < (m.capabilities || []).length; c++)
      caps.push(
        extCap(m.capabilities[c]) + ((p.grants || []).indexOf(m.capabilities[c]) >= 0 ? " <span class='hint'>(granted by you)</span>" : '')
      );
    var hasUi = (m.capabilities || []).indexOf('ui') >= 0;
    g += kv(
      m.name || p.id,
      extTier(p.tier) +
        " <span class='mono'>" +
        esc(p.id) +
        ' ' +
        esc(p.version) +
        '</span>' +
        (hasUi && p.enabled
          ? " <button type='button' class='extui' data-id='" + esc(p.id) + "'>Open</button>"
          : '') +
        (m.author ? ', by ' + esc(m.author) : '') +
        '<br>' +
        extStateOf(j, p) +
        (m.description ? '<br>' + esc(m.description) : '') +
        (caps.length ? "<br>It may:<ul class='extcaps'><li>" + caps.join('</li><li>') + '</li></ul>' : '<br>') +
        (p.hold
          ? "Its hold, when it cannot speak for itself: <select class='form-select form-select-sm exthold' data-id='" +
            esc(p.id) +
            "' style='display: inline-block; width: auto'><option value='hold-advisory'" +
            (p.hold === 'advisory' ? ' selected' : '') +
            ">is dropped (advisory)</option><option value='hold-required'" +
            (p.hold === 'required' ? ' selected' : '') +
            '>stands (required)</option></select><br>'
          : '') +
        "<button class='btn btn-sm btn-outline-secondary extact' data-id='" +
        esc(p.id) +
        "' data-action='" +
        (p.enabled ? 'disable' : 'enable') +
        "'>" +
        (p.enabled ? 'Turn off' : 'Turn on') +
        "</button> <button class='btn btn-sm btn-outline-danger extact' data-id='" +
        esc(p.id) +
        "' data-action='remove'>Remove</button>"
    );
  }
  if (!list.length) g += "<p class='hint'>No package is installed.</p>";
  $('extpkgs').innerHTML = g;
  renderExtKeys(j.keys || []);
  $('extswitch').textContent = j.enabled ? 'Turn extensions off' : 'Turn extensions on…';
  $('extswitch').setAttribute('data-on', j.enabled ? '1' : '0');
}
/* The owner's keys: what makes a package community rather than unverified.
 * Adding one takes the machine's button held, as unsigned firmware does. */
function renderExtKeys(keys) {
  var g = '',
    i;
  for (i = 0; i < keys.length; i++)
    g += kv(
      keys[i].name,
      "<span class='mono brk'>" +
        esc(keys[i].key || '') +
        "</span> <button class='btn btn-sm btn-outline-danger extkeyrm' data-name='" +
        esc(keys[i].name) +
        "'>Remove</button>"
    );
  $('extkeys').innerHTML = g || "<p class='hint'>No key of yours is here: only a package signed by OpenGlow reads as official, and every other one as unverified.</p>";
}
function extKeyAdd() {
  extSay('msg-extkey', '\u2026');
  fx('/ext/key', {
    method: 'POST',
    body: new URLSearchParams({ name: $('extkeyname').value.trim(), key: $('extkeytext').value.trim() })
  })
    .then(extAnswer)
    .then(function (j) {
      extSay('msg-extkey', '');
      $('extkeyname').value = '';
      $('extkeytext').value = '';
      renderExt(j);
    })
    .catch(function (e) {
      extSay('msg-extkey', String(e));
    });
}
function extKeyRemove(name) {
  if (!window.confirm('Remove the key ' + name + '? A package signed with it then reads as unverified.')) return;
  extSay('msg-extkey', '\u2026');
  fx('/ext/key/remove', { method: 'POST', body: new URLSearchParams({ name: name }) })
    .then(extAnswer)
    .then(function (j) {
      extSay('msg-extkey', '');
      renderExt(j);
    })
    .catch(function (e) {
      extSay('msg-extkey', String(e));
    });
}
/* A package's interface renders in a frame that can reach nothing: no
 * same-origin, so it holds no session and no token; and a policy carried
 * as the document's own first element, because the sandbox attribute
 * alone does not stop a page fetching the network. The panel draws the
 * label above it - the frame cannot, and a package must not be able to
 * claim a name or a tier that is not its own. */
var EXT_FRAME_POLICY =
  "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; " +
  "img-src blob: data:; connect-src 'none'; form-action 'none'; base-uri 'none'; webrtc 'block'";

function extFrameDoc(html) {
  /* The policy goes first, before anything the package wrote, so that it
   * governs the whole document. A package may add its own policy after
   * it and only make it stricter. */
  return '<meta http-equiv="Content-Security-Policy" content="' + EXT_FRAME_POLICY + '">' + html;
}

/* ---- the bridge ----
 *
 * A frame holds no session and can reach nothing itself, so everything
 * it wants goes through here. Three rules run this:
 *
 *   1. A frame is known by `event.source` and never by `event.origin`.
 *      Every sandboxed frame reports the origin `null`, so two open
 *      packages are indistinguishable by origin - identifying by origin
 *      would let either speak for the other.
 *   2. The capability checked is the one the panel holds for that
 *      package, read from the machine's own list. A frame's claim about
 *      what it may do is never an input.
 *   3. The panel makes the call under its own session. The token and the
 *      camera key never go into a message: a camera frame is fetched
 *      here and handed over as bytes.
 */
var EXT_FRAMES = [];            /* {win, id} for each open frame */

function extBridgeOwner(win) {
  var i;
  for (i = 0; i < EXT_FRAMES.length; i++) if (EXT_FRAMES[i].win === win) return EXT_FRAMES[i].id;
  return null;
}

/* What the host would honor for this package, which is the question the
 * bridge has to ask. `package.capabilities` is the manifest's list: what
 * the package asked for, granted or not. `effective` is what the host
 * built its identity from - the capabilities that need no grant, and
 * those the operator granted - and it is the same list the package's own
 * API socket answers with. A package the operator disabled holds
 * nothing: disabling is the way out of everything it does. A host too old
 * to send `effective` leaves the bridge holding nothing, which is the
 * side to be wrong on. */
function extHolds(id, cap) {
  var list = EXT_LAST.packages || [],
    i;
  for (i = 0; i < list.length; i++)
    if (list[i].id === id) {
      if (!list[i].enabled) return false;
      return (list[i].effective || []).indexOf(cap) >= 0;
    }
  return false;
}

var EXT_BRIDGE_CALLS = {
  'self': { cap: null },
  'machine.status': { cap: 'machine.read', path: '/status' },
  'machine.cool': { cap: 'machine.read', path: '/cool/status' },
  'machine.mode': { cap: 'machine.read', path: '/mode' },
  'settings.get': { cap: 'settings.own' },
  'settings.set': { cap: 'settings.own' },
  'camera.frame': { cap: null },
  'motion.jog': { cap: 'motion.jog' }
};

function extBridgeReply(win, rid, ok, value) {
  /* The frame's origin is opaque, so the reply is addressed to the
   * window itself with '*'; nothing secret goes in it. */
  try {
    win.postMessage({ forgefirm: 1, id: rid, ok: ok, value: ok ? value : undefined, error: ok ? undefined : value }, '*');
  } catch (e) {
    /* the frame went away */
  }
}

function extBridgeCall(id, call, args) {
  var spec = EXT_BRIDGE_CALLS[call];
  if (call === 'self') {
    var list = EXT_LAST.packages || [],
      i;
    for (i = 0; i < list.length; i++)
      if (list[i].id === id)
        return Promise.resolve({
          id: id,
          version: list[i].version,
          tier: list[i].tier,
          /* What it may use, not what it asked for: the same list its own
           * API socket gives a service at GET /v0/self. */
          capabilities: list[i].effective || []
        });
    return Promise.reject('this package is not installed');
  }
  if (call === 'camera.frame') {
    var which = args && args.camera === 'head' ? 'head' : 'lid';
    if (!extHolds(id, 'camera.' + which)) return Promise.reject('this package does not hold that camera');
    /* Fetched by the panel, under the panel's own session, and handed
     * over as bytes. The camera key is never in a message. */
    return fx('/cam/snapshot?cam=' + which + '&res=half&background=1').then(function (r) {
      if (!r.ok) return Promise.reject('the machine did not give a frame');
      return r.blob();
    });
  }
  if (call === 'settings.get') return fx('/ext/settings?id=' + encodeURIComponent(id)).then(extAnswer);
  if (call === 'settings.set')
    return fx('/ext/settings', {
      method: 'POST',
      body: new URLSearchParams({ id: id, set: JSON.stringify(args || {}) })
    }).then(extAnswer);
  if (call === 'motion.jog') {
    /* The four the machine's route takes, each a number, and nothing else
     * the frame put in the message. The machine judges the values - its
     * bounds, its mode, its lease, the sender who always wins - and this
     * is only about what is carried to it. */
    var jog = new URLSearchParams();
    ['x', 'y', 'z', 'feed'].forEach(function (k) {
      if (typeof (args || {})[k] === 'number' && isFinite(args[k])) jog.set(k, String(args[k]));
    });
    if (!jog.has('x') && !jog.has('y') && !jog.has('z')) return Promise.reject('a jog moves at least one axis');
    return fx('/motion/jog', { method: 'POST', body: jog }).then(extAnswer);
  }
  return fx(spec.path).then(extAnswer);
}

window.addEventListener('message', function (ev) {
  var msg = ev.data;
  if (!msg || msg.forgefirm !== 1 || typeof msg.call !== 'string') return;
  var id = extBridgeOwner(ev.source);
  if (!id) return;                        /* not a frame this panel opened */
  var spec = EXT_BRIDGE_CALLS[msg.call];
  if (!spec) return extBridgeReply(ev.source, msg.id, false, 'no such call');
  if (spec.cap && !extHolds(id, spec.cap))
    return extBridgeReply(ev.source, msg.id, false, 'this package does not hold ' + spec.cap);
  extBridgeCall(id, msg.call, msg.args).then(
    function (v) { extBridgeReply(ev.source, msg.id, true, v); },
    function (e) { extBridgeReply(ev.source, msg.id, false, String(e && e.message ? e.message : e)); }
  );
});

function extOpenUi(id) {
  var host = $('extframe');
  if (!host) return;
  host.innerHTML = "<p class='hint'>Loading the interface...</p>";
  fx('/ext/ui?id=' + encodeURIComponent(id))
    .then(extAnswer)
    .then(function (j) {
      var pkg = extFrameOwner(id),
        f = document.createElement('iframe');
      host.innerHTML = '';
      /* The label is the panel's, above the frame and outside it. */
      var label = document.createElement('div');
      label.className = 'extframe-label';
      /* The name and the tier are the panel's own knowledge of the
       * package. When it has none - the list has not loaded - the label
       * says the id and nothing it cannot stand behind. */
      label.innerHTML =
        '<b>' + esc(pkg.name || id) + '</b> ' +
        (pkg.tier ? extTier(pkg.tier) + ' ' : '') +
        "<span class='mono'>" + esc(id) + "</span>" +
        " <button type='button' class='extframeclose'>Close</button>";
      host.appendChild(label);
      /* No allow-same-origin: with it the frame would hold the panel's
       * session and the rest of this would be decoration. */
      f.setAttribute('sandbox', 'allow-scripts');
      f.setAttribute('referrerpolicy', 'no-referrer');
      f.className = 'extframe';
      f.srcdoc = extFrameDoc(j.html || '');
      host.appendChild(f);
      /* Remembered by its window, which is how the bridge will know it:
       * the frame's origin is null and tells the panel nothing. */
      EXT_FRAMES = [{ win: f.contentWindow, id: id }];
    })
    .catch(function (e) {
      host.innerHTML = "<p class='hint'>" + esc(String(e)) + '</p>';
    });
}

/* What the panel knows of the package, for the label it draws. */
var EXT_LAST = { packages: [] };
function extFrameOwner(id) {
  var list = EXT_LAST.packages || [],
    i;
  for (i = 0; i < list.length; i++)
    if (list[i].id === id) return { name: (list[i].package || {}).name, tier: list[i].tier };
  return {};
}

function loadExt() {
  fx('/ext/status')
    .then(extAnswer)
    .then(function (j) {
      EXT_LAST = j;
      return j;
    })
    .then(renderExt)
    .catch(function (e) {
      $('extpkgs').innerHTML = "<p class='hint'>" + esc(String(e)) + '</p>';
    });
}
function extAct(id, action) {
  if (action === 'remove' && !window.confirm('Remove ' + id + ' and its data?')) return;
  extSay('msg-ext', '…');
  fx('/ext/package', { method: 'POST', body: new URLSearchParams({ id: id, action: action }) })
    .then(extAnswer)
    .then(function (j) {
      extSay('msg-ext', '');
      renderExt(j);
    })
    .catch(function (e) {
      extSay('msg-ext', String(e));
      loadExt();
    });
}
/* The master switch. On goes over the advisory: its text as it stands, and
 * the typed phrase. */
function extSwitch() {
  if ($('extswitch').getAttribute('data-on') === '1') {
    extSay('msg-ext', '…');
    postSettings({ ext_enabled: '0' })
      .then(function () {
        extSay('msg-ext', '');
        loadExt();
      })
      .catch(function (e) {
        extSay('msg-ext', String(e));
      });
    return;
  }
  fetch('/advisories/' + EXT_DOC)
    .then(function (r) {
      if (!r.ok) throw 'the advisory cannot be read';
      extAdvisory = r.headers.get('ETag');
      return r.text();
    })
    .then(function (t) {
      $('extadvtext').innerHTML = mdRender(t);
      $('extadv').style.display = '';
      $('extphrase').value = '';
      $('extphrase').focus();
    })
    .catch(function (e) {
      extSay('msg-ext', String(e));
    });
}
function extTurnOn() {
  extSay('msg-extadv', '…');
  postSettings({ ext_enabled: '1', advisory: extAdvisory || '', phrase: $('extphrase').value })
    .then(function () {
      extSay('msg-extadv', '');
      $('extadv').style.display = 'none';
      loadExt();
    })
    .catch(function (e) {
      extSay('msg-extadv', String(e));
    });
}
function extCancelOn() {
  $('extadv').style.display = 'none';
  extSay('msg-extadv', '');
}
/* Installing: the upload, what the host says it is, the grants and the
 * consent its tier takes, the install. */
function extUpload() {
  var f = $('extfile').files[0],
    fd;
  if (!f) {
    extSay('msg-extup', 'choose a package file (.ffx) first');
    return;
  }
  fd = new FormData();
  fd.append('file', f, f.name);
  extSay('msg-extup', 'uploading…');
  fx('/ext/upload', { method: 'POST', body: fd })
    .then(extAnswer)
    .then(function (j) {
      extSay('msg-extup', '');
      extStaged = j;
      renderStaged(j);
    })
    .catch(function (e) {
      extSay('msg-extup', String(e));
      extStaged = null;
      $('extstaged').style.display = 'none';
    });
}
function renderStaged(j) {
  var m = j.package || {},
    caps = [],
    g = '',
    i,
    c,
    need = j.needs_grant || [],
    fresh = j.new_capabilities || [];
  for (i = 0; i < (m.capabilities || []).length; i++) {
    c = m.capabilities[i];
    if (need.indexOf(c) >= 0)
      caps.push(
        "<label><input type='checkbox' class='extgrant' value='" +
          esc(c) +
          "'> " +
          extCap(c) +
          " <span class='hint'>(only with your grant)</span></label>"
      );
    else caps.push(extCap(c) + (j.update && fresh.indexOf(c) >= 0 ? " <span class='b-warn'>new</span>" : ''));
  }
  g += kv(
    m.name || m.id || 'Package',
    extTier(j.tier) +
      " <span class='mono'>" +
      esc(m.id || '') +
      ' ' +
      esc(m.version || '') +
      '</span>' +
      (m.author ? ', by ' + esc(m.author) : '') +
      '<br>' +
      esc((EXT_TIERS[j.tier] || ['', '', ''])[2]) +
      (j.update
        ? '<br>' + (j.downgrade ? "<span class='b-warn'>An older version</span> than" : 'An update of') + ' the installed ' + esc(j.from_version)
        : '') +
      (m.description ? '<br>' + esc(m.description) : '') +
      (caps.length ? "<br>It asks to:<ul class='extcaps'><li>" + caps.join('</li><li>') + '</li></ul>' : '')
  );
  $('extstagedkv').innerHTML = g;
  $('extconsent-typed').style.display = j.consent === 'typed' ? '' : 'none';
  $('extconsent-button').style.display = j.consent === 'button' ? '' : 'none';
  $('extinstallphrase').value = '';
  $('extstaged').style.display = '';
}
function extInstall() {
  var grants = [],
    boxes = document.querySelectorAll('.extgrant'),
    i;
  for (i = 0; i < boxes.length; i++) if (boxes[i].checked) grants.push(boxes[i].value);
  extSay('msg-extinstall', 'installing…');
  fx('/ext/install', {
    method: 'POST',
    body: new URLSearchParams({ grants: grants.join(','), phrase: $('extinstallphrase').value })
  })
    .then(extAnswer)
    .then(function (j) {
      extSay('msg-extinstall', '');
      extStaged = null;
      $('extstaged').style.display = 'none';
      $('extfile').value = '';
      renderExt(j);
    })
    .catch(function (e) {
      extSay('msg-extinstall', String(e));
    });
}
function extDiscard() {
  fx('/ext/upload/discard', { method: 'POST' }).then(function () {
    extStaged = null;
    $('extstaged').style.display = 'none';
    $('extfile').value = '';
    extSay('msg-extinstall', '');
  });
}
document.addEventListener('click', function (ev) {
  if (!ev.target.closest) return;
  var b = ev.target.closest('.extact');
  if (b) extAct(b.getAttribute('data-id'), b.getAttribute('data-action'));
  var k = ev.target.closest('.extkeyrm');
  if (k) extKeyRemove(k.getAttribute('data-name'));
  var u = ev.target.closest('.extui');
  if (u) extOpenUi(u.getAttribute('data-id'));
  if (ev.target.closest('.extframeclose')) {
    var h = $('extframe');
    if (h) h.innerHTML = '';
    EXT_FRAMES = [];
  }
});
document.addEventListener('change', function (ev) {
  var s = ev.target;
  if (s && s.classList && s.classList.contains('exthold')) extAct(s.getAttribute('data-id'), s.value);
});
