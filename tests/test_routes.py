#!/usr/bin/env python3
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
"""Host test: the daemon's route table, read from its sources.

Every API path the panel's scripts call is a route main.c registers;
every row of main.c's routes[] carries its capability column, NULL or
a capability the closed list in tokens.c resolves; serve_page sends
PAGE_POLICY, the header that lets no frame navigate anywhere; and the
dev-server mock serves the daemon's page, its policy and its panel's
files, because the frame-isolation harness renders the mock's page;
main.c's list of environment variables, the ones the daemon's
start notice names, is held to every read in the sources; and the 409
body the panel sends a refused diagnostic start once more on is the
body cb_diag_start sends for the cooling engine's refusal, in the mock
the walk plays the panel against too.

Run: python3 -B -m unittest -v tests/test_routes.py
"""
import importlib.util
import os
import re
import unittest

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
ROW = re.compile(r'\{\s*"(GET|POST)",\s*"([^"]+)",\s*\w+,\s*[^,{}]+,\s*[01],\s*(NULL|"[^"]*")\s*\}')
ENV_NAME = r'"((?:GF|GFCOOL|FORGECTRL|FFLOG)_[A-Z0-9_]+)"'
ENV_READ = re.compile(r'\b(?:getenv|env_or|url_from_env)\(\s*' + ENV_NAME)
ENV_TABLE_ROW = re.compile(r'\{\s*' + ENV_NAME)
ENV_INDIRECT = re.compile(r'\bgetenv\(\s*(?!")([^)]*)\)')


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8') as f:
        return f.read()


def routes():
    """The routes[] rows of main.c as (method, path, cap or None), and how
    many rows the table opens, parsed or not."""
    table = re.search(r'struct route routes\[\] = \{(.*?)\n    \};', read('src/main.c'), re.S).group(1)
    rows = [(m, p, None if c == 'NULL' else c.strip('"')) for m, p, c in ROW.findall(table)]
    return rows, len(re.findall(r'\{\s*"(?:GET|POST)"', table))


def closed_caps():
    """The capabilities a scoped token can hold (CAPS in tokens.c)."""
    return set(re.findall(r'"([^"]+)"', re.search(r'CAPS\[\] = \{(.*?)\};', read('src/tokens.c'), re.S).group(1)))


def resolves(cap, closed):
    """Whether the guards can judge a route's capability (auth.c): one of
    the list, "camera" for the camera the request names, or a prefix.any."""
    if cap == 'camera':
        return {'camera.lid', 'camera.head'} <= closed
    if cap.endswith('.any'):
        return any(c.startswith(cap[:-3]) for c in closed)
    return cap in closed


def devserver():
    """tools/devserver.py, loaded: the page the dev server and the
    frame-isolation harness serve."""
    spec = importlib.util.spec_from_file_location('devserver', os.path.join(ROOT, 'tools', 'devserver.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def panel_routes():
    """The API paths the panel's scripts call (a quoted literal starting
    with a slash, at a fetch/fx/startJob call or as a stream source)."""
    paths = set()
    for name in os.listdir(os.path.join(ROOT, 'src', 'ui')):
        if name.endswith('.js'):
            js = read(os.path.join('src', 'ui', name))
            paths.update(re.findall(r"(?:fetch|fx|startJob)\('(/[^'?]*)", js))
            paths.update(re.findall(r"src = '(/[^'?]*)", js))
    return sorted(p for p in paths if p != '/' and not p.endswith('/'))


def panel_retry_text():
    """The 409 body the panel's startDiag (src/ui/panel.js) sends the
    start once more on, a second later."""
    body = re.search(r'function startDiag\(.*?\n\}', read(os.path.join('src', 'ui', 'panel.js')), re.S).group(0)
    return re.search(r"r\.status === 409 && x === '([^']*)'", body).group(1)


def daemon_refusal_text():
    """The body cb_diag_start (src/main.c) sends for diag_start's -2, the
    cooling engine's refusal."""
    body = re.search(r'static int cb_diag_start\(.*?\n\}', read('src/main.c'), re.S).group(0)
    return re.search(r'case -2:\s*return reply_error\(res, 409, "([^"]*)"\)', body).group(1)


def mock_refusal_text():
    """The body the dev server's mock (tools/devserver.py) sends for the
    engine's refusal it is told to play (diag_engine_busy), which the
    walk's diagnostic cases use."""
    src = read(os.path.join('tools', 'devserver.py'))
    block = src[src.index("if path.startswith('/diag/') and path != '/diag/abort':"):]
    return re.search(r"if self\.diag_engine_busy:\s*self\.diag_engine_busy -= 1\s*return T\(409, '([^']*)'\)",
                     block).group(1)


def env_reads():
    """Every environment variable src/ reads: getenv("X"), the env_or and
    url_from_env wrappers, and the GFCOOL_* rows of cool.c's tunables; and
    every getenv that does not name its variable, which must be one of
    those wrappers, that table's loop, or the start notice's own loop."""
    names, indirect = set(), set()
    for name in sorted(os.listdir(os.path.join(ROOT, 'src'))):
        if name.endswith(('.c', '.h')):
            src = read(os.path.join('src', name))
            names.update(ENV_READ.findall(src))
            if name == 'cool.c':
                names.update(ENV_TABLE_ROW.findall(src))
            indirect.update('%s:%s' % (name, m.strip()) for m in ENV_INDIRECT.findall(src))
    return names, indirect


class RouteTest(unittest.TestCase):
    def test_every_panel_route_is_registered(self):
        registered = {p for _, p, _ in routes()[0]}
        called = panel_routes()
        self.assertGreater(len(called), 10, 'panel routes parsed')
        self.assertEqual([p for p in called if p not in registered], [])

    def test_every_route_has_its_capability(self):
        rows, opened = routes()
        self.assertGreater(len(rows), 50, 'routes parsed')
        self.assertEqual(len(rows), opened, 'a routes[] row without its capability column')
        closed = closed_caps()
        self.assertEqual(['%s %s: %s' % r for r in rows if r[2] is not None and not resolves(r[2], closed)], [])

    def test_every_page_sends_the_page_policy(self):
        src = read('src/main.c')
        self.assertRegex(src, r'#define PAGE_POLICY "[^"]+"')
        body = re.search(r'static int serve_page\(.*?\n\}', src, re.S).group(0)
        self.assertIn('"Content-Security-Policy", PAGE_POLICY', body)

    def test_the_mock_serves_the_daemons_page(self):
        ds = devserver()
        policy = re.search(r'#define PAGE_POLICY "([^"]*)"', read('src/main.c')).group(1)
        index = re.search(r'PAGE STREQUAL "index"\)\s*set\(CSS_FILES ([^)]*)\)\s*set\(JS_FILES ([^)]*)\)',
                          read('src/ui/embed.cmake'))
        self.assertEqual(ds.PAGE_HEADERS['Content-Security-Policy'], policy)
        self.assertEqual((ds.CSS_FILES, ds.JS_FILES), (tuple(index.group(1).split()), tuple(index.group(2).split())))

    def test_the_panels_retry_text_is_the_daemons_refusal_text(self):
        text = daemon_refusal_text()
        self.assertEqual(panel_retry_text(), text)
        self.assertEqual(mock_refusal_text(), text)

    def test_every_environment_override_is_named_at_start(self):
        table = re.search(r'ENV_OVERRIDES\[\] = \{(.*?)\};', read('src/main.c'), re.S).group(1)
        listed = re.findall(ENV_NAME, table)
        names, indirect = env_reads()
        self.assertGreater(len(names), 50, 'environment reads parsed')
        self.assertEqual(sorted(listed), sorted(names))
        self.assertEqual(len(listed), len(set(listed)), 'a name listed twice')
        self.assertEqual(indirect, {'logs.c:name', 'relcheck.c:name', 'super.c:name',
                                    'cool.c:tunables[i].env', 'main.c:ENV_OVERRIDES[i]'})


if __name__ == '__main__':
    unittest.main()
