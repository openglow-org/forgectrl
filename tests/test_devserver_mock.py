#!/usr/bin/env python3
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# https://community.openglow.org
# SPDX-License-Identifier:    MIT
"""Host test: the panel dev server's mock mirrors the daemon.

The mock in tools/devserver.py is a static copy of what forgectrl
serves. This test reads the daemon's own tables with a regex each (the
gate table in src/gates.c, the settings table and the endpoint list in
src/main.c, the format strings of the JSON builders, the routes the
panel calls in src/ui/*.js) and holds the mock to them, so a table
edit that misses the mock fails here instead of on a developer's
screen.

Run: python3 -B -m unittest -v tests/test_devserver_mock.py
"""
import hashlib
import importlib.util
import json
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8') as f:
        return f.read()


def load_devserver():
    spec = importlib.util.spec_from_file_location(
        'devserver', os.path.join(ROOT, 'tools', 'devserver.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# -- the daemon's tables, read from the C sources
GATE_ROW = re.compile(
    r'\{\s*"(?P<key>\w+)",\s*(?:"(?P<gate>\w+)"|NULL),\s*'
    r'(?P<def>[-\d.]+),\s*(?P<lo>[-\d.]+),\s*(?P<hi>[-\d.]+),\s*'
    r'(?P<blo>[-\d.]+),\s*(?P<bhi>[-\d.]+),\s*(?P<off>[-+]?\d)\s*\}')
OFF_END = {'-1': 'low', '+1': 'high', '1': 'high', '0': 'none'}


def gate_table():
    """(key, gate, def, lo, hi, band_lo, band_hi, off) per row of the
    table in gates.c, in table order."""
    m = re.search(r'gate_setting_t table\[\] = \{(.*?)\n\};',
                  read('src/gates.c'), re.S)
    rows = []
    for r in GATE_ROW.finditer(m.group(1)):
        rows.append((r.group('key'), r.group('gate'),
                     float(r.group('def')), float(r.group('lo')),
                     float(r.group('hi')), float(r.group('blo')),
                     float(r.group('bhi')), OFF_END[r.group('off')]))
    return rows


SETTING_ROW = re.compile(
    r'\{\s*"(?P<key>\w+)",\s*\w+,\s*(?P<secret>[01])\s*\}')


def settings_table():
    """(key, secret) per row of setting_defs in main.c, in table order."""
    m = re.search(r'\} setting_defs\[\] = \{(.*?)\n\};', read('src/main.c'),
                  re.S)
    return [(r.group('key'), r.group('secret') == '1')
            for r in SETTING_ROW.finditer(m.group(1))]


def endpoints():
    """(method, path) per row of the routes[] table in main.c."""
    m = re.search(r'struct route routes\[\] = \{(.*?)\n    \};', read('src/main.c'),
                  re.S)
    return re.findall(r'\{\s*"(GET|POST)",\s*"([^"]+)"', m.group(1))


def panel_routes():
    """The API paths the panel's JavaScript calls (a quoted literal that
    starts with a slash, at a fetch/fx/startJob call or as a stream
    source), less the diag prefix it completes at run time."""
    paths = set()
    for name in os.listdir(os.path.join(ROOT, 'src', 'ui')):
        if not name.endswith('.js'):
            continue
        js = read(os.path.join('src', 'ui', name))
        for p in re.findall(r"(?:fetch|fx|startJob)\('(/[^'?]*)", js):
            paths.add(p)
        for p in re.findall(r"src = '(/[^'?]*)", js):
            paths.add(p)
    return sorted(p for p in paths if p != '/' and not p.endswith('/'))


def c_keys(src, func=None):
    """Every JSON key a C source (or one function of it) writes: the
    \\"name\\": tokens of its format strings."""
    if func:
        m = re.search(r'^[\w \*]*\b%s\(' % re.escape(func), src, re.M)
        end = src.index('\n}\n', m.start())
        src = src[m.start():end]
    return set(re.findall(r'\\"(\w+)\\":', src))


def doc_keys(obj):
    """Every key at any depth of a JSON document."""
    out = set()
    if isinstance(obj, dict):
        for k, v in obj.items():
            out.add(k)
            out |= doc_keys(v)
    elif isinstance(obj, list):
        for v in obj:
            out |= doc_keys(v)
    return out


class MockTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ds = load_devserver()
        cls.token = cls.ds.MOCK_TOKEN

    def mock(self):
        return self.ds.Mock(self.token)

    def call(self, mock, method, path, q=None, body=b'', token=True):
        headers = {'Content-Type': 'application/x-www-form-urlencoded'}
        if token:
            headers['X-ForgeFIRM-Token'] = self.token
        return mock.handle(method, path, q or {}, headers, body)

    def get_json(self, mock, path, q=None):
        code, hdrs, body = self.call(mock, 'GET', path, q)
        self.assertEqual(code, 200, path)
        self.assertEqual(hdrs['Content-Type'], 'application/json', path)
        return json.loads(body)

    # -- the gate table
    def test_gate_table_matches_gates_c(self):
        table = gate_table()
        self.assertGreater(len(table), 0, 'gates.c table parsed')
        self.assertEqual(list(self.ds.Mock.GATES), table)

    def test_gate_state_rule(self):
        for row in self.ds.Mock.GATES:
            key, gate, default, lo, hi, blo, bhi, off = row
            st = self.ds.Mock.gate_state
            self.assertEqual(st(row, default), 'ok', key)
            if off == 'low':
                self.assertEqual(st(row, lo), 'off', key)
            elif off == 'high':
                self.assertEqual(st(row, hi), 'off', key)
            if blo > lo:
                self.assertEqual(st(row, blo - 0.001), 'warn', key)
            if bhi < hi:
                self.assertEqual(st(row, bhi + 0.001), 'warn', key)

    def test_settings_gates_block(self):
        m = self.mock()
        gates = self.get_json(m, '/settings')['gates']
        self.assertEqual(list(gates), [r[0] for r in self.ds.Mock.GATES])
        keys = c_keys(read('src/gates.c'), 'gates_json')
        for key, g in gates.items():
            self.assertEqual(set(g), keys, key)
        # A value at the off end is reported off, and lands in gates_off
        # on /status and /cool/status the way the engine reports it.
        m.settings['cool_flow_check_s'] = '0'
        self.assertEqual(self.get_json(m, '/settings')['gates']
                         ['cool_flow_check_s']['state'], 'off')
        self.assertIn('flow', self.get_json(m, '/status')['gates_off'])
        self.assertIn('flow', self.get_json(m, '/cool/status')['gates_off'])

    # -- the settings key set
    def test_settings_keys_match_main_c(self):
        table = settings_table()
        self.assertGreater(len(table), 0, 'main.c setting_defs parsed')
        self.assertEqual(list(self.ds.SETTINGS_KEYS), [k for k, _ in table])
        self.assertEqual(list(self.ds.SECRET_KEYS),
                         [k for k, secret in table if secret])
        expect = [k + '_set' if secret else k for k, secret in table]
        expect += ['gates', 'version', 'machine_id', 'tls_fingerprint']
        reply = self.get_json(self.mock(), '/settings')
        self.assertEqual(list(reply), expect)
        for k, secret in table:
            self.assertIsInstance(reply[k + '_set' if secret else k],
                                  bool if secret else str, k)

    def test_settings_post(self):
        m = self.mock()
        code, hdrs, body = self.call(m, 'POST', '/settings',
                                     {'cool_temp_max': '35'})
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)['cool_temp_max'], '35')
        code, hdrs, body = self.call(m, 'POST', '/settings',
                                     {'cool_temp_max': '999'})
        self.assertEqual((code, hdrs['Content-Type'], body),
                         (400, 'text/plain',
                          b'invalid value for cool_temp_max'))
        code, hdrs, body = self.call(m, 'POST', '/settings', {'bogus': '1'})
        self.assertEqual((code, body), (400, b'no known setting in request'))
        code, hdrs, body = self.call(m, 'POST', '/settings',
                                     {'cool_temp_max': '35'}, token=False)
        self.assertEqual(code, 403)
        self.assertEqual(json.loads(body),
                         {'error': 'authentication required'})

    def test_settings_cloud_enabled_is_the_cloud_steps_decision(self):
        # as main.c rules it: on from off takes the typed phrase; off
        # sweeps the cloud homing and the cloud boot mode
        m = self.mock()
        m.settings.update({'cloud_enabled': '0', 'homing_mode': 'none',
                           'controller_mode': 'grbl'})
        code, hdrs, body = self.call(m, 'POST', '/settings',
                                     {'cloud_enabled': '1'})
        self.assertEqual((code, body),
                         (400, b'type I UNDERSTAND to turn cloud mode on'))
        self.assertEqual(m.settings['cloud_enabled'], '0')
        code, hdrs, body = self.call(m, 'POST', '/settings',
                                     {'cloud_enabled': '1',
                                      'phrase': 'I UNDERSTAND'})
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)['cloud_enabled'], '1')
        m.settings.update({'homing_mode': 'gfcloud', 'controller_mode': 'cloud'})
        code, hdrs, body = self.call(m, 'POST', '/settings',
                                     {'cloud_enabled': '0'})
        self.assertEqual(code, 200)
        reply = json.loads(body)
        self.assertEqual((reply['cloud_enabled'], reply['homing_mode'],
                          reply['controller_mode']), ('0', 'none', 'grbl'))

    def test_settings_ext_enabled_is_turned_on_over_its_advisory(self):
        # as main.c rules it: on from off takes the hash of the Extensions
        # advisory as it stands and the typed phrase, and records both;
        # the first-run acceptance refuses the document
        main = read('src/main.c')
        adv = read('src/advisories.c')
        self.assertIn('{ "extensions", "Extensions", "typed", "I UNDERSTAND", NULL, 0, "", 1 }', adv)
        self.assertEqual(self.ds.ON_DEMAND_DOCS, ('extensions',))
        m = self.mock()
        self.assertNotIn('extensions', [d['id'] for d in m.wiz_reply()['documents']])
        code, hdrs, text = self.call(m, 'GET', '/advisories/extensions')
        self.assertEqual(code, 200)
        digest = hashlib.sha256(text).hexdigest()
        self.assertEqual(hdrs['ETag'], digest)
        for form, want in (
                ({'ext_enabled': '1'},
                 (409, b'read the Extensions advisory first: turning extensions on agrees to it')),
                ({'ext_enabled': '1', 'advisory': '0' * 64, 'phrase': 'I UNDERSTAND'},
                 (409, b'read the Extensions advisory first: turning extensions on agrees to it')),
                ({'ext_enabled': '1', 'advisory': digest},
                 (400, b'type I UNDERSTAND to turn extensions on')),
                ({'ext_enabled': '1', 'advisory': digest, 'phrase': 'i understand'},
                 (400, b'type I UNDERSTAND to turn extensions on'))):
            code, hdrs, body = self.call(m, 'POST', '/settings', form)
            self.assertEqual((code, body), want, form)
            self.assertIn(want[1].decode(), main)
            self.assertNotEqual(m.settings.get('ext_enabled'), '1')
            self.assertNotIn('on_demand', m.wiz_record())
        # a good consent beside a write the daemon refuses records nothing
        code, hdrs, body = self.call(m, 'POST', '/settings',
                                     {'ext_enabled': '1', 'advisory': digest,
                                      'phrase': 'I UNDERSTAND', 'cloud_enabled': '0',
                                      'controller_mode': 'cloud'})
        self.assertEqual(code, 409)
        self.assertNotEqual(m.settings.get('ext_enabled'), '1')
        self.assertNotIn('on_demand', m.wiz_record())
        code, hdrs, body = self.call(m, 'POST', '/settings',
                                     {'ext_enabled': '1', 'advisory': digest,
                                      'phrase': 'I UNDERSTAND'})
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)['ext_enabled'], '1')
        self.assertEqual(m.wiz_record()['on_demand']['extensions']['hash'], digest)
        # re-sending 1 while it stands, and turning it off, ask nothing
        for form in ({'ext_enabled': '1'}, {'ext_enabled': '0'}):
            code, hdrs, body = self.call(m, 'POST', '/settings', form)
            self.assertEqual(code, 200, form)
        code, hdrs, body = self.call(m, 'POST', '/wiz/advisories/accept',
                                     {'doc': 'extensions', 'hash': digest,
                                      'phrase': 'I UNDERSTAND'})
        self.assertEqual((code, json.loads(body)),
                         (400, {'error': 'this document is accepted where its feature is turned on'}))
        self.assertIn('this document is accepted where its feature is turned on', read('src/wiz.c'))

    def test_ext_routes_match_extpkg_c(self):
        # as extpkg.c rules it: a closed list of actions, a package id in
        # the form of one, the host's refusal in its words (409), and the
        # status document's four keys
        src = read('src/extpkg.c')
        actions = re.findall(r'\{ "([a-z-]+)",\s+\{ "', src)
        self.assertEqual(tuple(actions), self.ds.Mock.EXT_ACTIONS)
        m = self.mock()
        code, hdrs, body = self.call(m, 'GET', '/ext/status', token=False)
        self.assertEqual(code, 403)
        doc = self.get_json(m, '/ext/status')
        self.assertEqual(set(doc), {'enabled', 'safe_mode', 'host', 'packages', 'keys'})
        for key in ('"enabled"', '"safe_mode"', '"host"', '"packages"', '"keys"', '"running"'):
            self.assertIn(key, src)
        self.assertEqual([p['id'] for p in doc['packages']],
                         ['org.openglow.notify', 'org.example.badge', 'org.example.panel'])
        # The one with an interface: the route serves it, and a package
        # without one is a 404 rather than an empty page.
        code, _h, body = self.call(m, 'GET', '/ext/ui', q={'id': 'org.example.panel'})
        self.assertEqual(code, 200)
        doc2 = json.loads(body)
        self.assertTrue(doc2['ok'] and doc2['bytes'] > 0 and doc2['html'].startswith('<!doctype html>'))
        code, _h, _b = self.call(m, 'GET', '/ext/ui', q={'id': 'org.example.badge'})
        self.assertEqual(code, 404)
        for form, want in (({'id': 'org.example.badge', 'action': 'install'}, 400), ({'id': 'badge', 'action': 'enable'}, 400),
                           ({'id': 'org.example;x', 'action': 'enable'}, 400), ({'id': 'org.example.none', 'action': 'enable'}, 409),
                           ({'id': 'org.openglow.notify', 'action': 'hold-required'}, 409)):
            code, hdrs, body = self.call(m, 'POST', '/ext/package', form)
            self.assertEqual(code, want, form)
        for words in (b'id is a package id', b'action is enable, disable, remove, remove-keep-data, hold-required, or hold-advisory'):
            self.assertIn(words.decode(), src)
        code, hdrs, body = self.call(m, 'POST', '/ext/package', {'id': 'org.example.badge', 'action': 'hold-required'})
        self.assertEqual(code, 200)
        self.assertEqual([p.get('hold') for p in json.loads(body)['packages']], [None, 'required', None])
        code, hdrs, body = self.call(m, 'POST', '/ext/package', {'id': 'org.example.badge', 'action': 'disable'})
        self.assertEqual([p['enabled'] for p in json.loads(body)['packages']], [True, False, True])
        code, hdrs, body = self.call(m, 'POST', '/ext/package', {'id': 'org.example.badge', 'action': 'remove'})
        self.assertEqual([p['id'] for p in json.loads(body)['packages']],
                         ['org.openglow.notify', 'org.example.panel'])
        code, hdrs, body = self.call(m, 'POST', '/ext/package', {'id': 'org.openglow.notify', 'action': 'disable'}, token=False)
        self.assertEqual(code, 403)

    def test_ext_install_matches_extpkg_c(self):
        # as extpkg.c and main.c rule it: upload, the host's inspect with the
        # consent its tier takes, then the install with the grants and the
        # consent; the tier is never the request's to say
        src = read('src/extpkg.c')
        m = self.mock()
        code, hdrs, body = self.call(m, 'POST', '/ext/install', {'phrase': 'I UNDERSTAND', 'grants': 'hold'})
        self.assertEqual((code, body), (409, b'no package is staged: upload one first'))
        self.assertIn(body.decode(), src)
        code, hdrs, body = self.call(m, 'POST', '/ext/upload', body=b'')
        self.assertEqual(code, 400)
        code, hdrs, body = self.call(m, 'POST', '/ext/upload', body=b'an archive')
        doc = json.loads(body)
        self.assertEqual((code, doc['tier'], doc['consent'], doc['needs_grant']), (200, 'community', 'typed', ['hold']))
        for word in ('"consent"', '"login"', '"typed"', '"button"'):
            self.assertIn(word, src)
        for form, want in (({'grants': 'hold'}, 400), ({'grants': 'hold', 'phrase': 'i understand'}, 400),
                           ({'phrase': 'I UNDERSTAND'}, 409), ({'phrase': 'I UNDERSTAND', 'grants': 'hold;reboot'}, 400),
                           ({'phrase': 'I UNDERSTAND', 'grants': 'a,b,c,d,e,f,g,h,i'}, 400)):
            code, hdrs, body = self.call(m, 'POST', '/ext/install', form)
            self.assertEqual(code, want, form)
        self.assertIn('type " EXTPKG_PHRASE " to install it', src)
        self.assertIn('grants is a comma-separated list of at most %d capability names', src)
        code, hdrs, body = self.call(m, 'POST', '/ext/install', {'phrase': 'I UNDERSTAND', 'grants': 'hold'})
        self.assertEqual(code, 200)
        self.assertIn('org.example.filter', [p['id'] for p in json.loads(body)['packages']])
        code, hdrs, body = self.call(m, 'POST', '/ext/install', {'phrase': 'I UNDERSTAND', 'grants': 'hold'})
        self.assertEqual(code, 409)                 # the staged file went with the install
        code, hdrs, body = self.call(m, 'POST', '/ext/upload/discard')
        self.assertEqual((code, json.loads(body)), (200, {'discarded': True}))
        code, hdrs, body = self.call(m, 'POST', '/ext/upload', body=b'an archive', token=False)
        self.assertEqual(code, 403)

    def test_ext_keys_match_extpkg_c(self):
        # as extpkg.c rules it: a name's form, and the button held
        src = read('src/extpkg.c')
        m = self.mock()
        doc = self.get_json(m, '/ext/status')
        self.assertEqual([k['name'] for k in doc['keys']], ['a-maker'])
        for form, want in (({'name': 'a maker', 'key': 'AAAA'}, 400), ({'name': '../x', 'key': 'AAAA'}, 400),
                           ({'name': '.hidden', 'key': 'AAAA'}, 400), ({'name': 'maker'}, 400),
                           ({'name': 'maker', 'key': 'AAAA'}, 409)):
            code, hdrs, body = self.call(m, 'POST', '/ext/key', form)
            self.assertEqual(code, want, form)
        self.assertIn("hold the machine's button while you add it", src)
        self.assertIn("a key's name is letters, digits, dash, underscore, and dot, at most 48 bytes", src)
        code, hdrs, body = self.call(m, 'POST', '/ext/key/remove', {'name': 'nothere'})
        self.assertEqual(code, 409)
        code, hdrs, body = self.call(m, 'POST', '/ext/key/remove', {'name': 'a-maker'})
        self.assertEqual((code, json.loads(body)['keys']), (200, []))
        code, hdrs, body = self.call(m, 'POST', '/ext/key', {'name': 'maker', 'key': 'AAAA'}, token=False)
        self.assertEqual(code, 403)

    # -- the mode vocabulary
    def test_mode_matches_super_c(self):
        src = read('src/super.c')
        switch = re.search(r'int super_mode_switch\(.*?\n\}', src, re.S)
        accepted = re.findall(r'!strcmp\(mode, "(\w+)"\)', switch.group(0))
        self.assertEqual(sorted(accepted), ['cloud', 'grbl'])
        m = self.mock()
        doc = self.get_json(m, '/mode')
        self.assertEqual(set(doc), c_keys(src, 'super_status_json'))
        states = set(re.findall(r'"(running|motion-fault|waiting|gated|standby|stopped)"',
                                src))
        self.assertIn(doc['controller'], states)
        for mode in accepted:
            code, hdrs, body = self.call(m, 'POST', '/mode',
                                         {'controller': mode})
            self.assertEqual(code, 200, mode)
            self.assertEqual(json.loads(body)['mode'], mode)
            self.assertEqual(self.get_json(m, '/settings')['controller_mode'],
                             mode)
        code, hdrs, body = self.call(m, 'POST', '/mode',
                                     {'controller': 'gfcloud'})
        self.assertEqual((code, body), (400, b'mode must be grbl or cloud'))
        code, hdrs, body = self.call(m, 'POST', '/mode')
        self.assertEqual((code, body), (400, b'controller is required'))

    # -- the document shapes, against the C format strings
    def test_cool_status_shape(self):
        src = read('src/coolfmt.c')
        keys = (c_keys(src, 'coolfmt_status') | c_keys(src, 'coolfmt_limits')
                | c_keys(src, 'coolfmt_fan_gates'))
        fans = re.search(r'fan_name\[Fan_N\] = \{([^}]*)\}',
                         read('src/cool.c')).group(1)
        keys |= set(re.findall(r'"(\w+)"', fans))
        doc = self.get_json(self.mock(), '/cool/status')
        self.assertEqual(doc_keys(doc), keys)
        self.assertEqual(set(doc), c_keys(src, 'coolfmt_status'))
        self.assertEqual(list(doc['fan_gates']),
                         re.findall(r'"(\w+)"', fans))

    def test_status_shape(self):
        # gates_off is spliced in by the route, and lease by lease.c
        # (test_lease_shape holds that one to its source).
        keys = c_keys(read('src/status.c')) | {'gates_off', 'lease'}
        doc = self.get_json(self.mock(), '/status')
        lease = doc_keys(doc.get('lease'))
        self.assertTrue(keys <= doc_keys(doc), keys - doc_keys(doc))
        self.assertTrue(set(doc) <= keys, set(doc) - keys)
        self.assertTrue(lease, 'no lease block in /status')
        report = doc['grbl']['report']
        ctl = os.path.join(ROOT, '..', 'grblHAL-glowforge', 'src',
                           'glowforge_status.c')
        if os.path.isfile(ctl):
            with open(ctl, encoding='utf-8') as f:
                self.assertEqual(doc_keys(report), c_keys(f.read()))

    def test_lease_shape(self):
        """The machine lease in /status, its words, and its refusals, held
        to src/lease.c."""
        src = read('src/lease.c')
        m = self.mock()
        rest = self.get_json(m, '/status')['lease']
        self.assertIsNone(rest['holder'])
        self.assertEqual(set(rest['observed']), {'sender', 'motors_released'})
        # the words table, row for row
        table = re.findall(r'\{ "(\w+:)",\s+"([^"]+)" \}', src)
        self.assertEqual(tuple(table), tuple(m.LEASE_WORDS))
        self.assertIn('"the dose-curve recorder"', src)
        self.assertIn('"%s holds the machine"', src)
        # a holder: every key lease_json writes, but under, which needs a
        # nested owner
        code, hdrs, body = self.call(m, 'POST', '/diag/flow-verify')
        self.assertEqual(code, 202)
        held = self.get_json(m, '/status')['lease']
        self.assertEqual(doc_keys({'lease': held}) | {'under'}, c_keys(src, 'lease_json'))
        self.assertEqual((held['holder']['owner'], held['holder']['kind'], held['holder']['words']),
                         ('diag:flow-verify', 'hardware', 'a diagnostic (flow-verify)'))
        # and it refuses, in the daemon's words, wherever the daemon asks it
        want = 'a diagnostic (flow-verify) holds the machine'
        for path, form, shape in (('/mode', {'controller': 'grbl'}, 'text'),
                                  ('/controller/start', {}, 'text'),
                                  ('/curve/record', {}, 'text'),
                                  ('/boot', {'target': 'b'}, 'json'),
                                  ('/system/reboot', {'confirm': '1'}, 'json')):
            code, hdrs, body = self.call(m, 'POST', path, form)
            got = json.loads(body)['error'] if shape == 'json' else body.decode()
            self.assertEqual((code, got), (409, want), path)
        code, hdrs, body = self.call(m, 'POST', '/settings', {'ui_units': 'metric'})
        self.assertEqual((code, body.decode()), (409, want + ' - settings are locked'))

    def test_diag_shape_and_tools(self):
        src = read('src/diag.c')
        m = self.mock()
        doc = self.get_json(m, '/diag/status')
        self.assertEqual(set(doc), c_keys(src, 'diag_status_json'))
        tools = set(re.findall(r'!strcmp\(tool, "([\w-]+)"\)', src))
        self.assertEqual(set(self.ds.DIAG_TOOLS), tools)
        for tool in tools:
            code, hdrs, body = self.call(m, 'POST', '/diag/' + tool)
            self.assertEqual((code, json.loads(body)),
                             (202, {'started': True}), tool)
            self.assertTrue(self.get_json(m, '/status')['diag'])
            code, hdrs, body = self.call(m, 'POST', '/diag/' + tool)
            self.assertEqual((code, body),
                             (409, b'a diagnostic is already running'))
            code, hdrs, body = self.call(m, 'POST', '/diag/abort')
            self.assertEqual(json.loads(body), {'aborting': True})
            doc = self.get_json(m, '/diag/status')
            self.assertFalse(doc['running'])
            self.assertEqual(doc['result'], {'error': 'aborted by operator'})
        code, hdrs, body = self.call(m, 'POST', '/diag/bogus')
        self.assertEqual((code, body), (400, b'unknown diagnostic'))
        for tool, result in self.ds.DIAG_RESULTS.items():
            self.assertIn(tool, tools)
            self.assertTrue(doc_keys(result) <= c_keys(src),
                            doc_keys(result) - c_keys(src))

    def test_cam_status_shape(self):
        keys = c_keys(read('src/main.c'), 'cb_status')
        self.assertEqual(doc_keys(self.get_json(self.mock(), '/cam/status')),
                         keys)

    def test_update_shapes(self):
        src = read('src/update.c')
        m = self.mock()
        self.assertTrue(c_keys(src, 'cb_slots') <=
                        doc_keys(self.get_json(m, '/slots')))
        self.assertEqual(set(self.get_json(m, '/update/status')),
                         c_keys(src, 'cb_update_status'))
        # The release check's reply is packed by the encoder in
        # reply_release: its keys are the quoted names of the pack call.
        rel = re.search(r'^[\w \*]*\breply_release\(', src, re.M)
        rel_body = src[rel.start():src.index('\n}\n', rel.start())]
        rel_keys = set(re.findall(r'"(\w+)",', rel_body))
        self.assertTrue(rel_keys)
        for method, path, form in (('POST', '/update/check', None),
                                   ('GET', '/update/release', None),
                                   ('POST', '/update/dismiss', {'version': 'v0.0.0'}),
                                   ('POST', '/update/dismiss', {'version': ''})):
            code, hdrs, body = self.call(m, method, path, form)
            self.assertEqual((path, code, set(json.loads(body))), (path, 200, rel_keys))
        code, hdrs, body = self.call(m, 'POST', '/update/dismiss', {'version': 'v0.0.1;rm'})
        self.assertEqual((code, json.loads(body)),
                         (400, {'error': 'version is not a release tag'}))
        code, hdrs, body = self.call(m, 'POST', '/boot', {'target': 'c'})
        self.assertEqual(json.loads(body),
                         {'error': 'target must be sd, a, b, or legacy'})
        code, hdrs, body = self.call(m, 'POST', '/system/reboot')
        self.assertEqual((code, json.loads(body)),
                         (400, {'error': 'confirm=1 required'}))
        code, hdrs, body = self.call(m, 'POST', '/update/download')
        self.assertEqual((code, json.loads(body)), (202, {'started': True}))
        code, hdrs, body = self.call(m, 'POST', '/update/download')
        self.assertEqual((code, json.loads(body)),
                         (409, {'error': 'an update job is already running'}))
        code, hdrs, body = self.call(m, 'POST', '/boot', {'target': 'b'})
        self.assertEqual((code, json.loads(body)['error'].split(' (')[0] +
                          json.loads(body)['error'].split(')')[-1]),
                         (409, 'an update job holds the machine'))
        # an update job locks the settings like any other holder
        code, hdrs, body = self.call(m, 'POST', '/settings', {'ui_units': 'metric'})
        self.assertEqual(code, 409)
        self.assertTrue(body.decode().startswith('an update job ('), body)
        self.assertTrue(body.decode().endswith('holds the machine - settings are locked'), body)

    def test_logs_shapes(self):
        src = read('src/logs.c')
        m = self.mock()
        code, hdrs, body = self.call(m, 'GET', '/logs', token=False)
        self.assertEqual(code, 403)
        self.assertEqual(doc_keys(self.get_json(m, '/logs')),
                         c_keys(src, 'logs_list_json'))
        self.assertEqual(set(self.get_json(m, '/logs/tail',
                                           {'name': 'forgectrl'})),
                         c_keys(src, 'logs_tail_json'))
        self.assertEqual(self.ds.LOGGERS,
                         tuple(re.findall(r'"(\w+)"', re.search(
                             r'logs_names\[\] = \{([^}]*)\}', src).group(1))))
        self.assertEqual(self.ds.LOG_LEVELS,
                         tuple(re.findall(r'"(\w+)"', re.search(
                             r'level_names\[\] = \{([^}]*)\}', src).group(1))))

    def test_curve_shape(self):
        keys = c_keys(read('src/curverec.c'), 'curverec_status_json')
        m = self.mock()
        self.assertTrue(set(self.get_json(m, '/curve/status')) <= keys)
        code, hdrs, body = self.call(m, 'POST', '/curve/record')
        self.assertEqual(code, 200)
        self.assertEqual(doc_keys(json.loads(body)) - keys, set())
        code, hdrs, body = self.call(m, 'POST', '/curve/stop')
        self.assertEqual(doc_keys(json.loads(body)),
                         keys - {'density', 'light'})

    def test_job_shape(self):
        keys = c_keys(read('src/jobrun.c'), 'jobrun_status_json')
        m = self.mock()
        self.assertEqual(doc_keys(self.get_json(m, '/job')), keys)

        def post(program, name=b'test', extra=b''):
            mark = b'xXxBoundaryxXx'
            body = b''.join([
                b'--', mark, b'\r\nContent-Disposition: form-data; name="name"\r\n\r\n', name,
                b'\r\n', extra,
                b'--', mark, b'\r\nContent-Disposition: form-data; name="program"; '
                b'filename="job.gcode"\r\nContent-Type: text/plain\r\n\r\n', program,
                b'\r\n--', mark, b'--\r\n'])
            return m.handle('POST', '/job', {}, {
                'X-ForgeFIRM-Token': self.token,
                'Content-Type': 'multipart/form-data; boundary=%s' % mark.decode()}, body)

        # The offenses of jobrun_program_check(), in its words.
        src = read('src/jobrun.c')
        for program, words in ((b'G21\n$X\n', 'is a $ command'),
                               (b'G1 X1!\n', 'has a realtime character'),
                               (b'G1 X\x9e1\n', 'has a byte that is not printable ASCII'),
                               (b'(nothing)\n', 'the program has no lines')):
            code, hdrs, body = post(program)
            self.assertEqual(code, 400, program)
            self.assertIn(words, body.decode())
            self.assertIn(words, src)
        code, hdrs, body = post(b'G21\n', name=b'not a name')
        self.assertEqual(code, 400)

        code, hdrs, body = post(b'(header!)\nG21 ; metric\nG1 X5 F600\n')
        self.assertEqual(code, 200, body)
        doc = json.loads(body)
        self.assertEqual(doc_keys(doc), keys)
        self.assertEqual((doc['state'], doc['owner'], doc['program'], doc['lines']),
                         ('running', 'job:test', True, 2))
        lease = self.get_json(m, '/status')['lease']['holder']
        self.assertEqual((lease['owner'], lease['kind'], lease['words']),
                         ('job:test', 'sender', 'a job (test)'))
        code, hdrs, body = post(b'G21\n')
        self.assertEqual((code, body.decode()), (409, 'a job (test) holds the machine'))
        # Whoever holds the machine moves it alone; the cancel is always taken.
        code, hdrs, body = self.call(m, 'POST', '/motion/jog', {'x': '1'})
        self.assertEqual((code, body.decode()), (409, 'a job (test) holds the machine'))
        code, hdrs, body = self.call(m, 'POST', '/motion/cancel')
        self.assertEqual(code, 200)
        code, hdrs, body = self.call(m, 'POST', '/job/abort')
        self.assertEqual(code, 200)
        doc = json.loads(body)
        self.assertEqual((doc['state'], doc['reason']), ('failed', 'aborted'))
        self.assertIsNone(self.get_json(m, '/status')['lease']['holder'])
        code, hdrs, body = self.call(m, 'POST', '/job/abort')
        self.assertEqual((code, body.decode()), (409, 'no job is running'))
        self.assertIn('no job is running', src)

    def test_extensions_match_builtin_c(self):
        src = read('src/builtin.c')
        m = self.mock()
        doc = self.get_json(m, '/extensions')
        self.assertEqual(doc_keys(doc), c_keys(src, 'builtin_json'))
        # The table, read out of the C: every string literal of the roles
        # array and of the entry, in order.
        roles = re.search(r'cloud_roles\[\] = \{(.*?)\n\};', src, re.S).group(1)
        rows = [re.findall(r'"([^"]*)"', row) for row in re.findall(r'\{(.*?)\}', roles, re.S)]
        mock_rows = [[r[k] for k in ('role', 'provider', 'kind', 'select_key', 'fallback')]
                     for r in self.ds.Mock.BUILTIN_EXT[0]['roles']]
        self.assertEqual([r[:5] for r in rows], mock_rows)
        refusals = [r[5] for r in rows]
        self.assertEqual(refusals, [r['refusal'] for r in self.ds.Mock.BUILTIN_EXT[0]['roles']])
        keys = re.findall(r'"([^"]*)"', re.search(r'cloud_settings\[\] = \{(.*?)\};', src, re.S).group(1))
        self.assertEqual(keys, self.ds.Mock.BUILTIN_EXT[0]['settings'])
        entry = re.search(r'builtin_ext\[\] = \{\s*\{(.*?)cloud_settings', src, re.S).group(1)
        lits = [''.join(re.findall(r'"([^"]*)"', part)) for part in re.split(r',\s*\n', entry) if '"' in part]
        e = self.ds.Mock.BUILTIN_EXT[0]
        self.assertEqual(lits, ['%s%s' % (e['id'], e['name']), e['summary'], e['enable_key'], e['consent'],
                                '%s%s' % (e['setup_step'], e['tab'])])

        # Off: nothing reads active, and the settings route refuses in the table's words.
        code, hdrs, body = self.call(m, 'POST', '/settings', {'cloud_enabled': '0'})
        self.assertEqual(code, 200)
        ext = self.get_json(m, '/extensions')['extensions'][0]
        self.assertFalse(ext['enabled'])
        self.assertEqual([r['active'] for r in ext['roles']], [False, False])
        for form, words in (({'homing_mode': 'gfcloud'}, refusals[0]), ({'controller_mode': 'cloud'}, refusals[1])):
            code, hdrs, body = self.call(m, 'POST', '/settings', form)
            self.assertEqual((code, body.decode()), (409, words))
        # On, with gfcloud homing: that role reads active, the other does not.
        self.call(m, 'POST', '/settings', {'cloud_enabled': '1', 'phrase': 'I UNDERSTAND'})
        self.call(m, 'POST', '/settings', {'homing_mode': 'gfcloud'})
        ext = self.get_json(m, '/extensions')['extensions'][0]
        self.assertTrue(ext['enabled'])
        self.assertEqual([r['active'] for r in ext['roles']], [True, False])
        # Off again sweeps the selection to the table's fallback.
        self.call(m, 'POST', '/settings', {'cloud_enabled': '0'})
        self.assertEqual(self.get_json(m, '/settings')['homing_mode'], rows[0][4])

    def test_tokens_shape_and_scope(self):
        m = self.mock()
        ds = self.ds.Mock
        # The closed list and the route table's capability column are the C's.
        caps = re.search(r'static const char \*const CAPS\[\] = \{(.*?)\};', read('src/tokens.c'), re.S)
        self.assertEqual(tuple(re.findall(r'"([^"]+)"', caps.group(1))), ds.TOKEN_CAPS)
        self.assertEqual(int(re.search(r'#define TOKENS_MAX\s+(\d+)', read('src/tokens.h')).group(1)),
                         ds.TOKENS_MAX)
        table = re.search(r'struct route routes\[\] = \{(.*?)\n    \};', read('src/main.c'), re.S).group(1)
        column = {(r[0], r[1]): r[2] for r in re.findall(
            r'\{\s*"(GET|POST)",\s*"([^"]+)",[^{}]*?,\s*[01],\s*"([^"]+)"\s*\}', table)}
        self.assertEqual(column, ds.ROUTE_CAPS)
        self.assertEqual(len(re.findall(r'\{\s*"(?:GET|POST)"', table)),
                         len(re.findall(r'\{\s*"(?:GET|POST)",[^{}]*?,\s*[01],\s*(?:NULL|"[^"]+")\s*\}', table)),
                         'a route row without its capability column')
        for c in set(column.values()):
            self.assertTrue(c in ('camera', 'camera.any') or c in ds.TOKEN_CAPS, c)

        keys = c_keys(read('src/tokens.c'), 'tokens_json')
        self.assertEqual(doc_keys(self.get_json(m, '/tokens')), keys - {'id', 'name', 'created', 'last_used'})
        src = read('src/tokens.c')
        for form, words in (({'name': 'bad;name', 'caps': 'events'}, 'name is 1 to 40'),
                            ({'name': 'x', 'caps': 'settings.write'}, 'is not a capability a token can hold'),
                            ({'name': 'x', 'caps': ''}, 'a token holds at least one capability')):
            code, hdrs, body = self.call(m, 'POST', '/tokens', form)
            self.assertEqual(code, 400, form)
            self.assertIn(words, body.decode())
            self.assertIn(words.replace('40', '%d'), src)
        code, hdrs, body = self.call(m, 'POST', '/tokens', {'name': 'hub', 'caps': 'machine.read,camera.lid'})
        self.assertEqual(code, 200, body)
        made = json.loads(body)
        self.assertEqual(set(made), {'id', 'token'})
        self.assertRegex(made['token'], r'^fft_[0-9a-f]{32}$')
        listed = self.get_json(m, '/tokens')
        self.assertEqual(doc_keys(listed), keys)
        self.assertNotIn(made['token'][4:], json.dumps(listed))

        def scoped(method, path, q=None, how='bearer', token=None):
            token = token or made['token']
            headers = ({'Authorization': 'Bearer ' + token} if how == 'bearer'
                       else {'X-ForgeFIRM-Token': token} if how == 'header' else {})
            q = dict(q or {})
            if how == 'key':
                q['key'] = token
            return m.handle(method, path, q, headers, b'')

        code, hdrs, body = self.call(m, 'POST', '/tokens', {'name': 'viewer', 'caps': 'camera.lid'})
        viewer = json.loads(body)['token']

        auth_src = read('src/auth.c')
        self.assertEqual(scoped('GET', '/status')[0], 200)
        self.assertEqual(scoped('GET', '/status', how='header')[0], 200)
        # In a URL: a token that holds cameras and nothing else.
        self.assertEqual(scoped('GET', '/cam/status', how='key', token=viewer)[0], 200)
        code, hdrs, body = scoped('GET', '/cam/status', how='key')
        self.assertEqual((code, json.loads(body)['error']),
                         (403, 'a token in a URL may hold camera capabilities only'))
        self.assertIn('a token in a URL may hold camera capabilities only', read('src/auth.c'))
        self.assertEqual(scoped('GET', '/cam/status')[0], 200)
        for method, path, q, words in (
                ('POST', '/motion/jog', {'x': '1'}, 'this token does not hold motion.jog'),
                ('GET', '/cam/snapshot', {'cam': 'head'}, 'this token does not hold camera.head'),
                ('POST', '/settings', {'ui_units': 'metric'}, 'no scoped token reaches this route'),
                ('GET', '/tokens', None, 'no scoped token reaches this route'),
                ('POST', '/tokens', {'name': 'child', 'caps': 'events'}, 'no scoped token reaches this route')):
            code, hdrs, body = scoped(method, path, q)
            self.assertEqual((code, json.loads(body)['error']), (403, words), path)
            self.assertIn(words.split(' motion.jog')[0].split(' camera.head')[0], auth_src)
        # A key that looks scoped is a camera route's alone.
        self.assertNotEqual(scoped('POST', '/motion/jog', {'x': '1'}, how='key')[0], 200)
        code, hdrs, body = m.handle('GET', '/status', {}, {'Authorization': 'Bearer fft_' + '0' * 32}, b'')
        self.assertEqual((code, json.loads(body)['error']), (403, 'authentication required'))

        self.assertGreater(self.get_json(m, '/tokens')['tokens'][0]['last_used'], 0)
        self.assertEqual(self.get_json(m, '/tokens')['tokens'][0]['name'], 'hub')
        code, hdrs, body = self.call(m, 'POST', '/tokens/revoke', {'id': made['id']})
        self.assertEqual(code, 200)
        self.assertEqual(scoped('GET', '/status')[0], 403)
        code, hdrs, body = self.call(m, 'POST', '/tokens/revoke', {'id': made['id']})
        self.assertEqual((code, body.decode()), (404, 'no such token'))

    # -- every route the daemon registers, and every route the panel calls
    def test_every_daemon_route_is_served(self):
        m = self.mock()
        missing = []
        for method, path in endpoints():
            if path == '/':
                continue            # the page: the dev server's own
            code, hdrs, body = self.call(m, method, path)
            if (code == 404 and b'mock: no such endpoint' in body) or \
                    code == 405:
                missing.append('%s %s' % (method, path))
        self.assertEqual(missing, [])

    def test_every_panel_route_is_registered(self):
        registered = {p for _, p in endpoints()}
        routes = panel_routes()
        self.assertGreater(len(routes), 10, 'panel routes parsed')
        self.assertEqual([p for p in routes if p not in registered], [])


if __name__ == '__main__':
    unittest.main()
