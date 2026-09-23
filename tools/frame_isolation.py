#!/usr/bin/env python3
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# https://community.openglow.org
# SPDX-License-Identifier:    MIT
"""exthost.ui-frame-isolation: a hostile package page, in the panel's own frame.

The browser harness for a package's page. It runs the panel as the mock
serves it (devserver.py: the files under src/ui/, the page's own headers),
installs one more package in the mock, org.forgetest.hostile, and gives it
frame_isolation_hostile.html as its page. A driver script, added to one
copy of the panel page and to nothing else, opens that page through the
panel's own extOpenUi() once per variant - the whole battery, then one
navigation each - so the frame, its policy, its label and the bridge are
exactly the panel's.

Three listeners stand in for everywhere that is not the machine:

  the internet  http  <port+1>   any request here left the browser
  STUN          udp   <port+2>   a WebRTC binding request left the browser
  preconnect    tcp   <port+3>   a connection hint opened a socket

and the panel's own port records every request that carries a harness
tag, since a request that reaches the machine is a request too. **The
listeners' log is the ground truth**: an attempt failed only if its tag
arrived. A control frame (the same page with no policy) must get out, or
the witnesses prove nothing and the run says NORESULT.

What is asserted: nothing reaches the internet or the machine, and no
navigation leaves the frame; nothing of the panel is read; nothing draws
over it; the bridge answers what the package may use and refuses the rest
by name; the frame is sandboxed with scripts alone and carries the policy
first; the panel's label sits outside the frame; the panel page sends
frame-src 'none'. What is recorded and not asserted, as the operator
decided: WebRTC and a connection hint, the two residuals the frame policy
cannot close (a package with a page can send a little out of the
operator's browser; one without cannot).

Usage:
    python3 tools/frame_isolation.py [--port 8091] [--lan] [--once]
                                     [--out FILE] [--timeout 90]

Then browse http://127.0.0.1:<port>/harness (the verdict is on the page
and in its title). --lan binds every address: Firefox ignores a loopback
STUN server, so a WebRTC reading there needs the PC's LAN address. --once
exits with the verdict (0 PASS, 1 FAIL, 2 NORESULT) when the run files it.
Chrome and Firefox are the browsers this project tests.
"""
import argparse
import json
import os
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qsl, urlsplit

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import devserver as ds  # noqa: E402

HOSTILE_ID = 'org.forgetest.hostile'
HOSTILE_FILE = os.path.join(ds.HERE, 'frame_isolation_hostile.html')

NET = ['fetch-panel', 'xhr-panel', 'img-panel', 'fetch-other', 'xhr-other', 'beacon-other', 'ws-other',
       'eventsource-other', 'img-other', 'imgset-other', 'script-other', 'css-link-other', 'css-bg-other',
       'css-import-other', 'font-other', 'prefetch-other', 'preload-other', 'iframe-other', 'object-other',
       'embed-other', 'video-other', 'audio-other', 'worker-other']
NAVS = ['nav-location', 'nav-assign-panel', 'nav-form', 'nav-meta', 'nav-link', 'nav-popup', 'nav-top']
READS = ['read-parent-token', 'read-parent-document', 'read-parent-frames', 'read-top-location', 'read-cookie',
         'read-localstorage', 'read-parent-localstorage', 'fullscreen', 'alert', 'pointer-lock']
# What a read may answer and still have read nothing: alert() returns at once and draws nothing without
# allow-modals, and a request that is only requested is not an entry.
BENIGN = ('ran: undefined', 'ran: (empty)', 'ran: requested', 'ran: returned')
# The bridge's answers a package with this manifest must get, by name.
BRIDGE = {
    'bridge-self': ('ok: ', '"%s"' % HOSTILE_ID),
    'bridge-granted': ('ok: ', ''),
    'bridge-ungranted': ('refused: ', 'does not hold motion.jog'),
    'bridge-other-camera': ('refused: ', 'does not hold that camera'),
    'bridge-nosuch': ('refused: ', 'no such call'),
    'bridge-claim': ('refused: ', 'does not hold motion.jog'),
    'bridge-frame': ('ok: ', ''),
    'bridge-frame-lamp': ('refused: ', 'lamp is a whole number from 0 to 1023'),
    'bridge-frame-res': ('refused: ', 'resolution is full or half'),
    'bridge-cancel': ('refused: ', 'does not hold motion.jog'),
    'bridge-job': ('refused: ', 'does not hold motion.job'),
    'bridge-job-abort': ('refused: ', 'does not hold motion.job'),
    'bridge-height': ('ok: ', '"px":1400'),
    'bridge-height-bad': ('refused: ', 'px is a number of pixels'),
}
# What the bridge must send the machine for bridge-frame: every value the page asked for that the route takes,
# the capture marked as a background one, and nothing else the message carried.
FRAME_QUERY = {'cam': 'lid', 'res': 'full', 'q': '80', 'lamp': '60', 'background': '1'}
CONTROL_MIN = 10          # of the network vectors, how many the control frame must get out

HITS = []                  # {'side', 'tag', 'path', 't'}
REPORTS = {}               # variant -> what the frame (or the driver) reported
STATE = {'variant': 'main', 'phase': 'none', 'verdict': None, 'prelim_t': 0.0}
SNAPSHOTS = []             # every /cam/snapshot query the panel's bridge made
LOCK = threading.Lock()
DONE = threading.Event()


def record(side, tag, path):
    with LOCK:
        HITS.append({'side': side, 'tag': tag, 'path': path, 't': round(time.time(), 2)})


def hostile_page(mech, variant, host, port):
    """The hostile page with its placeholders filled for this run."""
    with open(HOSTILE_FILE, encoding='utf-8') as f:
        page = f.read()
    for k, v in (('__MECH__', mech), ('__VARIANT__', variant),
                 ('__OTHER__', 'http://%s:%d' % (host, port + 1)), ('__PANEL__', 'http://%s:%d' % (host, port)),
                 ('__STUNHOST__', host), ('__STUNPORT__', str(port + 2)),
                 ('__OTHERHOST__', host), ('__TCPPORT__', str(port + 3))):
        page = page.replace(k, v)
    return page


DRIVER = r"""<script>
/* frame_isolation.py's driver: added to this copy of the panel page alone.
 * It opens the hostile package's page through the panel's own extOpenUi(),
 * forwards what each frame reports to the harness, and draws the verdict. */
(function () {
  'use strict';
  var ID = '__ID__', NAVS = __NAVS__, waiting = null, control = null;
  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }
  function post(path, obj) {
    return fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(obj) });
  }
  window.addEventListener('message', function (ev) {
    var m = ev.data;
    if (!m || m.harness !== 1 || !waiting) return;
    var f = document.querySelector('#extframe iframe');
    if ((f && ev.source === f.contentWindow) || (control && ev.source === control.contentWindow)) waiting(m);
  });
  function awaitReport(ms) {
    var got = new Promise(function (r) { waiting = r; });
    return Promise.race([got, sleep(ms).then(function () { return null; })]).then(function (m) { waiting = null; return m; });
  }
  function open(variant, ms) {
    return fetch('/harness/variant?v=' + variant).then(function () {
      var p = awaitReport(ms);
      extOpenUi(ID);
      return p;
    });
  }
  function panelChecks() {
    var host = document.getElementById('extframe'),
      f = host && host.querySelector('iframe'),
      label = host && host.querySelector('.extframe-label'),
      out = {};
    out.frame = !!f;
    if (f) {
      out.sandbox = f.getAttribute('sandbox');
      out.allow = f.getAttribute('allow');
      out.policy_first = (f.getAttribute('srcdoc') || f.srcdoc || '').indexOf('<meta http-equiv="Content-Security-Policy"') === 0;
      var fr = f.getBoundingClientRect(), lr = label ? label.getBoundingClientRect() : null;
      out.label_outside = !!lr && lr.bottom <= fr.top + 0.5 && !label.contains(f) && !f.contains(label);
      out.frame_in_host = host.contains(f);
      out.height = fr.height;
    }
    return fetch('/', { cache: 'no-store' }).then(function (r) {
      out.page_policy = r.headers.get('Content-Security-Policy');
      return out;
    });
  }
  function show(v) {
    var d = document.createElement('div');
    d.style.cssText = 'position:fixed;left:8px;top:8px;right:8px;z-index:10000;padding:10px;font:14px monospace;' +
      'white-space:pre-wrap;max-height:90vh;overflow:auto;color:#fff;background:' +
      (v.verdict === 'PASS' ? '#1d6b33' : v.verdict === 'FAIL' ? '#8a1c1c' : '#7a5a00');
    d.textContent = v.verdict + '  ' + v.summary + '\n\n' + JSON.stringify(v, null, 1);
    document.body.appendChild(d);
    document.title = v.verdict + ' frame isolation';
  }
  function run() {
    location.hash = '#system';
    var t0 = Date.now();
    (function wait() {
      var list = (window.EXT_LAST && EXT_LAST.packages) || [];
      if (list.some(function (p) { return p.id === ID; })) return go();
      if (Date.now() - t0 > 15000) return post('/harness/report', { variant: 'driver', error: 'the package list never loaded' });
      loadExt();
      setTimeout(wait, 500);
    })();
  }
  function go() {
    fetch('/harness/page?v=control').then(function (r) { return r.text(); }).then(function (html) {
      control = document.createElement('iframe');
      control.setAttribute('sandbox', 'allow-scripts');
      control.style.cssText = 'width:300px;height:30px;border:1px dashed #999';
      control.srcdoc = html;
      var p = awaitReport(6000);
      document.body.appendChild(control);
      return p;
    }).then(function (m) {
      control.remove();
      control = null;
      return post('/harness/report', { variant: 'control', report: m });
    }).then(function () {
      return open('main', 6000);
    }).then(function (m) {
      return panelChecks().then(function (pc) {
        return post('/harness/report', { variant: 'main', report: m }).then(function () {
          return post('/harness/report', { variant: 'panel', report: pc });
        });
      });
    }).then(function () {
      var chain = Promise.resolve();
      NAVS.filter(function (n) { return n !== 'nav-top'; }).forEach(function (n) {
        chain = chain.then(function () { return open(n, 2500); }).then(function (m) {
          return post('/harness/report', { variant: n, report: m });
        });
      });
      return chain;
    }).then(function () {
      return post('/harness/prelim', {});
    }).then(function () {
      /* Last: a top navigation that got out would take this page away. */
      return open('nav-top', 3000);
    }).then(function (m) {
      return post('/harness/report', { variant: 'nav-top', report: m });
    }).then(function () {
      return post('/harness/done', { ua: navigator.userAgent });
    }).then(function (r) { return r.json(); }).then(show);
  }
  if (document.readyState === 'complete') run();
  else window.addEventListener('load', run);
})();
</script>"""


def verdict(ua):
    """The run's verdict, from the listeners' log and what the frames reported."""
    with LOCK:
        hits = list(HITS)
        reports = dict(REPORTS)
    tags = {}
    for h in hits:
        tags.setdefault(h['tag'], h)
    main = (reports.get('main') or {}).get('seen') or {}
    control = (reports.get('control') or {}).get('seen') or {}
    failed, notes = [], {}

    control_out = [v for v in NET if 'control-' + v in tags]
    ran = bool(main)
    for v in NET:
        if 'frame-' + v in tags:
            failed.append('%s reached %s' % (v, tags['frame-' + v]['side']))
    for v in NAVS:
        if 'frame-' + v in tags:
            failed.append('%s navigated to %s' % (v, tags['frame-' + v]['side']))
        r = (reports.get(v) or {}).get('seen') or {}
        if v == 'nav-popup' and r.get(v, '').endswith('OPENED'):
            failed.append('nav-popup opened a window')
    for v in READS:
        n = main.get(v, '(no report)')
        if n.startswith('ran: ') and n not in BENIGN:
            failed.append('%s read: %s' % (v, n[:80]))
        notes[v] = n
    for v, (lead, has) in BRIDGE.items():
        n = main.get(v, '(no report)')
        notes[v] = n
        if not n.startswith(lead) or has not in n:
            failed.append('%s answered "%s", wanted %s...%s' % (v, n[:100], lead, has))
    with LOCK:
        shots = list(SNAPSHOTS)
    if FRAME_QUERY not in shots:
        failed.append('the bridge did not ask the machine for %s: it asked for %s' % (FRAME_QUERY, shots))
    for q in shots:
        if q.get('background') != '1':
            failed.append('the bridge asked for a frame that is not a background capture: %s' % q)
    notes['snapshots'] = shots
    pc = reports.get('panel') or {}
    checks = {
        'the frame opened': pc.get('frame') is True,
        'sandbox is allow-scripts alone': pc.get('sandbox') == 'allow-scripts',
        'no permissions granted to the frame': not pc.get('allow'),
        'the policy is the first element': pc.get('policy_first') is True,
        "the panel's label sits outside the frame": pc.get('label_outside') is True,
        "the panel page sends frame-src 'none'": "frame-src 'none'" in (pc.get('page_policy') or ''),
        'the frame is no taller than the panel allows': 0 < (pc.get('height') or 0) <= 1400.5,
    }
    for k, ok in checks.items():
        if not ok:
            failed.append(k + ': no (%s)' % json.dumps(pc)[:160])
    # The two residuals carry no tag of their own, so each is read by the phase it arrived in: a witness
    # that stays quiet in the control is no witness (Firefox ignores a loopback STUN server).
    residual = {
        'webrtc': {'frame': 'udp-stun-frame' in tags, 'control': 'udp-stun-control' in tags},
        'preconnect': {'frame': 'tcp-preconnect-frame' in tags, 'control': 'tcp-preconnect-control' in tags},
    }
    if not ran:
        v = 'NORESULT'
        summary = 'the hostile page never reported: nothing is proven'
    elif len(control_out) < CONTROL_MIN:
        v = 'NORESULT'
        summary = 'the control frame got only %d vectors out (%d needed): the witnesses prove nothing' % (
            len(control_out), CONTROL_MIN)
    elif failed:
        v = 'FAIL'
        summary = '%d finding(s): %s' % (len(failed), '; '.join(failed))
    else:
        v = 'PASS'
        summary = ('nothing reached the internet or the machine, no navigation left the frame, nothing of the '
                   'panel was read, the bridge answered by name (control: %d vectors out)' % len(control_out))
    return {'verdict': v, 'summary': summary, 'ua': ua, 'failed': failed, 'checks': checks,
            'control_out': control_out, 'residual': residual, 'notes': notes, 'control': control,
            'hits': hits, 'time': time.strftime('%Y-%m-%dT%H:%M:%S')}


def make_handler(port):
    class HarnessHandler(ds.Handler):
        def _route(self):
            u = urlsplit(self.path)
            q = dict(parse_qsl(u.query, keep_blank_values=True))
            if 't' in q and q['t'].startswith(('frame-', 'control-')):
                record('the machine', q['t'], u.path)
            elif u.path == '/cam/snapshot' and 't' not in q:
                # The panel's own preview carries a cache-busting t=; the
                # bridge never does, so what is left is the bridge's.
                with LOCK:
                    SNAPSHOTS.append(q)
            try:
                if u.path == '/harness' and self.command == 'GET':
                    return self._harness_page()
                if u.path == '/harness/page':
                    STATE['phase'] = 'control'
                    return self._reply(200, 'text/html; charset=utf-8',
                                       hostile_page('control', 'main', self._host(), port))
                if u.path == '/harness/variant':
                    STATE['variant'] = q.get('v', 'main')
                    STATE['phase'] = 'frame'
                    return self._reply(200, 'text/plain', 'ok')
                if u.path in ('/harness/report', '/harness/prelim', '/harness/done') and self.command == 'POST':
                    return self._harness_post(u.path)
                if u.path == '/harness/verdict':
                    return self._reply(200, 'application/json', json.dumps(STATE['verdict']))
                if u.path == '/ext/ui' and q.get('id') == HOSTILE_ID and self.command == 'GET':
                    page = hostile_page('frame', STATE['variant'], self._host(), port)
                    return self._reply(200, 'application/json',
                                       json.dumps({'ok': True, 'id': HOSTILE_ID, 'bytes': len(page), 'html': page}))
            except ConnectionError:
                return None
            return super()._route()

        def _host(self):
            return (self.headers.get('Host') or '127.0.0.1').rsplit(':', 1)[0]

        def _reply(self, code, ctype, text):
            self._send(code, {'Content-Type': ctype}, text.encode('utf-8'))

        def _harness_page(self):
            token, label = self._token_label()
            page = self.panel.render(token, label, True, self.bundled)
            drv = DRIVER.replace('__ID__', HOSTILE_ID).replace('__NAVS__', json.dumps(NAVS))
            k = page.rfind('</body>')
            page = page + drv if k < 0 else page[:k] + drv + page[k:]
            with LOCK:
                HITS.clear()
                REPORTS.clear()
                SNAPSHOTS.clear()
            STATE['verdict'] = None
            STATE['variant'] = 'main'
            STATE['phase'] = 'none'
            self._send(200, dict(ds.PAGE_HEADERS), page.encode('utf-8'))

        def _harness_post(self, path):
            try:
                body = json.loads(self._read_body() or b'{}')
            except ValueError:
                body = {}
            if path == '/harness/report':
                with LOCK:
                    REPORTS[body.get('variant', '?')] = body.get('report') or {'error': body.get('error')}
                return self._reply(200, 'text/plain', 'ok')
            if path == '/harness/prelim':
                STATE['prelim_t'] = time.time()
                threading.Thread(target=self._late_verdict, daemon=True).start()
                return self._reply(200, 'text/plain', 'ok')
            v = verdict(body.get('ua', ''))
            STATE['verdict'] = v
            DONE.set()
            return self._reply(200, 'application/json', json.dumps(v))

        @staticmethod
        def _late_verdict():
            """A top navigation that got out takes the driver with it, so the
            run is judged without its last word."""
            time.sleep(12)
            if STATE['verdict'] is None:
                STATE['verdict'] = verdict('(the page went away before it filed its verdict)')
                DONE.set()

    return HarnessHandler


class Internet(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        self.do_GET()

    def do_GET(self):
        u = urlsplit(self.path)
        t = dict(parse_qsl(u.query)).get('t', '')
        if t:
            record('the internet', t, u.path)
        body = b"<body style='background:#c00;color:#fff;font:20px sans-serif'>THE INTERNET: this left the browser</body>"
        self.send_response(200)
        self.send_header('Content-Type', 'text/html')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def stun_listener(bind, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((bind, port))
    while True:
        s.recvfrom(2048)
        record('STUN over UDP', 'udp-stun-' + STATE['phase'], 'stun')


def tcp_listener(bind, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((bind, port))
    s.listen(8)
    while True:
        c, _ = s.accept()
        c.close()
        record('a bare TCP connection', 'tcp-preconnect-' + STATE['phase'], 'connect')


def main():
    ap = argparse.ArgumentParser(description="exthost.ui-frame-isolation: a hostile package page in the panel's frame")
    ap.add_argument('--port', type=int, default=8091)
    ap.add_argument('--lan', action='store_true', help='bind every address (Firefox needs it for the WebRTC reading)')
    ap.add_argument('--once', action='store_true', help='exit with the verdict once the run files it')
    ap.add_argument('--out', help='write the verdict as JSON here')
    ap.add_argument('--timeout', type=float, default=90.0, help='with --once, how long to wait for a run')
    args = ap.parse_args()
    bind = '0.0.0.0' if args.lan else '127.0.0.1'

    cfg_args = argparse.Namespace(mock=True, host='', token='', env=os.devnull, port=args.port, bind=bind)
    mock = ds.Mock(ds.MOCK_TOKEN)
    mock.settings['ext_enabled'] = '1'
    mock.ext_packages.append({
        'id': HOSTILE_ID, 'version': '1.0.0', 'previous': '', 'tier': 'unverified', 'key': '', 'enabled': True,
        'quarantined': False, 'grants': [], 'account': 'ffx9',
        'package': {'id': HOSTILE_ID, 'name': 'Hostile page', 'version': '1.0.0', 'author': 'forgetest',
                    'license': 'MIT', 'description': 'Tries every way out of its frame.', 'runtime': 'ui',
                    'capabilities': ['ui', 'machine.read', 'settings.own', 'camera.lid'], 'modes': ['grbl', 'cloud']}})
    handler = make_handler(args.port)
    handler.cfg = ds.Config(cfg_args)
    handler.panel = ds.Panel(ds.UI_DIR)
    handler.mock = mock
    handler.verbose = False
    handler.bundled = True             # the page as the daemon serves it: one document, inlined

    servers = [ThreadingHTTPServer((bind, args.port), handler), ThreadingHTTPServer((bind, args.port + 1), Internet)]
    for s in servers:
        s.daemon_threads = True
    threading.Thread(target=servers[1].serve_forever, daemon=True).start()
    threading.Thread(target=stun_listener, args=(bind, args.port + 2), daemon=True).start()
    threading.Thread(target=tcp_listener, args=(bind, args.port + 3), daemon=True).start()
    threading.Thread(target=servers[0].serve_forever, daemon=True).start()
    print('frame isolation harness: http://%s:%d/harness' % ('127.0.0.1' if not args.lan else '<this PC>', args.port),
          flush=True)

    if not args.once:
        try:
            while True:
                DONE.wait()
                v = STATE['verdict']
                print('%s  %s' % (v['verdict'], v['summary']), flush=True)
                if args.out:
                    with open(args.out, 'w', encoding='utf-8') as f:
                        json.dump(v, f, indent=1)
                DONE.clear()
        except KeyboardInterrupt:
            return 0
    if not DONE.wait(args.timeout):
        print('NORESULT  no run filed a verdict within %.0f s' % args.timeout, flush=True)
        return 2
    v = STATE['verdict']
    print('%s  %s' % (v['verdict'], v['summary']), flush=True)
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            json.dump(v, f, indent=1)
    return {'PASS': 0, 'FAIL': 1}.get(v['verdict'], 2)


if __name__ == '__main__':
    sys.exit(main())
