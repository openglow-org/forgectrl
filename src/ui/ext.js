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
function renderExt(j) {
  var g = '',
    list = j.packages || [],
    i,
    c;
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
function loadExt() {
  fx('/ext/status')
    .then(extAnswer)
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
});
document.addEventListener('change', function (ev) {
  var s = ev.target;
  if (s && s.classList && s.classList.contains('exthold')) extAct(s.getAttribute('data-id'), s.value);
});
