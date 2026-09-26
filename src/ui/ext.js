/*
 * ext.js - forgectrl panel: the Extension packages card and the Extensions tab
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 *
 * The operator's door to extension packages: whether extensions are on
 * (turned on over their advisory), what is installed and how each is
 * doing, the switch and the hold's kind and the removal of one package,
 * and the install of a new one with the consent its tier takes, from a
 * file or from the catalog. Every decision is forgectrl's and the extension
 * host's: this card shows what GET /ext/status and GET /ext/catalog say and
 * sends what the operator chose.
 *
 * The Extensions tab holds a card for each package that is turned on and
 * has a page of its own: the heading is the panel's label, the body the
 * package's page in its sandboxed frame, with the bridge below serving
 * every frame open there.
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
  ui: 'Show a card of its own on the Extensions tab',
  'ui.background': 'Keep its page running while you are on another tab',
  'sender.keep_out': 'Disconnect the Grbl sender while it uses the machine (only when the machine is idle)',
  'net.outbound': 'Connect to',
  'net.outbound.operator': 'Connect to the places you name for it, below',
  'net.listen': 'Listen on port',
  storage: 'Keep data on the machine, in MiB up to',
  mcode: 'Answer, in a job that names it, holding the job until it does, M',
  wizard: 'Add a check of its own to the Setup page'
};
var EXT_TIERS = {
  official: ['Official', 'b-ok', 'Signed with the OpenGlow extension key.'],
  community: ['Community', 'b-warn', "Signed with a key you added to this machine, or with its author's key that OpenGlow's catalog names for it; not by OpenGlow."],
  unverified: ['Unverified', 'b-bad', 'Signed by nobody this machine trusts, or not signed at all.']
};
var EXT_DOC = 'extensions';             /* the advisory turning extensions on agrees to */
var extStaged = null,
  extAdvisory = null,
  extCatalog = null;

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
/* A package's name as the machine lists it, or its id. */
function extName(id) {
  var list = EXT_LAST.packages || [],
    i;
  for (i = 0; i < list.length; i++) if (list[i].id === id) return (list[i].package || {}).name || id;
  return id;
}

function extHasPage(p) {
  return ((p.package || {}).capabilities || []).indexOf('ui') >= 0;
}

function renderExt(j) {
  var g = '',
    list = j.packages || [],
    i,
    c;
  EXT_LAST = j;                         /* the bridge and the catalog go by the latest word */
  applyExtSurface(!!j.enabled);
  extCardsFollow(j);
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
    var named = '';
    /* The places the operator named for a package that asks to be given
     * some: the operator's own list, on the panel's card, never the
     * package's page. */
    if ((m.capabilities || []).indexOf('net.outbound.operator') >= 0) {
      var dl = p.destinations || [];
      for (c = 0; c < dl.length; c++)
        named +=
          "<li><span class='mono'>" + esc(dl[c]) + "</span> <button type='button' class='btn btn-sm btn-outline-danger extdestrm' " +
          "data-id='" + esc(p.id) + "' data-dest='" + esc(dl[c]) + "'>Remove</button></li>";
      named =
        'Places you named for it' + (dl.length ? ":<ul class='extcaps'>" + named + '</ul>' : ': none yet.<br>') +
        "<input class='form-control form-control-sm extdestin' data-id='" + esc(p.id) +
        "' placeholder='host:port, as 192.168.1.40:80 or broker.lan:1883' style='display: inline-block; width: auto' " +
        "maxlength='95'> <button type='button' class='btn btn-sm btn-outline-secondary extdestadd' data-id='" + esc(p.id) +
        "'>Add</button><br>";
    }
    g += kv(
      m.name || p.id,
      extTier(p.tier) +
        " <span class='mono'>" +
        esc(p.id) +
        ' ' +
        esc(p.version) +
        '</span>' +
        (m.author ? ', by ' + esc(m.author) : '') +
        '<br>' +
        extStateOf(j, p) +
        (extHasPage(p) && p.enabled && j.enabled
          ? "<br>Its page: <a href='#ext' data-nolock='1'>Extensions tab</a>"
          : '') +
        extWithdrawn(p.withdrawn, true) +
        (m.description ? '<br>' + esc(m.description) : '') +
        (caps.length ? "<br>It may:<ul class='extcaps'><li>" + caps.join('</li><li>') + '</li></ul>' : '<br>') +
        named +
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
  if (extCatalog) renderCatalog(extCatalog);
  $('extswitch').textContent = j.enabled ? 'Turn extensions off' : 'Turn extensions on…';
  $('extswitch').setAttribute('data-on', j.enabled ? '1' : '0');
}
/* The catalog: OpenGlow's signed list of packages, which the machine
 * fetches only when the operator asks. Getting one fetches it into the
 * staging file, and from there it is an upload: the host says what it is,
 * and the install takes the consent its tier takes. */
function extHostOf(url) {
  var m = /^https:\/\/([^\/?#]+)/.exec(url || '');
  return m ? m[1] : 'the catalog\'s host';
}
/* MAJOR.MINOR.PATCH compared by number; the host's judgment is the one
 * that counts, and this only picks which listed version to name. */
function extVerCmp(a, b) {
  var x = String(a).split('-')[0].split('.'),
    y = String(b).split('-')[0].split('.'),
    i;
  for (i = 0; i < 3; i++) if (+x[i] !== +y[i]) return +x[i] < +y[i] ? -1 : 1;
  return 0;
}
/* What OpenGlow's catalog says it withdrew, of a package installed here or
 * staged: the machine removes nothing on its own. */
function extWithdrawn(w, installed) {
  if (!w) return '';
  return (
    "<br><span class='b-warn'>OpenGlow withdrew " +
    (w.scope === 'version' ? 'this version' : 'this package') +
    ' from its catalog</span>' +
    (w.reason ? ': ' + esc(w.reason) : '') +
    '.' +
    (installed ? ' It stays installed; remove it if you no longer want it.' : '')
  );
}
function renderCatalog(j) {
  var idx = j && j.index,
    have = {},
    g = '',
    i,
    c;
  extCatalog = j;
  if (!idx) {
    $('extcatalog').innerHTML =
      "<p class='hint'>This machine has not fetched the catalog. Fetch the catalog asks " +
      esc(extHostOf(j && j.url)) +
      ' for it, this once.</p>';
    return;
  }
  (EXT_LAST.packages || []).forEach(function (p) {
    have[p.id] = p.version;
  });
  for (i = 0; i < (idx.packages || []).length; i++) {
    /* The host judged every listed version against this firmware as it
     * read the index: the one offered is the newest this firmware runs,
     * and a newer one that it does not run says why. */
    var e = idx.packages[i],
      vers = e.versions || [],
      caps = [],
      inst = have[e.id],
      off = null,
      newest = null;
    for (c = 0; c < vers.length; c++) {
      if (vers[c].version === e.offer) off = vers[c];
      if (!newest || extVerCmp(vers[c].version, newest.version) > 0) newest = vers[c];
    }
    var shown = off || newest || {};
    for (c = 0; c < (shown.capabilities || []).length; c++) caps.push(extCap(shown.capabilities[c]));
    g += kv(
      e.name || e.id,
      /* An entry the index names no key for is OpenGlow's own; the tier the
       * install goes by is the host's reading of the archive itself. */
      extTier(e.key ? 'community' : 'official') +
        " <span class='mono'>" +
        esc(e.id) +
        ' ' +
        esc(shown.version || '') +
        '</span>' +
        (e.author ? ', by ' + esc(e.author) : '') +
        (e.homepage ? " <a href='" + esc(e.homepage) + "' target='_blank' rel='noopener noreferrer'>home page</a>" : '') +
        (e.description ? '<br>' + esc(e.description) : '') +
        (caps.length ? "<br>It asks to:<ul class='extcaps'><li>" + caps.join('</li><li>') + '</li></ul>' : '<br>') +
        (off && newest && newest !== off
          ? "<span class='hint'>" + esc(newest.version) + ' is listed too, and ' + esc(newest.why || 'does not run here') + '.</span><br>'
          : '') +
        (!off
          ? "<span class='b-warn'>No version of it runs on this firmware</span>" + (e.why ? ': ' + esc(e.why) : '') + '.'
          : inst === off.version
          ? "<span class='hint'>Installed.</span>"
          : "<button class='btn btn-sm btn-outline-secondary extcatget' data-id='" +
            esc(e.id) +
            "'>" +
            'Get ' + esc(off.version) + (inst ? ' (this machine has ' + esc(inst) + ')' : '') +
            '</button>')
    );
  }
  $('extcatalog').innerHTML =
    (g || "<p class='hint'>The catalog lists no package.</p>") +
    "<p class='hint'>Catalog " +
    esc(idx.version || '') +
    ', signed with the OpenGlow extension key.</p>';
}
function loadCatalog() {
  fx('/ext/catalog')
    .then(extAnswer)
    .then(renderCatalog)
    .catch(function (e) {
      $('extcatalog').innerHTML = "<p class='hint'>" + esc(String(e)) + '</p>';
    });
}
function extCatalogRefresh() {
  extSay('msg-extcat', 'fetching the catalog\u2026');
  fx('/ext/catalog/refresh', { method: 'POST' })
    .then(extAnswer)
    .then(function (j) {
      extSay('msg-extcat', '');
      renderCatalog(j);
    })
    .catch(function (e) {
      extSay('msg-extcat', String(e));
    });
}
function extCatalogGet(id) {
  /* The one staging file is this fetch's now: whatever was staged goes. */
  extStaged = null;
  $('extstaged').style.display = 'none';
  extSay('msg-extcat', 'fetching ' + id + '\u2026');
  fx('/ext/catalog/get', { method: 'POST', body: new URLSearchParams({ id: id }) })
    .then(extAnswer)
    .then(function (j) {
      extSay('msg-extcat', '');
      extStaged = j;
      renderStaged(j);
      $('extstaged').scrollIntoView({ block: 'nearest' });
    })
    .catch(function (e) {
      extSay('msg-extcat', String(e));
    });
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
   * it and only make it stricter. Then the color scheme: the frame takes
   * the panel's own (theme.css), which the page reads as its
   * prefers-color-scheme, so a page that names no scheme of its own
   * follows the panel's light or dark theme. */
  return (
    '<meta http-equiv="Content-Security-Policy" content="' + EXT_FRAME_POLICY + '">' +
    '<meta name="color-scheme" content="light dark">' +
    html
  );
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
var EXT_FRAMES = [];            /* {id, version, card, el, win, stream} for each card on the Extensions tab */

function extBridgeOwner(win) {
  var i;
  if (!win) return null;
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
  'camera.stream': { cap: null },
  'motion.jog': { cap: 'motion.jog' },
  'motion.cancel': { cap: 'motion.jog' },
  'motion.job': { cap: 'motion.job' },
  'motion.job.abort': { cap: 'motion.job' },
  'motion.job.state': { cap: 'motion.job' },
  'frame.height': { cap: null },
  'service.call': { cap: 'ui' }
};
/* How tall a page may ask its frame to be. The frame is the panel's and
 * sits in the panel's layout: a page picks its height inside these, and
 * nothing a page does moves the label above it or the page around it. */
var EXT_FRAME_MIN = 200,
  EXT_FRAME_MAX = 1400;
var EXT_JOB_MAX = 2 * 1024 * 1024;      /* a program a page may hand the panel, in characters */
var EXT_CALL_BODY_MAX = 4096;           /* what a page may send its own service, as JSON */

function extBridgeReply(win, rid, ok, value) {
  /* The frame's origin is opaque, so the reply is addressed to the
   * window itself with '*'; nothing secret goes in it. */
  try {
    win.postMessage({ forgefirm: 1, id: rid, ok: ok, value: ok ? value : undefined, error: ok ? undefined : value }, '*');
  } catch (e) {
    /* the frame went away */
  }
}

function extDestinations(p) {
  var caps = (p.package || {}).capabilities || [],
    out = [],
    i;
  for (i = 0; i < caps.length; i++) if (caps[i].indexOf('net.outbound:') === 0) out.push(caps[i].slice(13));
  if (caps.indexOf('net.outbound.operator') >= 0) out = out.concat(p.destinations || []);
  return out;
}

/* ---- a camera's video, for a page ----
 *
 * A frame can fetch nothing, so the panel fetches the camera's stream
 * under its own session, and hands the page each picture as bytes, with
 * nothing of the session in them. One stream a page; it ends when the
 * page asks, when the page goes away, or when the machine ends it, and
 * the page is told which. */
var EXT_STREAM_BOUNDARY = '--forgectrl-frame';

function extStreamStop(fr) {
  if (fr && fr.stream) {
    fr.stream.abort();
    fr.stream = null;
  }
}

function extStreamTell(fr, msg) {
  msg.forgefirm = 1;
  msg.stream = 'camera';
  try {
    if (fr.win) fr.win.postMessage(msg, '*');
  } catch (e) {
    /* the frame went away */
  }
}

function extStreamStart(fr, url) {
  var ctl = new AbortController();
  fr.stream = ctl;
  return fx(url, { signal: ctl.signal }).then(function (r) {
    if (!r.ok)
      return r.text().then(function (t) {
        fr.stream = null;
        throw t || 'the camera did not answer';
      });
    var reader = r.body.getReader(),
      buf = new Uint8Array(0),
      marker = new TextEncoder().encode('\r\n\r\n');
    function find(hay, needle, from) {
      outer: for (var i = from; i <= hay.length - needle.length; i++) {
        for (var j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
        return i;
      }
      return -1;
    }
    function frames() {
      /* Each part: the boundary, its headers with a Content-Length, a
       * blank line, and that many bytes of JPEG. */
      for (;;) {
        var end = find(buf, marker, 0);
        if (end < 0) return;
        var head = new TextDecoder().decode(buf.subarray(0, end)),
          m = /Content-Length:\s*(\d+)/i.exec(head);
        if (!m || head.indexOf(EXT_STREAM_BOUNDARY) < 0) {
          buf = buf.subarray(end + 4);
          continue;
        }
        var n = +m[1];
        if (buf.length < end + 4 + n) return;
        var jpeg = buf.slice(end + 4, end + 4 + n);
        buf = buf.subarray(end + 4 + n);
        extStreamTell(fr, { frame: new Blob([jpeg], { type: 'image/jpeg' }) });
      }
    }
    (function pump() {
      reader.read().then(
        function (chunk) {
          if (fr.stream !== ctl) return;
          if (chunk.done) {
            fr.stream = null;
            extStreamTell(fr, { end: true, error: 'the camera stream ended' });
            return;
          }
          var joined = new Uint8Array(buf.length + chunk.value.length);
          joined.set(buf);
          joined.set(chunk.value, buf.length);
          buf = joined;
          frames();
          pump();
        },
        function () {
          if (fr.stream !== ctl) return;
          fr.stream = null;
          extStreamTell(fr, { end: true, error: 'the camera stream ended' });
        }
      );
    })();
    return { on: true };
  });
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
          capabilities: list[i].effective || [],
          /* Where its service may connect, its manifest's and then the
           * places the operator named: GET /v0/self's list too. */
          destinations: extDestinations(list[i])
        });
    return Promise.reject('this package is not installed');
  }
  if (call === 'camera.stream') {
    var sa = args || {},
      own = extCardOf(id),
      cam = sa.camera === 'head' ? 'head' : 'lid';
    if (!own) return Promise.reject('this page has no frame open');
    extStreamStop(own);
    if (sa.on === false) return Promise.resolve({ on: false });
    if (!extHolds(id, 'camera.' + cam)) return Promise.reject('this package does not hold that camera');
    var q = new URLSearchParams({ cam: cam });
    if (sa.fps !== undefined) {
      if (typeof sa.fps !== 'number' || sa.fps % 1 !== 0 || sa.fps < 1 || sa.fps > 15)
        return Promise.reject('fps is a whole number from 1 to 15');
    }
    q.set('fps', String(sa.fps || 5));
    if (sa.lamp !== undefined) {
      if (typeof sa.lamp !== 'number' || sa.lamp % 1 !== 0 || sa.lamp < 0 || sa.lamp > 1023)
        return Promise.reject('lamp is a whole number from 0 to 1023');
      q.set('lamp', String(sa.lamp));
    }
    return extStreamStart(own, '/cam/stream?' + q.toString());
  }
  if (call === 'camera.frame') {
    var a = args || {},
      which = a.camera === 'head' ? 'head' : 'lid',
      shot = new URLSearchParams({ cam: which });
    if (!extHolds(id, 'camera.' + which)) return Promise.reject('this package does not hold that camera');
    /* The frame the page asked for, each value checked here and carried as
     * the machine's route takes it: the whole frame or half of it, a JPEG
     * quality, and the camera's own lamp for this one frame (the machine
     * puts its level back after it). Nothing else in the message goes. */
    if (a.resolution !== undefined && a.resolution !== 'full' && a.resolution !== 'half')
      return Promise.reject('resolution is full or half');
    shot.set('res', a.resolution === 'full' ? 'full' : 'half');
    if (a.quality !== undefined) {
      if (typeof a.quality !== 'number' || a.quality % 1 !== 0 || a.quality < 1 || a.quality > 100)
        return Promise.reject('quality is a whole number from 1 to 100');
      shot.set('q', String(a.quality));
    }
    if (a.lamp !== undefined) {
      if (typeof a.lamp !== 'number' || a.lamp % 1 !== 0 || a.lamp < 0 || a.lamp > 1023)
        return Promise.reject('lamp is a whole number from 0 to 1023');
      shot.set('lamp', String(a.lamp));
    }
    shot.set('background', '1');
    /* Fetched by the panel, under the panel's own session, and handed
     * over as bytes. The camera key is never in a message. */
    return fx('/cam/snapshot?' + shot.toString()).then(function (r) {
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
  if (call === 'motion.cancel') return fx('/motion/cancel', { method: 'POST' }).then(extAnswer);
  if (call === 'motion.job') {
    /* A program the page wrote, run as the machine's one sender under
     * every gate a sender is under, the arm press included. Who it is
     * from is the panel's word - the package's id - and never a name the
     * page chose, which is also what the arm prompt shows. */
    var p = args || {},
      form = new FormData();
    if (typeof p.program !== 'string' || !p.program.length) return Promise.reject('program is the text of a G-code program');
    if (p.program.length > EXT_JOB_MAX) return Promise.reject('a program from a page is at most 2 MiB');
    form.append('name', id);
    var k, key;
    for (k = 0; k < 2; k++) {
      key = ['lit_within_s', 'timeout_s'][k];
      if (p[key] === undefined) continue;
      if (typeof p[key] !== 'number' || !isFinite(p[key]) || p[key] < 0)
        return Promise.reject(key + ' is a number of seconds');
      form.append(key, String(p[key]));
    }
    form.append('program', new Blob([p.program], { type: 'text/plain' }), 'program.gcode');
    return fx('/job', { method: 'POST', body: form }).then(extAnswer);
  }
  if (call === 'frame.height') {
    var px = (args || {}).px,
      fr = null;
    if (typeof px !== 'number' || !isFinite(px)) return Promise.reject('px is a number of pixels');
    for (var f = 0; f < EXT_FRAMES.length; f++) if (EXT_FRAMES[f].id === id && EXT_FRAMES[f].el) fr = EXT_FRAMES[f].el;
    if (!fr) return Promise.reject('this page has no frame open');
    px = Math.max(EXT_FRAME_MIN, Math.min(EXT_FRAME_MAX, Math.round(px)));
    fr.style.height = px + 'px';
    return Promise.resolve({ px: px });
  }
  if (call === 'service.call') {
    /* The page's own service and no other: the package is the panel's
     * word, from the frame that asked. The method, the path, and the body
     * are carried as the page gave them, the body as the JSON of an
     * object; the host holds all three to their form. */
    var c = args || {},
      callForm = new URLSearchParams({ id: id });
    if (c.method !== 'GET' && c.method !== 'POST') return Promise.reject('a call is GET or POST');
    if (typeof c.path !== 'string' || c.path.charAt(0) !== '/') return Promise.reject("a call's path starts with '/'");
    callForm.set('method', c.method);
    callForm.set('path', c.path);
    if (c.body !== undefined && c.body !== null) {
      if (c.method === 'GET') return Promise.reject('a GET call has no body');
      if (typeof c.body !== 'object' || Array.isArray(c.body)) return Promise.reject("a call's body is an object");
      var text = JSON.stringify(c.body);
      if (text.length > EXT_CALL_BODY_MAX) return Promise.reject("a call's body is at most 4096 bytes");
      callForm.set('body', text);
    }
    return fx('/ext/call', { method: 'POST', body: callForm })
      .then(extAnswer)
      .then(function (j) { return { status: j.status, body: j.body }; });
  }
  if (call === 'motion.job.abort') return fx('/job/abort', { method: 'POST' }).then(extAnswer);
  if (call === 'motion.job.state') return fx('/job').then(extAnswer);
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

/* ---- the Extensions tab ----
 *
 * One card for each package that is turned on and has a page, in the
 * order the machine lists them. A page runs while the tab is shown: one
 * nobody is looking at asks the machine for nothing, and nothing it does
 * starts from a tab the operator has left. The exception is the operator's
 * to make: a page granted ui.background loads when the panel does and keeps
 * running on every tab, until the panel closes or its package is turned off.
 */
var extTabOn = false,
  extTabTimer = null;

function extTabShown(on) {
  if (on === extTabOn) return;
  extTabOn = on;
  if (on) extStatusPoll();
  else extCardsFollow(EXT_LAST);
}

/* A page that may run off its tab: the machine's word, its effective
 * list, never the manifest's. */
function extBackground(p) {
  return p.enabled && (p.effective || []).indexOf('ui.background') >= 0;
}

/* The machine's list is asked for again every 5 s while any page runs, so
 * a package turned off anywhere takes its page with it. */
function extPollKeep() {
  var need = extTabOn || EXT_FRAMES.length > 0;
  if (need && !extTabTimer) extTabTimer = setInterval(extStatusPoll, 5000);
  if (!need && extTabTimer) {
    clearInterval(extTabTimer);
    extTabTimer = null;
  }
}

function extStatusPoll() {
  fx('/ext/status')
    .then(extAnswer)
    .then(renderExt)
    .catch(function (e) {
      if (EXT_FRAMES.length) return;      /* the cards stand; the next answer decides */
      $('extnonemsg').textContent = String(e);
      $('extnone').style.display = '';
    });
}

function extCardOf(id) {
  var i;
  for (i = 0; i < EXT_FRAMES.length; i++) if (EXT_FRAMES[i].id === id) return EXT_FRAMES[i];
  return null;
}

function extListIndex(id) {
  var list = EXT_LAST.packages || [],
    i;
  for (i = 0; i < list.length; i++) if (list[i].id === id) return i;
  return list.length;
}

/* The cards follow the machine's list. A card goes when its package is
 * turned off, removed, or loses its page, and when the tab is left unless
 * its page may run off it: turning a package off is the operator's way out
 * of everything it does, and a frame left open would go on asking the
 * bridge. An updated package's page loads afresh. */
function extCardsFollow(j) {
  var list = (j && j.packages) || [],
    want = [],
    i,
    k;
  if (j && j.enabled)
    for (i = 0; i < list.length; i++)
      if (list[i].enabled && extHasPage(list[i]) && (extTabOn || extBackground(list[i]))) want.push(list[i]);
  EXT_FRAMES = EXT_FRAMES.filter(function (fr) {
    for (k = 0; k < want.length; k++) if (want[k].id === fr.id && want[k].version === fr.version) return true;
    extStreamStop(fr);
    fr.card.remove();
    return false;
  });
  for (i = 0; i < want.length; i++) if (!extCardOf(want[i].id)) extOpenUi(want[i].id);
  $('extnonemsg').textContent =
    "No package that is turned on has a page. Install and turn on packages in the System tab's " +
    'Extension packages card.';
  $('extnone').style.display = extTabOn && !want.length ? '' : 'none';
  extPollKeep();
}

/* A package's card, made when the tab has none for it yet, with its page
 * loaded afresh into it. */
function extOpenUi(id) {
  var host = $('extcards'),
    fr = extCardOf(id),
    i;
  if (!host) return;
  if (!fr) {
    var pkg = extFrameOwner(id),
      card = document.createElement('div'),
      at = extListIndex(id),
      before = null;
    card.className = 'card extcard';
    card.setAttribute('data-id', id);
    /* The label is the panel's: the card's heading, above the frame and
     * outside it. The name and the tier are the panel's own knowledge of
     * the package. When it has none - the list has not loaded - the label
     * says the id and nothing it cannot stand behind. */
    card.innerHTML =
      "<h2 class='extframe-label'>" +
      esc(pkg.name || id) +
      (pkg.tier ? ' ' + extTier(pkg.tier) : '') +
      " <span class='mono'>" +
      esc(id) +
      "</span></h2><div class='extframe-body'></div>";
    for (i = 0; i < EXT_FRAMES.length; i++)
      if (extListIndex(EXT_FRAMES[i].id) > at && (!before || extListIndex(EXT_FRAMES[i].id) < extListIndex(before.getAttribute('data-id'))))
        before = EXT_FRAMES[i].card;
    host.insertBefore(card, before);
    fr = { id: id, version: pkg.version, card: card, el: null, win: null, stream: null };
    EXT_FRAMES.push(fr);
  }
  var body = fr.card.querySelector('.extframe-body');
  extStreamStop(fr);
  fr.el = fr.win = null;
  body.innerHTML = "<p class='hint'>Loading the page…</p>";
  fx('/ext/ui?id=' + encodeURIComponent(id))
    .then(extAnswer)
    .then(function (j) {
      if (EXT_FRAMES.indexOf(fr) < 0) return;      /* the card went while the page was on its way */
      var f = document.createElement('iframe');
      /* No allow-same-origin: with it the frame would hold the panel's
       * session and the rest of this would be decoration. */
      f.setAttribute('sandbox', 'allow-scripts');
      f.setAttribute('referrerpolicy', 'no-referrer');
      f.className = 'extframe';
      f.srcdoc = extFrameDoc(j.html || '');
      body.innerHTML = '';
      body.appendChild(f);
      /* Remembered by its window, which is how the bridge will know it:
       * the frame's origin is null and tells the panel nothing. */
      fr.el = f;
      fr.win = f.contentWindow;
    })
    .catch(function (e) {
      if (EXT_FRAMES.indexOf(fr) >= 0) body.innerHTML = "<p class='hint'>" + esc(String(e)) + '</p>';
    });
}

/* What the panel knows of the package, for the label it draws. */
var EXT_LAST = { packages: [] };
function extFrameOwner(id) {
  var list = EXT_LAST.packages || [],
    i;
  for (i = 0; i < list.length; i++)
    if (list[i].id === id) return { name: (list[i].package || {}).name, tier: list[i].tier, version: list[i].version };
  return {};
}

function loadExt() {
  fx('/ext/status')
    .then(extAnswer)
    .then(renderExt)
    .then(loadCatalog)
    .catch(function (e) {
      $('extpkgs').innerHTML = "<p class='hint'>" + esc(String(e)) + '</p>';
    });
}
function extDest(id, action, dest) {
  extSay('msg-ext', '\u2026');
  fx('/ext/dest', { method: 'POST', body: new URLSearchParams({ id: id, action: action, dest: dest }) })
    .then(extAnswer)
    .then(function (j) {
      extSay('msg-ext', '');
      renderExt(j);
    })
    .catch(function (e) {
      extSay('msg-ext', String(e));
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
      extWithdrawn(j.withdrawn, false) +
      (m.description ? '<br>' + esc(m.description) : '') +
      (caps.length ? "<br>It asks to:<ul class='extcaps'><li>" + caps.join('</li><li>') + '</li></ul>' : '')
  );
  $('extstagedkv').innerHTML = g;
  $('extconsent-typed').style.display = j.consent === 'typed' ? '' : 'none';
  $('extconsent-typed-why').textContent = j.endorsed
    ? "Its author's key, which OpenGlow's catalog names for this package, signed it, not OpenGlow. OpenGlow read it " +
      'before listing it; that is not a test, and nobody vouches for it.'
    : 'A key you added to this machine signed this package, not OpenGlow. Nobody reviewed it.';
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
  var cg = ev.target.closest('.extcatget');
  if (cg) extCatalogGet(cg.getAttribute('data-id'));
  var da = ev.target.closest('.extdestadd');
  if (da) {
    var inp = document.querySelector(".extdestin[data-id='" + da.getAttribute('data-id') + "']");
    if (inp && inp.value.trim()) extDest(da.getAttribute('data-id'), 'add', inp.value.trim());
  }
  var dr = ev.target.closest('.extdestrm');
  if (dr) extDest(dr.getAttribute('data-id'), 'remove', dr.getAttribute('data-dest'));
});
document.addEventListener('change', function (ev) {
  var s = ev.target;
  if (s && s.classList && s.classList.contains('exthold')) extAct(s.getAttribute('data-id'), s.value);
});
