#!/usr/bin/env python3
"""Panel development server: live reload for the control panel.

Serves the panel from src/ui/ (index.html, theme.css, help.js, forms.js,
panel.js and the Bootstrap files under vendor/) as plain files - so the
browser's devtools see real file names and line numbers -
and reloads the open tab through an injected watcher as soon as anything
in that directory changes, while every API request is either

  * proxied to a real machine (GF_HOST / GF_TOKEN, from the environment
    or the .env file at the repo root; see .env.example), so the panel
    shows live data and its actions reach the hardware exactly as they
    would from the machine's own HTTPS port; or
  * answered by the built-in mock backend (--mock, or automatically when
    no GF_HOST is configured), which exercises the JavaScript without a
    machine and mirrors the daemon's token check on state-changing
    endpoints.

Usage:
    python3 tools/devserver.py [--port 8081] [--bind 127.0.0.1]
                               [--host ADDR[:PORT]] [--token HEX]
                               [--env FILE] [--mock] [--bundle] [-v]
    python3 tools/devserver.py --dump    # the bundled page to stdout

Then browse http://127.0.0.1:8081 and edit src/ui/. --bundle serves the
page the way the daemon does (CSS and JS inlined, one response, before
the daemon's gzip), which is also what --dump prints; the bundling
mirrors src/ui/embed.cmake.

Requires only the Python 3 standard library.
"""
import argparse
import hashlib
import http.client
import io
import json
import os
import re
import socket
import ssl
import sys
import tarfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qsl

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
UI_DIR = os.path.join(ROOT, 'src', 'ui')
ENV_FILE = os.path.join(ROOT, '.env')

TOKEN_MARK = '__FFTOKEN__'          # in panel.js; the daemon substitutes it
# The files index.html links, in load order: the list embed.cmake inlines,
# one marker tag per file.
CSS_FILES = ('vendor/bootstrap.min.css', 'theme.css')
JS_FILES = ('vendor/bootstrap.bundle.min.js', 'regions.js', 'help.js', 'forms.js',
            'panel.js')

DEFAULT_PORT = 8081
DEVICE_PORT = 443                # the machine's HTTPS listener; the dev server proxies over TLS
MOCK_TOKEN = '0123456789abcdef0123456789abcdef'
WATCH_POLL_S = 0.25         # src/ui/ stat interval
WATCH_HOLD_S = 25.0         # long-poll ceiling per /__dev/watch call
PROXY_TIMEOUT_S = 30.0      # per-request upstream timeout (streams: none)
STREAM_PATHS = ('/cam/stream',)

HOP_BY_HOP = {
    'connection', 'keep-alive', 'proxy-authenticate',
    'proxy-authorization', 'te', 'trailers', 'transfer-encoding',
    'upgrade',
}
# The daemon's anti-rebinding / anti-CSRF layer wants an address-literal
# Host and no foreign Origin. The panel is same-origin to this server,
# so these browser-side headers describe the dev origin, not the
# machine's - they are dropped and Host is rewritten to the device.
DROP_REQ_HEADERS = HOP_BY_HOP | {
    'host', 'origin', 'referer', 'accept-encoding',
    'sec-fetch-site', 'sec-fetch-mode', 'sec-fetch-dest',
    'sec-fetch-user',
}
DROP_RESP_HEADERS = HOP_BY_HOP | {'content-length'}


# ------------------------------------------------------------ .env
def parse_env_file(path):
    """KEY=VALUE per line; '#' comments, optional 'export ', optional
    single/double quotes. Returns {} for a missing file."""
    out = {}
    try:
        with open(path, encoding='utf-8') as f:
            lines = f.read().splitlines()
    except OSError:
        return out
    for ln in lines:
        s = ln.strip()
        if not s or s.startswith('#') or '=' not in s:
            continue
        if s.startswith('export '):
            s = s[7:].lstrip()
        k, v = s.split('=', 1)
        k = k.strip()
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in '"\'':
            v = v[1:-1]
        elif ' #' in v:
            v = v[:v.index(' #')].rstrip()
        if k:
            out[k] = v
    return out


class Config:
    """Effective device settings: CLI > process environment > .env.
    The .env file is re-read whenever it changes, so switching machines
    (or fixing a token) does not need a restart."""

    def __init__(self, args):
        self.args = args
        self.env_path = args.env or ENV_FILE
        self._stamp = None
        self._file = {}
        self._lock = threading.Lock()
        self.refresh()

    def _stat(self):
        try:
            st = os.stat(self.env_path)
            return (st.st_mtime_ns, st.st_size)
        except OSError:
            return None

    def refresh(self):
        with self._lock:
            st = self._stat()
            if st != self._stamp:
                self._stamp = st
                self._file = parse_env_file(self.env_path)

    def get(self, key):
        self.refresh()
        return self._file.get(key, '')

    @property
    def mock(self):
        return bool(self.args.mock) or not self.host

    @property
    def host(self):
        h = self.args.host or os.environ.get('GF_HOST') or self.get('GF_HOST')
        return (h or '').strip()

    @property
    def token(self):
        t = (self.args.token or os.environ.get('GF_TOKEN') or
             self.get('GF_TOKEN'))
        return (t or '').strip()

    def upstream(self):
        """(ip, port, display) for the device; the daemon accepts only an
        address-literal Host, so a name is resolved here once per call
        and the literal is what goes on the wire."""
        h = self.host
        if '://' in h:                              # a pasted URL is fine
            h = h.split('://', 1)[1]
        h = h.split('/', 1)[0]
        port = DEVICE_PORT
        if h.startswith('['):                       # [v6]:port
            end = h.find(']')
            name = h[1:end]
            rest = h[end + 1:]
            if rest.startswith(':'):
                port = int(rest[1:])
        elif h.count(':') == 1:
            name, p = h.split(':')
            port = int(p)
        else:
            name = h
        try:
            ip = socket.getaddrinfo(name, port)[0][4][0]
        except (socket.gaierror, IndexError):
            ip = name
        if ':' in ip:
            ip = '[%s]' % ip                        # bracket a v6 literal
        return ip, port, '%s:%d' % (name if ':' not in name else
                                    '[%s]' % name, port)


# ------------------------------------------------------------ panel
RELOAD_JS = (
    "<script>(function(){var v=%(ver)s;"
    "function w(){var x=new XMLHttpRequest();"
    "x.open('GET','/__dev/watch?v='+encodeURIComponent(v),true);"
    "x.onreadystatechange=function(){if(x.readyState!==4)return;"
    "if(x.status===200){var j;try{j=JSON.parse(x.responseText)}catch(e){}"
    "if(j&&j.v!==v){location.reload();return}w()}"
    "else{setTimeout(w,1500)}};"
    "x.onerror=function(){setTimeout(w,1500)};x.send()}w()})()</script>"
)
BADGE_HTML = (
    "<div id='__devbadge' style='position:fixed;right:10px;bottom:10px;"
    "z-index:9999;background:%(bg)s;color:#fff;font:600 11px/1 "
    "system-ui,sans-serif;padding:6px 9px;border-radius:5px;opacity:.85;"
    "letter-spacing:.3px;pointer-events:none'>DEV &middot; %(label)s</div>"
)
CONTENT_TYPES = {
    '.html': 'text/html; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.js': 'application/javascript; charset=utf-8',
    '.svg': 'image/svg+xml',
    '.png': 'image/png',
    '.ico': 'image/x-icon',
}


def html_escape(s):
    return (str(s).replace('&', '&amp;').replace('<', '&lt;')
            .replace('>', '&gt;'))


def bundle(html, read):
    """One self-contained page - the same replacement embed.cmake does,
    so `--dump` / `--bundle` show exactly what the daemon serves (before
    its gzip). `read(name)` returns a file under src/ui/ as text."""
    for name in CSS_FILES:
        tag = '<link rel="stylesheet" href="%s" />' % name
        if tag not in html:
            raise ValueError('index.html lacks the marker %s' % tag)
        html = html.replace(tag, '<style>\n' + read(name) + '</style>', 1)
    for name in JS_FILES:
        tag = '<script src="%s"></script>' % name
        if tag not in html:
            raise ValueError('index.html lacks the marker %s' % tag)
        html = html.replace(tag, '<script>\n' + read(name) + '</script>', 1)
    return html


class Panel:
    """The files under src/ui/. A watcher thread stamps the directory and
    wakes the long-poll waiters when anything in it changes."""

    def __init__(self, ui_dir):
        self.dir = ui_dir
        self.cond = threading.Condition()
        self.version = self._stamp()
        t = threading.Thread(target=self._watch, daemon=True)
        t.start()

    def _stamp(self):
        parts = []
        try:
            names = sorted(os.listdir(self.dir))
        except OSError:
            return 'missing'
        for n in names:
            p = os.path.join(self.dir, n)
            if os.path.isdir(p):
                try:
                    subs = [n + '/' + m for m in sorted(os.listdir(p))]
                except OSError:
                    continue
            else:
                subs = [n]
            for s in subs:
                try:
                    st = os.stat(os.path.join(self.dir, s))
                except OSError:
                    continue
                parts.append('%s:%d:%d' % (s, st.st_mtime_ns, st.st_size))
        return hashlib.sha1('|'.join(parts).encode()).hexdigest()[:16]

    def _watch(self):
        while True:
            time.sleep(WATCH_POLL_S)
            v = self._stamp()
            if v != self.version:
                with self.cond:
                    self.version = v
                    self.cond.notify_all()

    def wait_change(self, seen, timeout):
        """Block until the version differs from `seen` or the timeout
        passes; returns the current version."""
        with self.cond:
            if self.version == seen:
                self.cond.wait(timeout)
            return self.version

    def path(self, name):
        """Absolute path of a file in src/ui/ or its vendor/ directory, or
        None if `name` is not a plain file there (no traversal, no
        dotfiles)."""
        parts = name.split('/') if name else []
        if not parts or len(parts) > 2 or (len(parts) == 2 and
                                            parts[0] != 'vendor'):
            return None
        for part in parts:
            if not part or part.startswith('.') or '\\' in part:
                return None
        p = os.path.join(self.dir, *parts)
        return p if os.path.isfile(p) else None

    def read(self, name):
        with open(os.path.join(self.dir, name), 'rb') as f:
            return f.read()

    def text(self, name):
        return self.read(name).decode('utf-8')

    def page(self, bundled):
        """index.html as text: the files linked (dev default, real file
        names and line numbers in the browser) or inlined."""
        html = self.text('index.html')
        if bundled:
            html = bundle(html, self.text)
        return html

    def render(self, token, label, mock, bundled):
        page = self.page(bundled).replace(TOKEN_MARK, token or '', 1)
        badge = BADGE_HTML % {'bg': '#c7760a' if mock else '#3d854d',
                              'label': html_escape(label)}
        inject = badge + (RELOAD_JS % {'ver': json.dumps(self.version)})
        k = page.rfind('</body>')
        return page + inject if k < 0 else page[:k] + inject + page[k:]


# ------------------------------------------------------------- mock
MOCK_SVG = (
    "<svg xmlns='http://www.w3.org/2000/svg' width='640' height='360' "
    "viewBox='0 0 640 360'><rect width='640' height='360' fill='#3a3d46'/>"
    "<rect x='40' y='30' width='560' height='300' fill='none' "
    "stroke='#8b8f99' stroke-dasharray='8 6'/>"
    "<text x='320' y='170' fill='#d5d8de' font-family='system-ui,sans-serif' "
    "font-size='26' text-anchor='middle'>lid camera (mock)</text>"
    "<text x='320' y='205' fill='#8b8f99' font-family='system-ui,sans-serif' "
    "font-size='14' text-anchor='middle'>%s</text></svg>"
)

# The settings keys forgectrl serves, in the order of its setting_defs
# table (src/main.c). GET /settings reports a secret key as "<key>_set"
# (a boolean) and never its value; POST accepts it under its own name.
# tests/test_devserver_mock.py holds both tuples to the C table.
SETTINGS_KEYS = (
    'controller_mode', 'homing_mode',
    'gfcloud_home_x', 'gfcloud_home_y',
    'gfcloud_home_timeout_s', 'gf_serial', 'gf_password', 'ui_units',
    'wifi_country',
    'cool_flow_rise', 'cool_flow_heater_pct', 'cool_flow_check_s',
    'cool_recheck_s', 'cool_confirm_max_s', 'cool_laser_heat_cw',
    'cool_laser_heat_density', 'cool_aa_offset_counts',
    'cool_temp_max', 'cool_temp_resume', 'cool_temp_critical_c',
    'cool_temp_min', 'cool_temp_start', 'cool_tec_present',
    'cool_temp_offset_c',
    'cool_tec_on_c', 'cool_tec_off_c',
    'cool_fire_q1_alert', 'cool_fire_q1_critical',
    'cool_fire_q2_alert', 'cool_fire_q2_critical',
    'cool_accel_x_alert', 'cool_accel_y_alert', 'cool_accel_abort',
    'cool_cooldown_s', 'cool_cooldown_max_s',
    'cool_tach_exhaust_min_rpm', 'cool_tach_intake_min_rpm',
    'cool_tach_air_assist_min_rpm', 'cool_purge_min_current',
    'cool_fan_grace_s',
    'laser_button_timeout_s', 'laser_disarm_s', 'laser_floor_density',
    'laser_dose_curve', 'laser_corner_gamma', 'lens_hall_edge_z_mm', 'lens_park_z_mm',
    'lens_stop_below_steps', 'lens_stop_above_steps',
    'laser_pulse_ticks',
    'laser_pulse_min_ticks', 'rail_settle_s', 'lid_lamp_idle',
    'cloud_pause_backtrack_ticks', 'cloud_resume_lead_ticks',
    'cloud_hold_max_s', 'pulse_warn_threshold_bytes',
    'pulse_reject_threshold_bytes', 'lid_policy',
    'xy_microsteps',
    'cloud_enabled', 'panel_open_reads',
    'log_forgectrl_disk', 'log_forgectrl_remote',
    'log_grblhal_disk', 'log_grblhal_remote',
    'log_gfcloud_disk', 'log_gfcloud_remote',
    'log_gfhome_disk', 'log_gfhome_remote',
    'log_kernel_disk', 'log_kernel_remote',
    'log_system_disk', 'log_system_remote',
    'syslog_server', 'syslog_port', 'syslog_proto',
)
SECRET_KEYS = ('gf_password',)
# The enumerated keys the daemon's validators accept (the numeric keys
# outside the gate table are not range-checked here).
SETTING_CHOICES = {
    'controller_mode': ('grbl', 'cloud'),
    'homing_mode': ('none', 'gfcloud', 'switches'),
    'ui_units': ('metric', 'imperial'),
    'cool_tec_present': ('0', '1'),
    'lid_policy': ('cancel', 'hold'),
    'xy_microsteps': ('8', '16', '32'),
    'cloud_enabled': ('0', '1'),
    'panel_open_reads': ('0', '1'),
    'syslog_proto': ('udp', 'tcp'),
}
LOGGERS = ('forgectrl', 'grblhal', 'gfcloud', 'gfhome', 'kernel', 'system')
LOG_LEVELS = ('off', 'error', 'warning', 'notice', 'info', 'debug')
LOGS_ROOT = '/data/log/forgefirm'
FAN_NAMES = ('exhaust', 'intake_1', 'intake_2', 'air_assist', 'purge')
FIRE_GATES = ('cool_fire_q1_alert', 'cool_fire_q1_critical',
              'cool_fire_q2_alert', 'cool_fire_q2_critical')
DIAG_TOOLS = ('flow-verify', 'flow-calibrate', 'aa-offset-calibrate')
SLOT_TARGETS = {'sd': '/dev/mmcblk1p1', 'a': '/dev/mmcblk2p1',
                'b': '/dev/mmcblk2p2', 'legacy': '/dev/mmcblk2p4'}
MOCK_VERSION = '20260101000000 (mock)'
MOCK_RELEASE = '0.0.2'          # what /update/check offers
MOCK_FINGERPRINT = ':'.join(['%02X' % ((i * 37 + 11) & 0xff)
                             for i in range(32)])
MOCK_SHEET_ID = 'UL5UU-LS5PI'
MOCK_MANIFEST = ('PACKAGE NAME: forgectrl\nPACKAGE VERSION: 0.0.1\n'
                 'RECIPE NAME: forgectrl\nLICENSE: MIT\n\n'
                 'PACKAGE NAME: grblhal-glowforge\nPACKAGE VERSION: 0.0.1\n'
                 'RECIPE NAME: grblhal-glowforge\nLICENSE: GPL-3.0-or-later\n\n')
MOCK_CAMKEY = 'c0ffee00' * 4
ADVISORY_DOCS = ('safety-and-risk', 'licenses', 'privacy', 'cloud-service')
ADVISORY_META = {
    'safety-and-risk': ('Safety and risk', 'typed', 'I UNDERSTAND'),
    'licenses': ('Licenses and notices', 'check', None),
    'privacy': ('Privacy', 'check', None),
    'cloud-service': ('The Glowforge cloud service', 'check', None),
}
WIZARDS = (('advisories', 'Advisories'), ('account', 'Your account'),
           ('preferences', 'Preferences'), ('machine', 'Your machine'),
           ('cloud', 'Cloud mode'),
           ('switches', 'Switches'), ('sensors', 'Sensors'), ('airflow', 'Airflow'),
           ('motion', 'Motion'), ('cameras', 'Cameras'),
           ('cooling.aa-offset', 'Coolant offset'), ('cooling.flow', 'Coolant flow'),
           ('cooling.tec', 'TEC'), ('cooling.flow-verify', 'Flow check'),
           ('cloud.header', 'Cloud header'),
           ('sheet.place', 'Place the sheet'), ('sheet.frame', 'First fire'),
           ('laser.focus', 'Focus'), ('laser.floor', 'Laser floor'),
           ('laser.dose-curve', 'Dose curve'), ('laser.corner', 'Corner rolloff'),
           ('cooling.flow-load', 'Flow under load'))
DARK = ('switches', 'sensors', 'airflow', 'motion', 'cameras', 'cooling.aa-offset',
        'cooling.flow', 'cooling.tec', 'cooling.flow-verify', 'cloud.header')
# The sheet wizards run on the same mock runner; the daemon's previews
# are stood in for by a drawing of the card's box.
LIVE = ('sheet.place', 'sheet.frame', 'laser.focus', 'laser.floor', 'laser.dose-curve',
        'laser.corner', 'cooling.flow-load')
# The what-changed menu (commission.c): a change, its title, its reason,
# and the wizards it flags with their levels.
CHANGES = (
    ('tube', 'The laser tube was replaced', 'the tube was replaced',
     (('laser.floor', 'required'), ('laser.dose-curve', 'required'),
      ('cooling.flow-load', 'required'), ('laser.corner', 'recommended'))),
    ('pump', 'The coolant pump was replaced', 'the pump was replaced',
     (('cooling.flow', 'required'),)),
    ('coolant', 'The coolant was changed', 'the coolant was changed',
     (('cooling.flow', 'required'),)),
    ('fan', 'A fan was replaced', 'a fan was replaced', (('airflow', 'required'),)),
    ('head', 'The head was replaced', 'the head was replaced',
     (('machine', 'required'), ('laser.focus', 'required'), ('motion', 'recommended'),
      ('cameras', 'recommended'))),
    ('tray', 'The tray was replaced', 'the tray was replaced',
     (('laser.focus', 'recommended'),)),
    ('service', 'A cover was off: belts, drivers, wiring, or switches were serviced',
     'the machine was serviced', (('switches', 'recommended'), ('motion', 'recommended'))),
)
# The daemon's layout (sheet.c): a 200 x 150 sheet, the frame 180 x 130
# at (10, 10), the header band, and the five cards in two rows.
SHEET_CARDS = {
    'laser.focus': ('Focus', 10, 46, 58, 48), 'laser.floor': ('Laser floor', 72, 46, 66, 48),
    'laser.corner': ('Corner rolloff', 142, 46, 48, 48),
    'laser.dose-curve': ('Dose curve', 10, 98, 112, 38),
    'cooling.flow-load': ('Flow under load', 126, 98, 64, 38),
}


def sheet_svg(card):
    """A stand-in for GET /wiz/sheet.svg: the sheet with the frame, the
    card's box (every box for the frame), and the header line."""
    out = ["<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 200 150' width='200mm' height='150mm'>",
           "<rect width='200' height='150' fill='#e7d3ac' stroke='#9c8a66' stroke-width='0.4'/>",
           "<rect x='10' y='10' width='180' height='130' fill='none' stroke='#3a2a12' stroke-width='0.3'/>",
           "<text x='58' y='20' font-size='6' fill='#3a2a12'>OpenGlow ForgeFIRM</text>",
           "<text x='58' y='27' font-size='4' fill='#3a2a12'>Hardware Commissioning (mock)</text>"]
    boxes = list(SHEET_CARDS) if card in ('sheet.place', 'sheet.frame') else \
        [card] if card in SHEET_CARDS else []
    for k in boxes:
        t, x, y, w, h = SHEET_CARDS[k]
        out.append("<rect x='%d' y='%d' width='%d' height='%d' fill='none' stroke='#3a2a12' "
                   "stroke-width='0.3'/><text x='%d' y='%d' font-size='4' fill='#3a2a12'>%s</text>"
                   % (x, y, w, h, x + 2, y + 6, t))
    out.append('</svg>')
    return '\n'.join(out)
# The mock dark wizard: a prompt after PROMPT_AT seconds, done DONE_AFTER
# seconds after the answer, with a result shaped like the daemon's.
DARK_PROMPT_AT = 1.0
DARK_DONE_AFTER = 2.0
DARK_PROMPTS = {
    'sheet.place': ('jog', 'place', 'Put the head at the back-left, place the sheet, close the lid, '
                    'and set the origin.', ['X-10', 'X+10', 'Y-10', 'Y+10', 'X-1', 'X+1', 'Y-1', 'Y+1',
                                            'Set origin']),
    'sheet.frame': ('choice', 'frame-ok', 'Is the frame on the sheet, square, and visible?',
                    ['Yes', 'Again, lighter', 'Again, darker']),
    'laser.focus': ('multichoice', 'focus-pick', 'Which line is the narrowest and sharpest? Choose '
                    'every adjacent line that looks the same; the middle of the run is taken. Then Done.',
                    [str(i) for i in range(1, 13)]),
    'laser.floor': ('choice', 'floor-pick', 'Which is the faintest rung with a continuous line?',
                    [str(d) for d in range(2, 25, 2)] + ['None']),
    'laser.corner': ('choice', 'corner-pick', 'Which pattern has the most even corners?',
                     ['1.00', '1.25', '1.50', '1.75', '2.00']),
    'switches': ('wait', 'lid-open', 'Open the lid.', []),
    'sensors': ('number', 'room-temp', 'Optional: the room temperature in C.', ['Set', 'Skip']),
    'cameras': ('confirm', 'lid-view', 'This is the lid camera. Can you see the bed?', ['Yes', 'No']),
    'motion': ('continue', 'jogs', 'The head moves 50 mm each way on X, then on Y.', ['Continue']),
    'cloud.header': ('continue', 'print', 'In the Glowforge app, place any small design and press '
                     'Print. Do not press the machine button.', ['Continue']),
}
DARK_RESULTS = {
    'sheet.place': {'origin_x': 0, 'origin_y': 229, 'origin_z': 0, 'alone': False, 'thickness_mm': 3.2,
                    'steps_per_mm': {'x': 53.333, 'y': 53.333, 'z': 2.832}, 'z_referenced': True,
                    'summary': "The origin is set at the head's position. The full sheet, 3.2 mm thick. "
                               'Every card is drawn from this point: do not move the sheet until the '
                               'last card is done.'},
    'sheet.frame': {'mark_s': 400, 'mark_feed': 3000, 'started': '2026-09-05 20:00Z',
                    'emission': {'hv_max': 412, 'laser_on_samples': 1800, 'thermopile_delta': 380, 'lit_s': 62.0},
                    'summary': 'The frame and the header are on the sheet. The mark dose is S400 at '
                               "3000 mm/min; every card's box and labels burn at it. All three witnesses "
                               "saw the beam: the tube current, the kernel's LASER_ON samples, and the "
                               'head thermopile.'},
    'laser.focus': {'pick': 11.5, 'picked_lines': [11, 12], 'thickness_mm': 2.8, 'pick_half_steps': -12.5,
                    'edge_z_mm': 7.08, 'steps_per_mm': 2.922, 'max_height_mm': 13.92,
                    'focus_range_mm': {'min': 2.29, 'max': 13.92},
                    'stops': {'found': True, 'below': 14, 'above': 20, 'contact_below': 16,
                              'contact_above': 22, 'why': ''},
                    'emission': {'hv_max': 402, 'laser_on_samples': 300, 'thermopile_delta': 350, 'lit_s': 9.0},
                    'summary': 'Line 11.5 on 2.8 mm: with the lens on its hall reference the focus is '
                               '7.08 mm above the tray. A home puts the lens there, sets Z to that '
                               'height, and parks the focus at the park height; Z counts the focus '
                               'height above the bed. The stops were found 14 half-steps below the '
                               'reference and 20 above, so the lens reaches 2.29 mm to 13.92 mm. Every '
                               'later card runs at the focus for this sheet.'},
    'laser.floor': {'faintest_density': 8.0, 'floor_density': 10.0,
                    'emission': {'hv_max': 610, 'laser_on_samples': 900, 'thermopile_delta': 600, 'lit_s': 30.0},
                    'summary': 'The faintest rung with a continuous line is 8 percent density. The laser '
                               'floor is set at 10 percent, two above it: no job asks the tube for less, '
                               'so the lightest engraving still marks this wood.'},
    'laser.dose-curve': {'curve': '10:0.5,20:2,30:7,45:21,60:37,80:50,100:100',
                         'points': [{'density': 10, 'light': 0.5}, {'density': 20, 'light': 2},
                                    {'density': 30, 'light': 7}, {'density': 45, 'light': 21},
                                    {'density': 60, 'light': 37}, {'density': 80, 'light': 50},
                                    {'density': 100, 'light': 100}],
                         'emission': {'hv_max': 1010, 'laser_on_samples': 1200, 'thermopile_delta': 900, 'lit_s': 70.0},
                         'summary': 'The curve rose rung by rung and is written. It maps the density a job '
                                    'asks for to the light the tube gives: 10 percent density gives 0.5 '
                                    'percent of the light, 45 gives 21, and 100 gives 100. The chart shows '
                                    'the fit.'},
    'laser.corner': {'gamma': 1.5,
                     'emission': {'hv_max': 350, 'laser_on_samples': 400, 'thermopile_delta': 300, 'lit_s': 20.0},
                     'summary': 'Pattern 1.50 has the most even corners: the corner rolloff is set to 1.50. '
                                'Under velocity-scaled power the machine eases the power into every corner '
                                'by that exponent, so corners burn like the straights.'},
    'cooling.flow-load': {'lit_s': 88.0, 'dose_raw_s': 52000.0, 'hv_mean': 590.0, 'base_c': 22.4, 'peak_c': 1.6,
                          't_peak_s': 95.0, 'lag_s': 15.0, 'k_density': 3.1e-05, 'k_cw': 4.0e-05,
                          'emission': {'hv_max': 620, 'laser_on_samples': 2000, 'thermopile_delta': 500, 'lit_s': 88.0},
                          'summary': 'The tube was lit for 88 s and warmed the coolant by 1.60 C, peaking 95 s '
                                     "after the fire began. That is the tube's own share of a flow check's "
                                     'rise on this machine. It is written, and the flow check allows for it '
                                     'from now on.'},
    'switches': {'lid': True, 'button': True, 'interlock': 'satisfied',
                 'hv_enable_follows_lid': True, 'head_present': True},
    'sensors': {'coolant_down_c': 22.4, 'coolant_up_c': 21.9, 'chassis_c': 28.0, 'soc_c': 45.2,
                'lid_ir_max': [110, 120, 105, 118], 'accel_events': 0, 'laser_pgood': 1,
                'hv_current_max': 0, 'exhaust_rpm_idle': 3400, 'intake_rpm_idle': 2200},
    'airflow': {'fans': {'exhaust': {'steady_rpm': 12000, 'spinup_s': 3.6, 'ok': True},
                         'intake 1': {'steady_rpm': 4200, 'spinup_s': 2.1, 'ok': True},
                         'intake 2': {'steady_rpm': 4180, 'spinup_s': 2.2, 'ok': True},
                         'air assist': {'steady_rpm': 11000, 'spinup_s': 1.4, 'ok': True}},
                'purge_current_on': 628, 'purge_current_off': 1,
                'floors': {'cool_tach_exhaust_min_rpm': '6600', 'cool_tach_intake_min_rpm': '2299',
                           'cool_tach_air_assist_min_rpm': '6050', 'cool_purge_min_current': '345',
                           'cool_fan_grace_s': '9'}},
    'motion': {'probe': 'MOTION OK - head accel p2p x=3567 y=1312', 'z_referenced': True,
               'z_passes': [40, 40, 42, 40, 41], 'rail': 'up',
               'moves': {'+X': {'p2p_x': 3100, 'p2p_y': 400, 'witnessed': True}}},
    'cameras': {'sensor': 'OV5648', 'lamp_idle': 236, 'lid_ok': True, 'head_ok': True},
    'cooling.aa-offset': {'offset_counts': 16.0, 'spread_counts': 1.2, 'recommend': 16.0},
    'cooling.flow': {'flow_max': 6.1, 'noflow_min': 18.9, 'gap': 12.8, 'recommend': 12.5,
                     'heater_pct': 40, 'threshold': 12.5},
    'cooling.tec': {'tec': False, 'skipped': 'no TEC on this machine'},
    'cooling.flow-verify': {'pass': True, 'threshold': 12.5, 'flow_rise': 6.0, 'noflow_rise': 18.8,
                            'thin_margin': False},
    'cloud.header': {'captured': '2026-01-01T00:00:00Z', 'job_id': 'mock', 'tag_count': 548,
                     'calibration_tags': {'EFrd': 65535, 'IFrd': 43278, 'AArd': 1023, 'CMrx': 33000,
                                          'XScr': 135, 'YScr': 135},
                     'limits': {'coolant_max_c': 33.0},
                     'machine': {'cool_tach_exhaust_min_rpm': {'local': '6600', 'default': 6400}}},
}
DARK_APPLIED = {
    'laser.focus': {'lens_hall_edge_z_mm': {'from': '3.35', 'to': '7.08'},
                    'lens_stop_below_steps': {'from': '', 'to': '14'},
                    'lens_stop_above_steps': {'from': '', 'to': '20'}},
    'laser.floor': {'laser_floor_density': {'from': '12', 'to': '10'}},
    'laser.dose-curve': {'laser_dose_curve': {'from': 'off', 'to': '10:0.5,20:2,30:7,45:21,60:37,80:50,100:100'}},
    'laser.corner': {'laser_corner_gamma': {'from': '2', 'to': '1.50'}},
    'cooling.flow-load': {'cool_laser_heat_density': {'from': '2.7e-05', 'to': '3.1e-05'},
                          'cool_laser_heat_cw': {'from': '3.5e-05', 'to': '4e-05'}},
    'sensors': {'cool_temp_offset_c': {'from': '0', 'to': '0.5'}},
}
PRESS_S = 3                     # seconds until the mock button 'presses'
JOB_S = 8                       # seconds a mock update job runs
CURVE_WAIT_S, CURVE_RECORD_S = 3, 12
# A mock update job walks its kind's phases (the daemon's own strings)
# evenly over JOB_S seconds.
JOB_PHASES = {
    'download': ('downloading', 'verifying signature'),
    'apply': ('taking update lock', 'verifying archive',
              'unmounting target', 'writing slot', 'verifying written slot'),
    'restore': ('taking update lock', 'verifying archive checksum',
                'unmounting target', 'writing factory image',
                'verifying written slot'),
    'factory-return': ('taking update lock', 'verifying archive checksum',
                       'writing factory image', 'verifying written slot',
                       'selecting the factory slot for the next boot',
                       'rebooting into the factory firmware'),
}

# A mock diagnostic walks these phases (the daemon's own phase strings)
# by elapsed second, then ends with the canned result for its tool.
DIAG_PLANS = {
    'flow-verify': (
        (3, 'stopping the motion controller'),
        (10, 'settling before trial 1/1 (pump on)'),
        (20, 'trial 1/1: heater 40% for 50 s (pump on)'),
        (27, 'settling before trial 1/1 (pump off)'),
        (37, 'trial 1/1: heater 40% for 50 s (pump off)'),
        (40, 'standing down'),
    ),
    'flow-calibrate': (
        (3, 'stopping the motion controller'),
        (8, 'settling before trial 1/3 (pump on)'),
        (14, 'trial 1/3: heater 40% for 50 s (pump on)'),
        (20, 'trial 1/3: heater 40% for 50 s (pump off)'),
        (26, 'trial 2/3: heater 40% for 50 s (pump on)'),
        (32, 'trial 2/3: heater 40% for 50 s (pump off)'),
        (38, 'trial 3/3: heater 40% for 50 s (pump on)'),
        (44, 'trial 3/3: heater 40% for 50 s (pump off)'),
        (47, 'standing down'),
    ),
    'aa-offset-calibrate': (
        (3, 'stopping the motion controller'),
        (10, 'settling with the fans idle'),
        (16, 'cycle 1/3: air assist to run'),
        (22, 'cycle 1/3: air assist to idle'),
        (28, 'cycle 2/3: air assist to run'),
        (34, 'cycle 2/3: air assist to idle'),
        (40, 'cycle 3/3: air assist to run'),
        (46, 'cycle 3/3: air assist to idle'),
        (49, 'standing down'),
    ),
}
DIAG_RESULTS = {
    'flow-verify': {'pass': True, 'threshold': 14.4, 'flow_rise': 11.8,
                    'flow_dt': 2.1, 'noflow_rise': 17.6, 'noflow_dt': 7.9,
                    'margin_flow': 2.6, 'margin_noflow': 3.2,
                    'thin_margin': False},
    'flow-calibrate': {'flow_rises': [11.6, 12.0, 12.0],
                       'noflow_rises': [17.6, 17.7, 18.1],
                       'flow_max': 12.0, 'noflow_min': 17.6, 'gap': 5.6,
                       'recommend': 14.8},
    'aa-offset-calibrate': {'steps': [[-19.5, -20.1], [20.3, 19.8],
                                      [-19.9, -20.4], [20.0, 19.6],
                                      [-20.2, -19.7], [19.8, 20.3]],
                            'offset_counts': 19.9, 'spread_counts': 0.8,
                            'recommend': 19.9},
}


def num(x):
    """A float as the daemon's %g prints it: 33 stays 33, 14.4 stays
    14.4 (JSON reads both as numbers; the text just matches)."""
    return int(x) if float(x) == int(x) else x


class Mock:
    """In-memory stand-in for the daemon: every endpoint it registers,
    in the JSON shape it serves, so the JavaScript runs end to end.
    State-changing calls are token-checked like the real thing, so a
    missing fx() shows up as the same 403 it would on the machine, and
    refusals come back the way the daemon sends them: text/plain from
    the handlers in main.c, {"error":...} from the update module and
    the token check.

    GF_MOCK_BUTTON=1 in the environment holds the machine button down
    (the fuse-identity viewer needs an operator at the machine)."""

    # The gate settings, one row per line of the table in src/gates.c:
    # key, the gate name /status reports (None for a key that tunes a
    # gate without being one), default, legal range, recommended band,
    # and which end of the range turns the gate off.
    # tests/test_devserver_mock.py holds this to the C table.
    GATES = (
        ('cool_temp_max', 'coolant_max', 33.0, 5.0, 60.0, 25.0, 38.0, 'high'),
        ('cool_temp_resume', None, 31.0, 5.0, 59.0, 20.0, 36.0, 'none'),
        ('cool_temp_critical_c', 'coolant_critical', 38.0, 6.0, 70.0, 36.0, 45.0, 'high'),
        ('cool_temp_min', 'coolant_min', 5.0, 0.0, 40.0, 3.0, 8.0, 'low'),
        ('cool_temp_start', 'warm_up', 16.0, 0.0, 40.0, 12.0, 20.0, 'low'),
        ('cool_tec_on_c', None, 20.0, 6.0, 32.0, 18.0, 24.0, 'none'),
        ('cool_tec_off_c', None, 18.0, 5.0, 31.0, 16.0, 22.0, 'none'),
        ('cool_fire_q1_alert', 'flame_q1_alert', 275.0, 0.0, 1023.0, 250.0, 450.0, 'low'),
        ('cool_fire_q1_critical', 'flame_q1_critical', 688.0, 0.0, 1023.0, 500.0, 1023.0, 'low'),
        ('cool_fire_q2_alert', 'flame_q2_alert', 374.0, 0.0, 1023.0, 300.0, 500.0, 'low'),
        ('cool_fire_q2_critical', 'flame_q2_critical', 1022.0, 0.0, 1023.0, 500.0, 1023.0, 'low'),
        ('cool_accel_x_alert', 'crash_x_alert', 132.0, 0.0, 255.0, 100.0, 170.0, 'low'),
        ('cool_accel_y_alert', 'crash_y_alert', 112.0, 0.0, 255.0, 85.0, 145.0, 'low'),
        ('cool_accel_abort', 'crash_abort', 133.0, 0.0, 255.0, 100.0, 170.0, 'low'),
        ('cool_flow_check_s', 'flow', 50.0, 0.0, 300.0, 30.0, 120.0, 'low'),
        ('cool_recheck_s', 'recheck', 150.0, 0.0, 3600.0, 60.0, 600.0, 'low'),
        ('cool_flow_rise', None, 14.4, 1.0, 40.0, 8.0, 16.0, 'none'),
        ('cool_tach_exhaust_min_rpm', 'exhaust', 6400.0, 0.0, 20000.0, 5800.0, 7000.0, 'low'),
        ('cool_tach_intake_min_rpm', 'intake', 2290.0, 0.0, 20000.0, 2100.0, 2500.0, 'low'),
        ('cool_tach_air_assist_min_rpm', 'air_assist', 6000.0, 0.0, 30000.0, 5500.0, 6600.0, 'low'),
        ('cool_purge_min_current', 'purge', 300.0, 0.0, 1023.0, 150.0, 500.0, 'low'),
        ('cool_fan_grace_s', None, 15.0, 0.0, 120.0, 5.0, 30.0, 'none'),
    )

    def __init__(self, token):
        self.token = token
        self.lock = threading.Lock()
        self.t0 = time.time()
        self.version = MOCK_VERSION
        self.machine_id = 'ABC-123'
        self.settings = dict.fromkeys(SETTINGS_KEYS, '')
        self.settings.update({'controller_mode': 'grbl',
                              'homing_mode': 'gfcloud'})
        # The supervisor (GET /mode): the selected mode, the controller
        # process state, its pid and the motion-liveness probe.
        self.mode = 'grbl'
        self.controller = 'running'
        self.pid = 412
        self.motion = 'verified'
        self.report_at = self.t0
        self.rep_armed = False
        self.button = os.environ.get('GF_MOCK_BUTTON') == '1'
        # The commissioning record (GET /wiz): the mock starts commissioned
        # unless GF_MOCK_FIRST_RUN=1, so the panel is what the dev server
        # shows by default and the wizard on request.
        first = os.environ.get('GF_MOCK_FIRST_RUN') == '1'
        self.docs = {}
        for d in ADVISORY_DOCS:
            try:
                with open(os.path.join(ROOT, 'docs', 'advisories', d + '.md'),
                          'rb') as f:
                    raw = f.read()
            except OSError:
                raw = ('# %s\n\nRevision: 0\n\n(mock document)\n' % d).encode()
            self.docs[d] = (raw, hashlib.sha256(raw).hexdigest())
        self.dark = {'id': '', 'running': False, 'started': 0.0, 'answered': 0.0,
                     'seq': 0, 'answer': None, 'error': '', 'aborted': False}
        self.wiz = {
            'accepted': set() if first else set(ADVISORY_DOCS),
            'pressed': not first,
            'account': None if first else 'owner',
            'reset': False,
            'versions': {} if first else {w: 1 for w, _ in WIZARDS},
            'flags': {},            # wizard id -> (level, reason), the what-changed menu
            'completed': not first,
            'button': 'idle', 'press_at': None,
            'machine': {'model': None, 'tec': False},
            'ssh': False,
            'camkey': MOCK_CAMKEY,
        }
        # GET /status, in the daemon's key order; diag, grbl and
        # gates_off are filled in per request.
        self.status = {
            'lens': {'edge_z': 3.48, 'below': 14, 'above': 20, 'stops_found': True,
                     'reach_min': -1.31, 'reach_max': 10.32},
            'state': 'idle', 'homed': True, 'diag': False,
            'pos': {'x': 12.34, 'y': -5.6, 'z': 0.0},
            'laser_locked': True,
            'laser': {'emission_samples': 0, 'pgood_samples': 255},
            'faults': 0, 'hv_current_raw': 0, 'lid_ir': [2, 2, 3, 2],
            'fans': {'air_assist': 1990, 'exhaust': 0, 'intake_1': 720,
                     'intake_2': 730},
            'coolant': {'down_c': 22.4, 'up_c': 22.3, 'pump': True,
                        'tec': False},
            'temps': {'chassis_c': 29.0, 'supply_raw': 589, 'soc_c': 42.8,
                      'soc_throttle': 0},
            'sys': {'cpu_pct': 7.4, 'mem_pct': 38.2},
            'gfsvc': {'latest': '2.6.0', 'tested': '2.6.0'},
            'switches': {'lid': True, 'button': False, 'interlock_ok': True,
                         'head': True, 'hv_enable': False},
        }
        # The controller's grbl.state report, embedded verbatim by the
        # daemon while the GRBL controller runs (ts_mono is stamped per
        # request).
        self.grbl_report = {
            'state': 'Idle', 'alarm': 0,
            'sender': {'connected': False, 'generation': 3, 'for_s': 0,
                       'peer': ''},
            'laser': {'armed': False, 'arming': False, 'model': 'density',
                      'floor_pct': 10, 'curve': 'off', 'gamma': 2.0},
            'modals': '[GC:G0 G54 G17 G21 G91 G94 M5 M9 T0 F600 S500.]',
            'overrides': {'feed': 100, 'rapid': 100},
            'driver': '260809',
        }
        self.diag = {
            'running': False, 'tool': 'flow-calibrate', 'phase': 'done',
            'elapsed_s': 0, 'down_c': 24.1, 'up_c': 23.9,
            'log': ['  0:00  start: duty 40%, window 50 s, '
                    'threshold 14.4 C',
                    '  8:44  bands: flow <= 12.0, no-flow >= 17.6 '
                    '(gap 5.6)',
                    '  8:47  done'],
            'result': DIAG_RESULTS['flow-calibrate'],
        }
        self.diag_t0 = 0.0
        self.slots = {
            'booted': SLOT_TARGETS['a'],
            'env': {'mmcdev': '2', 'mmchwpart': '0', 'mmcpart': '1',
                    'mmcroot': SLOT_TARGETS['a']},
            'slots': {
                'a': {'device': SLOT_TARGETS['a'], 'present': 'yes',
                      'state': 'ok', 'type': 'forgefirm',
                      'version': MOCK_VERSION, 'kernel': 'yes',
                      'booted': True, 'next': True},
                'b': {'device': SLOT_TARGETS['b'], 'present': 'yes',
                      'state': 'ok', 'type': 'factory', 'version': 'v2.6.0',
                      'kernel': 'yes', 'booted': False, 'next': False},
            },
            'archives': [{'file': 'factory-rootfs-20260101000000.img.gz',
                          'bytes': 210000000, 'version': '2.6.0',
                          'date': '2026-01-01'}],
            'staged': {'download': {'present': False, 'bytes': 0,
                                    'version': ''},
                       'upload': {'present': False, 'bytes': 0,
                                  'version': ''}},
        }
        self.update = {'running': False, 'kind': '', 'phase': '',
                       'elapsed': 0, 'progress_bytes': -1, 'result': None}
        self.job_t0 = 0.0
        self.job_args = {}
        self.curve = {'state': 'idle', 'reason': '', 'elapsed_s': 0,
                      'samples': 0, 'curve': '', 'points': []}
        self.curve_t0 = 0.0
        self.logtext = {lg: '' for lg in LOGGERS}
        self.logtext['forgectrl'] = (
            'Jan  1 00:00:01 forgectrl: auth: generated a new panel token\n'
            'Jan  1 00:00:01 forgectrl: super: grbl controller started '
            'pid 412\n'
            'Jan  1 00:00:02 forgectrl: cool: engine up, pump on\n')

    # -- gates (the table above, read the way gates.c reads it)
    def gate_row(self, key):
        for row in self.GATES:
            if row[0] == key:
                return row
        return None

    def gate_value(self, row):
        """The stored value inside its legal range, else the default
        (gate_parse: an empty or out-of-range value is no value)."""
        try:
            v = float(self.settings.get(row[0]) or '')
        except ValueError:
            return row[2]
        return v if row[3] <= v <= row[4] else row[2]

    def gate(self, key):
        return self.gate_value(self.gate_row(key))

    @staticmethod
    def gate_state(row, v):
        """gate_state in gates.c: off wins over warn."""
        lo, hi, blo, bhi, off = row[3:]
        if (off == 'low' and v <= lo) or (off == 'high' and v >= hi):
            return 'off'
        if v < blo or v > bhi:
            return 'warn'
        return 'ok'

    def gates_json(self):
        out = {}
        for row in self.GATES:
            key, gate, default, lo, hi, blo, bhi, off = row
            v = self.gate_value(row)
            out[key] = {'gate': gate, 'def': num(default), 'lo': num(lo),
                        'hi': num(hi), 'band': [num(blo), num(bhi)],
                        'off': off, 'value': num(v),
                        'state': self.gate_state(row, v)}
        return out

    def gates_off(self):
        return [row[1] for row in self.GATES
                if row[1] and self.gate_state(row, self.gate_value(row))
                == 'off']

    # -- documents
    def settings_reply(self):
        out = {}
        for k in SETTINGS_KEYS:
            if k in SECRET_KEYS:
                out[k + '_set'] = bool(self.settings[k])
            else:
                out[k] = self.settings[k]
        out['gates'] = self.gates_json()
        out['version'] = self.version
        out['machine_id'] = self.machine_id
        out['tls_fingerprint'] = MOCK_FINGERPRINT
        return out

    def setting_valid(self, key, v):
        """The daemon's validators, as far as the mock mirrors them: the
        gate table's legal ranges, the enumerated keys and the log
        levels. The other numeric keys pass unchecked."""
        row = self.gate_row(key)
        if row:
            try:
                f = float(v)
            except ValueError:
                return False
            return row[3] <= f <= row[4]
        if key in SETTING_CHOICES:
            return v in SETTING_CHOICES[key]
        if key.startswith('log_'):
            return v in LOG_LEVELS
        return True

    def idle(self):
        return self.status['state'] == 'idle'

    def _license_bundle(self):
        """A small tar.gz shaped like the image's: the manifest and one
        generic license text under common-licenses/."""
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode='w:gz') as t:
            for name, text in (('common-licenses/license.manifest', MOCK_MANIFEST),
                               ('common-licenses/generic_MIT', 'MIT License (mock)\n')):
                data = text.encode()
                ti = tarfile.TarInfo(name)
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data))
        return buf.getvalue()

    def _camkey_reply(self, headers):
        host = (headers.get('Host') or '127.0.0.1').split(':')[0]
        k = self.wiz['camkey']
        return {'key': k,
                'stream': 'http://%s/?action=stream&key=%s' % (host, k),
                'snapshot': 'http://%s/cam/snapshot?cam=lid&key=%s' % (host, k),
                'stream_https': 'https://%s/cam/stream?cam=lid&key=%s' % (host, k)}

    def wiz_missing(self):
        """The wizards the gate waits for: never run, or flagged required."""
        w = self.wiz
        return [k for k, _ in WIZARDS if not w['versions'].get(k)
                or w['flags'].get(k, ('',))[0] == 'required']

    def wiz_gate_open(self):
        w = self.wiz
        return (len(w['accepted']) == len(ADVISORY_DOCS) and w['pressed']
                and w['account'] and not w['reset'] and not self.wiz_missing())

    def wiz_why(self):
        w = self.wiz
        if len(w['accepted']) < len(ADVISORY_DOCS) or not w['pressed']:
            return 'the advisories are not accepted'
        if not w['account'] or w['reset']:
            return 'no account exists'
        missing = self.wiz_missing()
        return 'commissioning required: ' + ', '.join(missing) if missing else ''

    def wiz_record(self):
        w = self.wiz
        return {'schema': 1, 'sheet_id': MOCK_SHEET_ID, 'created': '2026-09-04T20:00:00Z',
                'completed': '2026-09-06T18:55:00Z' if w['completed'] else None,
                'advisories': {d: {'hash': self.docs[d][1], 'accepted': '2026-09-04T20:01:00Z',
                                   'method': ADVISORY_META[d][1]} for d in w['accepted']},
                'acceptance': {'pressed_at': '2026-09-04T20:03:00Z'} if w['pressed'] else {},
                'account': {'name': w['account'], 'uid': 1000,
                            'created': '2026-09-04T20:04:00Z'} if w['account'] else {},
                'machine': {'model': w['machine']['model'], 'tec': w['machine']['tec'],
                            'camera': 'ov5648', 'firmware': self.version, 'build_kind': 'dev',
                            'head': {'hw_id': '0x3', 'serial_hash': 'K6JXB-KMRVF',
                                     'version': '0x10'}},
                'wizards': {k: {'version': v, 'completed': '2026-09-06T13:24:00Z',
                                'result': DARK_RESULTS.get(k, {}),
                                'applied': DARK_APPLIED.get(k, {})}
                            for k, v in w['versions'].items()},
                'flags': {k: {'level': lv, 'reason': rs} for k, (lv, rs) in w['flags'].items()}}

    def wiz_record_html(self):
        """A stand-in for recordhtml.c: the same sections, plain."""
        rec = self.wiz_record()
        titles = dict(WIZARDS)
        h = ['<!doctype html><html lang="en"><head><meta charset="utf-8">'
             '<title>Commissioning record %s</title><style>body{font:14px/1.5 sans-serif;'
             'padding:28px 32px;max-width:900px}h2{border-bottom:1px solid #ddd}'
             'th{text-align:left;color:#666;font-weight:500;padding-right:14px}</style></head>'
             '<body><h1>ForgeFIRM commissioning record</h1><p>Sheet id <b>%s</b> &middot; '
             'firmware %s (mock)</p>' % (MOCK_SHEET_ID, MOCK_SHEET_ID, self.version)]
        h.append('<h2>Operator acknowledgment</h2><table>')
        for d in ADVISORY_DOCS:
            a = rec['advisories'].get(d)
            h.append('<tr><th>%s</th><td>%s</td></tr>' % (
                ADVISORY_META[d][0], a['accepted'] if a else 'not accepted'))
        h.append('</table><h2>The steps</h2>')
        for k, _ in WIZARDS:
            e = rec['wizards'].get(k)
            if not e:
                continue
            h.append('<h3>%s <small>v%d, %s</small></h3>' % (titles[k], e['version'], e['completed']))
            f = rec['flags'].get(k)
            if f:
                h.append('<p style="color:#a33">Asked for again (%s): %s</p>' % (f['level'], f['reason']))
            r = e['result'] or {}
            if r.get('summary'):
                h.append('<p>%s</p>' % r['summary'])
            if e['applied']:
                h.append('<div><b>Written to the settings</b>' + ''.join(
                    '<div><code>%s</code> = %s</div>' % (kk, vv.get('to'))
                    for kk, vv in e['applied'].items()) + '</div>')
        h.append('</body></html>')
        return ''.join(h)

    def wiz_reply(self):
        w = self.wiz
        if w['button'] == 'waiting' and w['press_at'] and \
                time.time() - w['press_at'] >= PRESS_S:
            w['button'] = 'pressed'
        docs = []
        for d in ADVISORY_DOCS:
            title, consent, phrase = ADVISORY_META[d]
            docs.append({'id': d, 'title': title, 'consent': consent,
                         'phrase': phrase, 'hash': self.docs[d][1],
                         'accepted': d in w['accepted']})
        gate = self.wiz_gate_open()
        return {
            'first_run': not w['completed'],
            'gate': 'open' if gate else 'closed', 'override': False,
            'why': '' if gate else self.wiz_why(),
            'advisories_complete': len(w['accepted']) == len(ADVISORY_DOCS),
            'acceptance_done': w['pressed'], 'account': bool(w['account']),
            'completed': w['completed'], 'sheet_id': MOCK_SHEET_ID,
            'required': [k for k, _ in WIZARDS if not w['versions'].get(k)] +
                        [{'id': k, 'reason': rs} for k, (lv, rs) in w['flags'].items()
                         if lv == 'required'],
            'recommended': [{'id': k, 'reason': rs} for k, (lv, rs) in w['flags'].items()
                            if lv == 'recommended'],
            'versions': dict(w['versions']),
            'changes': [{'id': c, 'title': t, 'wizards': [{'id': i, 'level': l} for i, l in wz]}
                        for c, t, _, wz in CHANGES],
            'documents': docs,
            'wizards': [{'id': k, 'title': t, 'version': 1,
                         'class': 'dark' if k in DARK else 'live' if k in LIVE else 'form',
                         'done': w['versions'].get(k, 0)} for k, t in WIZARDS],
            'dark': self.dark_reply(),
            'users': {'exists': bool(w['account']) and not w['reset'],
                      'reset_pending': w['reset'],
                      'name': w['account'] or ''},
            'button': w['button'], 'tls_fingerprint': MOCK_FINGERPRINT,
            'session': True,
            'clock': {'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                      'set': True},
            'machine': {'firmware': self.version, 'build': 'dev',
                        'camera': 'ov5648',
                        'head': {'present': True, 'hw_id': '0x3',
                                 'version': '0x10', 'serial_hash': 'K6JXB-KMRVF'},
                        'model': w['machine']['model'], 'tec': w['machine']['tec']},
        }

    def mode_reply(self):
        gated = not self.wiz_gate_open()
        return {'mode': self.mode,
                'controller': 'gated' if gated and self.controller != 'running'
                else self.controller,
                'pid': self.pid, 'motion': self.motion,
                'gated': gated, 'local': False,
                'why': self.wiz_why() if gated else ''}

    def status_reply(self):
        self.status['diag'] = self.diag['running']
        self.status['switches']['button'] = self.button
        grbl = None
        if self.mode == 'grbl' and self.controller == 'running':
            report = {'ts_mono': round(time.monotonic() - 1.2, 3)}
            report.update(self.grbl_report)
            grbl = {'age_s': 1.2, 'report': report}
        out = {}
        for k, v in self.status.items():
            if k == 'temps' and grbl:
                out['grbl'] = grbl
            if k == 'switches':
                out['gates_off'] = self.gates_off()
            out[k] = v
        return out

    def cool_reply(self):
        fans = self.status['fans']
        readings = dict(fans, purge=627)
        floors = {'exhaust': self.gate('cool_tach_exhaust_min_rpm'),
                  'intake_1': self.gate('cool_tach_intake_min_rpm'),
                  'intake_2': self.gate('cool_tach_intake_min_rpm'),
                  'air_assist': self.gate('cool_tach_air_assist_min_rpm'),
                  'purge': self.gate('cool_purge_min_current')}
        fan_gates = {}
        for n in FAN_NAMES:
            fan_gates[n] = {'reading': readings[n], 'floor': num(floors[n]),
                            'state': 'off' if floors[n] <= 0 else 'idle'}
        age = -1 if self.report_at is None else \
            round(time.time() - self.report_at, 1)
        watching = any(self.gate(k) > 0 for k in FIRE_GATES)
        c = self.status['coolant']
        return {
            'phase': 'idle', 'verdict': 'OK', 'fire_ok': True,
            'hold': False, 'resume_ok': True, 'reason': '',
            'down_c': c['down_c'], 'up_c': c['up_c'],
            'report_age_s': age, 'armed': self.rep_armed,
            'fire_watch': 'armed' if watching else 'watch',
            'accel_watch': 'watch',
            'quiet_hold': getattr(self, 'quiet_hold', False),
            'gates_off': self.gates_off(),
            'limits': {
                'coolant_max_c': self.gate('cool_temp_max'),
                'coolant_resume_c': self.gate('cool_temp_resume'),
                'coolant_critical_c': self.gate('cool_temp_critical_c'),
                'coolant_source': 'local',
                'coolant_min_c': self.gate('cool_temp_min'),
                'exhaust_min_rpm': num(floors['exhaust']),
                'intake_min_rpm': num(floors['intake_1']),
                'air_assist_min_rpm': num(floors['air_assist'])},
            'fan_gates': fan_gates}

    def cam_reply(self):
        return {'running': False, 'cam': 'lid', 'clients': 0, 'frames': 0,
                'fps': 0.0, 'fps_cap': 15.0, 'hw_fps_skip': False,
                'encoder': 'vpu', 'convert': 'gpu', 'buffers': 'cached',
                'sensor': 'OV5648',
                'stream': {'width': 1296, 'height': 972},
                'snapshot': {'width': 2592, 'height': 1944},
                'h264': {'active': False, 'clients': 0},
                'health': {'captured': 0, 'corrupt': 0, 'restarts': 0},
                'capture_allowed': self.status['switches']['lid'],
                'stopped_by_lid': False}

    def _logs_json(self):
        loggers = []
        for lg in LOGGERS:
            d = self.settings.get('log_%s_disk' % lg) or 'info'
            r = self.settings.get('log_%s_remote' % lg) or 'off'
            loggers.append({'name': lg, 'disk': d, 'remote': r,
                            'effective_disk': d, 'effective_remote': r,
                            'bytes': len(self.logtext[lg]),
                            'files': 1 if self.logtext[lg] else 0})
        return {'root': LOGS_ROOT, 'levels': list(LOG_LEVELS),
                'loggers': loggers,
                'syslog_server': self.settings['syslog_server'],
                'syslog_port': self.settings['syslog_port'] or '514',
                'syslog_proto': self.settings['syslog_proto'] or 'udp',
                'effective_syslog_server': self.settings['syslog_server'],
                'effective_known': True, 'pending_reboot': False}

    def _tail_json(self, q):
        name = q.get('name', 'forgectrl')
        text = self.logtext.get(name)
        if text is None:
            return None
        try:
            lines = max(1, int(q.get('lines', '200')))
        except ValueError:
            lines = 200
        try:
            frm = int(q.get('from', '-1'))
        except ValueError:
            frm = -1
        if not text:
            return {'name': name, 'size': 0, 'offset': 0, 'text': '',
                    'truncated': False, 'exists': False}
        if frm >= 0:
            chunk = text[frm:]
            off = frm
        else:
            parts = text.splitlines(True)[-lines:]
            chunk = ''.join(parts)
            off = len(text) - len(chunk)
        return {'name': name, 'size': len(text), 'offset': off,
                'text': chunk, 'truncated': False, 'exists': True}

    def _export_targz(self):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode='w:gz') as tf:
            for lg, text in self.logtext.items():
                data = text.encode()
                ti = tarfile.TarInfo('forgefirm-logs/%s/%s.log' % (lg, lg))
                ti.size = len(data)
                ti.mtime = int(time.time())
                tf.addfile(ti, io.BytesIO(data))
        return buf.getvalue()

    def _authorized(self, headers, q):
        tok = headers.get('X-ForgeFIRM-Token') or q.get('token')
        return tok == self.token

    def _log(self, line):
        self.logtext['forgectrl'] += 'Jan  1 %s forgectrl: %s\n' % (
            time.strftime('%H:%M:%S'), line)

    # -- the clock: mock jobs advance by wall time on every request
    def _diag_log(self, el, line):
        self.diag['log'].append('%3d:%02d  %s' % (el // 60, el % 60, line))

    def _diag_end(self, el, result):
        self._diag_log(el, 'done')
        self.diag.update({'running': False, 'phase': 'done',
                          'elapsed_s': 0, 'result': result})
        self.controller = 'running'
        self.pid += 1
        self._log('diag: %s done' % self.diag['tool'])

    def _tick(self):
        now = time.time()
        d = self.diag
        if d['running']:
            el = int(now - self.diag_t0)
            d['elapsed_s'] = el
            phase = None
            for t_end, name in DIAG_PLANS[d['tool']]:
                if el < t_end:
                    phase = name
                    break
            if phase is None:
                self._diag_end(el, DIAG_RESULTS[d['tool']])
            elif phase != d['phase']:
                d['phase'] = phase
                self._diag_log(el, phase)
        u = self.update
        if u['running']:
            el = int(now - self.job_t0)
            u['elapsed'] = el
            phases = JOB_PHASES[u['kind']]
            u['phase'] = phases[min(el * len(phases) // JOB_S,
                                    len(phases) - 1)]
            if u['kind'] != 'download':
                u['progress_bytes'] = min(el, JOB_S) * 25000000
            if el >= JOB_S:
                self._job_end()
        c = self.curve
        if c['state'] in ('waiting', 'recording'):
            el = int(now - self.curve_t0)
            c['elapsed_s'] = el
            if c['state'] == 'waiting' and el >= CURVE_WAIT_S:
                c['state'] = 'recording'
            if c['state'] == 'recording':
                c['samples'] = (el - CURVE_WAIT_S) * 8
                if el >= CURVE_WAIT_S + CURVE_RECORD_S:
                    c.update({'state': 'done', 'elapsed_s': 0,
                              'curve': '10:0.5,20:3,40:18,60:44,80:72,100:100',
                              'points': [{'density': 10, 'light': 0.5},
                                         {'density': 20, 'light': 3.0},
                                         {'density': 40, 'light': 18.0},
                                         {'density': 60, 'light': 44.0},
                                         {'density': 80, 'light': 72.0},
                                         {'density': 100, 'light': 100.0}]})

    def _job_end(self):
        u, a = self.update, self.job_args
        slots = self.slots['slots']
        if u['kind'] == 'download':
            self.slots['staged']['download'] = {
                'present': True, 'bytes': 87654321, 'version': MOCK_RELEASE}
            result = {'ok': True, 'file': 'download',
                      'version': MOCK_RELEASE, 'bytes': 87654321}
        elif u['kind'] == 'apply':
            ver = self.slots['staged'][a['file']]['version']
            slots[a['slot']].update({'type': 'forgefirm', 'version': ver,
                                     'state': 'ok', 'kernel': 'yes'})
            result = {'ok': True, 'slot': a['slot'], 'version': ver,
                      'signed': True}
        elif u['kind'] == 'factory-return':
            ver = a['archive']['version']
            slots[a['slot']].update({'type': 'factory', 'version': 'v' + ver,
                                     'state': 'ok', 'kernel': 'yes',
                                     'next': True})
            result = {'ok': True, 'slot': a['slot'], 'restored': True,
                      'factory_version': ver, 'rebooting': True}
        else:
            ver = a['archive']['version']
            slots[a['slot']].update({'type': 'factory', 'version': 'v' + ver,
                                     'state': 'ok', 'kernel': 'yes'})
            result = {'ok': True, 'slot': a['slot'], 'factory_version': ver}
        u.update({'running': False, 'phase': '', 'elapsed': 0,
                  'progress_bytes': -1, 'result': result})
        self._log('update: %s done' % u['kind'])

    def _job_start(self, kind, args, J):
        if self.update['running']:
            return J(409, {'error': 'an update job is already running'})
        if not self.idle():
            return J(409, {'error': 'machine is not idle'})
        if self.diag['running']:
            return J(409, {'error': 'a diagnostic is running'})
        self.update.update({'running': True, 'kind': kind,
                            'phase': JOB_PHASES[kind][0], 'elapsed': 0,
                            'progress_bytes': -1, 'result': None})
        self.job_t0 = time.time()
        self.job_args = args
        self._log('update: %s started' % kind)
        return J(202, {'started': True})

    def _slot_target(self, form, J):
        """The a/b slot an apply or restore writes, or the daemon's
        refusal (the booted slot and the next-boot slot are never
        written)."""
        slot = form.get('slot', '')
        s = self.slots['slots'].get(slot)
        if slot not in ('a', 'b') or not s:
            return None, J(400, {'error': 'slot must be a or b'})
        if s['booted']:
            return None, J(409, {'error':
                                 'refusing to write the booted root slot'})
        if s['next']:
            return None, J(409, {'error':
                                 'refusing to write the slot selected for '
                                 'the next boot - point the next boot back '
                                 'at the running slot first'})
        return slot, None

    # -- dispatch: returns (status, headers dict, body bytes)
    # -- the dark wizards: a time-driven stand-in for wizdark.c --------
    def dark_reply(self):
        d = self.dark
        now = time.time()
        if not d['id']:
            return {'id': '', 'running': False, 'owned': False, 'mine': True, 'phase': '',
                    'progress': 0, 'elapsed_s': 0, 'log': [], 'prompt': None, 'result': None,
                    'applied': None, 'error': '', 'shots': {'lid': False, 'head': False}}
        el = now - d['started']
        prompt = None
        result = None
        phase = 'starting'
        progress = min(int(el * 20), 30)
        if d['running'] and not d['aborted']:
            spec = DARK_PROMPTS.get(d['id'])
            # A 'wait' prompt is the machine's own switch edge: the mock
            # sees it two seconds later.
            if spec and spec[0] == 'wait' and el >= DARK_PROMPT_AT + 2.0 and d['answer'] is None:
                d['answer'] = ''
                d['answered'] = now
            if spec and el >= DARK_PROMPT_AT and d['answer'] is None:
                kind, pid, text, opts = spec
                if not d['seq']:
                    d['seq'] = 1
                prompt = {'seq': d['seq'], 'id': pid, 'kind': kind, 'text': text, 'options': opts,
                          'timeout_s': 600, 'since_s': int(el - DARK_PROMPT_AT)}
                phase = text
                progress = 30
            elif (not spec and el >= DARK_PROMPT_AT + DARK_DONE_AFTER) or \
                    (spec and d['answer'] is not None and now - d['answered'] >= DARK_DONE_AFTER):
                d['running'] = False
                self.wiz['versions'][d['id']] = 1
                self._log('wiz %s: complete' % d['id'])
            elif spec and d['answer'] is not None:
                phase = 'finishing'
                progress = 30 + int((now - d['answered']) / DARK_DONE_AFTER * 70)
        if not d['running']:
            progress = 100 if not d['aborted'] else 0
            phase = ''
            if not d['aborted']:
                result = DARK_RESULTS.get(d['id'], {})
        # GF_MOCK_MIRROR=1 shows the run as another browser's (the mirror).
        mirror = d['running'] and os.environ.get('GF_MOCK_MIRROR') == '1' and not d.get('taken')
        return {'id': d['id'], 'running': d['running'], 'owned': bool(d['running']),
                'mine': not mirror, 'phase': phase, 'progress': progress,
                'elapsed_s': int(el), 'log': ['00:00 started', '00:01 %s' % phase] if phase else
                ['00:00 started'], 'prompt': prompt, 'result': result,
                'applied': DARK_APPLIED.get(d['id'], {}) if result is not None else None,
                'error': 'aborted' if d['aborted'] else d['error'],
                'shots': {'lid': d['id'] == 'cameras', 'head': d['id'] == 'cameras'}}

    def _dark_post(self, path, form, J):
        m = re.match(r'^/wiz/([a-z.:-]+)/(start|answer|abort|takeover)$', path)
        if not m:
            return None
        wid, action = m.group(1), m.group(2)
        d = self.dark
        w = self.wiz
        if wid not in DARK and wid not in LIVE:
            return J(404, {'error': 'no such wizard'})
        if action == 'takeover':
            d['taken'] = True
            return J(200, {'ok': True})
        if action in ('answer', 'abort') and not self.dark_reply()['mine']:
            return J(409, {'error': 'another browser is running this step; take it over to answer'})
        if action == 'start':
            if not w['pressed']:
                return J(409, {'error': 'accept the advisories first'})
            if d['running']:
                return J(409, {'error': 'a wizard is already running (%s)' % d['id']})
            self.dark = {'id': wid, 'running': True, 'started': time.time(), 'answered': 0.0,
                         'seq': 0, 'answer': None, 'error': '', 'aborted': False}
            self._log('wiz %s: started' % wid)
            return J(200, {'started': True, 'id': wid})
        if action == 'answer':
            reply = self.dark_reply()
            if not reply['prompt'] or str(reply['prompt']['seq']) != form.get('seq', ''):
                return J(409, {'error': 'no such prompt is open'})
            value = form.get('value', '')
            if reply['prompt']['kind'] == 'jog' and value != 'Set origin':
                self._log('wiz %s: jog %s' % (wid, value))
                return J(200, {'ok': True})
            d['answer'] = value
            d['answered'] = time.time()
            return J(200, {'ok': True})
        if d['running']:
            d['running'] = False
            d['aborted'] = True
            self._log('wiz %s: aborted' % wid)
        return J(200, {'ok': True})

    def _wiz_post(self, path, form, J):
        w = self.wiz
        dark = self._dark_post(path, form, J)
        if dark is not None:
            return dark
        if path == '/wiz/advisories/accept':
            d = form.get('doc')
            if d not in self.docs:
                return J(400, {'error': 'unknown document'})
            if form.get('hash') != self.docs[d][1]:
                return J(409, {'error': 'the document changed; read it again'})
            title, consent, phrase = ADVISORY_META[d]
            if consent == 'typed' and form.get('phrase') != phrase:
                return J(400, {'error': 'type the phrase exactly as shown'})
            w['accepted'].add(d)
            w['pressed'] = False
            return J(200, self.wiz_reply())
        if path == '/wiz/advisories/press':
            if len(w['accepted']) < len(ADVISORY_DOCS):
                return J(409, {'error': 'accept every document first'})
            w['button'] = 'waiting'
            w['press_at'] = time.time()
            return J(200, {'button': 'waiting'})
        if path == '/wiz/advisories/press/cancel':
            w['button'] = 'idle'
            return J(200, {'button': 'idle'})
        if path == '/wiz/account':
            if not w['pressed']:
                return J(409, {'error': 'accept the advisories first'})
            name, pw = form.get('name', ''), form.get('password', '')
            if not re.match(r'^[a-z][a-z0-9_-]{1,31}$', name):
                return J(400, {'error': 'the name can hold lowercase letters, '
                                        "digits, '-' and '_'"})
            if len(pw) < 8 or pw == name:
                return J(400, {'error': 'the password must be at least 8 characters'})
            w['account'] = name
            w['reset'] = False
            w['versions']['account'] = 1
            return 200, {'Content-Type': 'application/json',
                         'Set-Cookie': 'ffsid=' + '0' * 64 + '; Path=/; HttpOnly'
                         }, json.dumps({'ok': True, 'name': name}).encode()
        if path == '/wiz/preferences':
            for k in ('ui_units', 'wifi_country'):
                if k in form and form[k]:
                    if not self.setting_valid(k, form[k]):
                        return J(400, {'error': 'invalid value for ' + k})
                    self.settings[k] = form[k]
            w['versions']['preferences'] = 1
            return J(200, self.wiz_reply())
        if path == '/wiz/machine':
            if form.get('model') not in ('basic', 'plus', 'pro'):
                return J(400, {'error': 'model must be basic, plus, or pro'})
            tec = form.get('tec') == '1'
            if tec and form.get('model') != 'pro':
                return J(400, {'error': 'only a Pro has a thermoelectric cooler'})
            w['machine'] = {'model': form['model'], 'tec': tec}
            self.settings['cool_tec_present'] = '1' if tec else '0'
            w['versions']['machine'] = 1
            return J(200, self.wiz_reply())
        if path == '/wiz/cloud':
            if form.get('enabled') == '1':
                if form.get('phrase') != 'I UNDERSTAND':
                    return J(400, {'error': 'type I UNDERSTAND to turn cloud mode on'})
                self.settings['cloud_enabled'] = '1'
                self.settings['homing_mode'] = form.get('homing_mode', 'gfcloud')
            else:
                self.settings['cloud_enabled'] = '0'
                if self.settings['homing_mode'] == 'gfcloud':
                    self.settings['homing_mode'] = 'none'
                if self.settings['controller_mode'] == 'cloud':
                    self.settings['controller_mode'] = 'grbl'
                    self.mode = 'grbl'
            w['versions']['cloud'] = 1
            return J(200, self.wiz_reply())
        if path == '/wiz/complete':
            if not self.wiz_gate_open():
                return J(409, {'error': self.wiz_why()})
            w['completed'] = True
            return J(200, self.wiz_reply())
        if path == '/wiz/changed':
            for c, _, reason, wz in CHANGES:
                if c == form.get('what'):
                    for wid, level in wz:
                        if w['flags'].get(wid, ('',))[0] == 'required' and level != 'required':
                            continue
                        w['flags'][wid] = (level, reason)
                    self._log('commission: %s: the wizards it needs are flagged' % reason)
                    return J(200, self.wiz_reply())
            return J(400, {'error': 'what must name a change from the menu'})
        if path == '/system/ssh':
            e = form.get('enable')
            if e not in ('0', '1'):
                return J(400, {'error': 'enable must be 0 or 1'})
            w['ssh'] = e == '1'
            self._log('ssh: %s' % ('enabled until the next reboot' if w['ssh'] else 'disabled'))
            return J(200, {'enabled': w['ssh'], 'running': w['ssh'], 'dev_image': True})
        if path == '/system/camera-key':
            if form.get('rotate') != '1':
                return J(400, {'error': 'rotate=1 required'})
            w['camkey'] = hashlib.sha256(w['camkey'].encode()).hexdigest()[:32]
            self._log('camkey: camera key rotated')
            return J(200, self._camkey_reply({}))
        if path == '/restore/factory-return':
            if form.get('confirm') != '1':
                return J(400, {'error': 'confirm=1 required'})
            if self.update['running']:
                return J(409, {'error': 'an update job is running'})
            return self._job_start('factory-return',
                                   {'slot': 'b', 'archive': {'version': '2.6.0'}}, J)
        return J(404, {'error': 'mock: no such endpoint'})

    def handle(self, method, path, q, headers, body):
        with self.lock:
            self._tick()
            return self._handle(method, path, q, headers, body)

    def _handle(self, method, path, q, headers, body):
        J = lambda code, obj: (code, {'Content-Type': 'application/json'},
                               json.dumps(obj).encode())
        T = lambda code, msg: (code, {'Content-Type': 'text/plain'},
                               msg.encode())
        if method == 'GET':
            if path == '/settings':
                return J(200, self.settings_reply())
            if path == '/status':
                return J(200, self.status_reply())
            if path == '/mode':
                return J(200, self.mode_reply())
            if path == '/cam/status':
                return J(200, self.cam_reply())
            if path in ('/cam/snapshot', '/cam/stream') or (
                    path == '/' and q.get('action') in ('snapshot',
                                                        'stream')):
                svg = MOCK_SVG % time.strftime('%H:%M:%S')
                return 200, {'Content-Type': 'image/svg+xml'}, svg.encode()
            if path == '/cam/h264':
                return T(503, 'H.264 stream unavailable (the MJPEG stream '
                              'still works)')
            if path == '/grbl/settings':
                if not (self.mode == 'grbl' and self.controller == 'running'):
                    return T(404, 'no grbl controller')
                return T(200, '$0=10\n$1=255\n$32=1\n$35=10\n')
            if path == '/curve/status':
                return J(200, self.curve)
            if path == '/curve/ladder.gcode':
                return 200, {'Content-Type': 'text/plain',
                             'Content-Disposition':
                             'attachment; filename="dose-ladder.gcode"'
                             }, b'; mock ladder\nG21\nM3\nS1000\nM5\n'
            if path == '/diag/status':
                return J(200, self.diag)
            if path == '/cool/status':
                return J(200, self.cool_reply())
            if path == '/slots':
                return J(200, self.slots)
            if path == '/update/status':
                return J(200, self.update)
            # Token-gated reads: log content and the fuse identity.
            if path in ('/logs', '/logs/tail', '/fuse-identity'):
                if not self._authorized(headers, q):
                    return J(403, {'error': 'authentication required'})
            if path == '/logs':
                return J(200, self._logs_json())
            if path == '/logs/tail':
                if 'name' not in q:
                    return T(400, 'name required')
                tail = self._tail_json(q)
                if tail is None:
                    return T(404, 'unknown logger')
                return J(200, tail)
            if path == '/fuse-identity':
                if not self.button:
                    return T(403, 'hold the machine button to reveal the '
                                  'fuse identity')
                return J(200, {'serial': '123456789', 'hostname': 'ABC-123',
                               'password': '0123456789abcdef' * 4})
            if path == '/wiz':
                return J(200, self.wiz_reply())
            if path == '/wiz/dark':
                return J(200, self.dark_reply())
            if path == '/wiz/sheet.svg':
                card = q.get('card', '')
                if card not in LIVE:
                    return J(404, {'error': 'no such card'})
                return 200, {'Content-Type': 'image/svg+xml', 'Cache-Control': 'no-store'}, \
                    sheet_svg(card).encode()
            if path == '/wiz/sheet.gcode':
                card = q.get('card', '')
                if card not in LIVE:
                    return J(404, {'error': 'no such card'})
                body = ('; ForgeFIRM commissioning: %s (mock)\nM3 S400\nG0 X10 Y10\nG1 X190 Y10 F3000\n'
                        'G1 X190 Y140 F3000\nG1 X10 Y140 F3000\nG1 X10 Y10 F3000\nM5\n' % card)
                return 200, {'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store'}, \
                    body.encode()
            if path == '/wiz/shot':
                if self.dark['id'] != 'cameras':
                    return J(404, {'error': 'no snapshot yet'})
                svg = MOCK_SVG % ('%s camera' % q.get('cam', 'lid'))
                return 200, {'Content-Type': 'image/svg+xml', 'Cache-Control': 'no-store'}, svg.encode()
            if path == '/wiz/record':
                # The daemon takes a login session or the token; the mock
                # is always logged in.
                hdrs = {'Content-Type': 'application/json', 'Cache-Control': 'no-store'}
                if 'download' in q:
                    hdrs['Content-Disposition'] = \
                        'attachment; filename="forgefirm-commissioning-%s.json"' % MOCK_SHEET_ID
                return 200, hdrs, json.dumps(self.wiz_record(), indent=2).encode()
            if path == '/wiz/record.html':
                return 200, {'Content-Type': 'text/html; charset=utf-8',
                             'Cache-Control': 'no-store'}, self.wiz_record_html().encode()
            if path == '/wiz/advisories/press':
                r = self.wiz_reply()
                if self.wiz['button'] == 'pressed' and not self.wiz['pressed']:
                    self.wiz['pressed'] = True
                    self.wiz['versions']['advisories'] = 1
                    self.wiz['button'] = 'idle'
                    self._log('wiz: the advisories were accepted at the machine')
                return J(200, {'button': r['button'], 'accepted': self.wiz['pressed']})
            if path.startswith('/advisories/'):
                d = path[len('/advisories/'):]
                if d not in self.docs:
                    return J(404, {'error': 'no such document'})
                return 200, {'Content-Type': 'text/markdown; charset=utf-8',
                             'ETag': self.docs[d][1]}, self.docs[d][0]
            if path == '/system/ssh':
                if not self._authorized(headers, q):
                    return J(403, {'error': 'authentication required'})
                return J(200, {'enabled': self.wiz['ssh'], 'running': self.wiz['ssh'],
                               'dev_image': True})
            if path == '/system/camera-key':
                if not self._authorized(headers, q):
                    return J(403, {'error': 'authentication required'})
                return J(200, self._camkey_reply(headers))
            if path == '/system/licenses':
                return 200, {'Content-Type': 'application/gzip',
                             'Content-Disposition':
                             'attachment; filename="forgefirm-licenses-mock.tar.gz"'
                             }, self._license_bundle()
            if path == '/cert':
                return 200, {'Content-Type': 'text/html; charset=utf-8'}, (
                    '<!doctype html><title>ForgeFIRM certificate</title>'
                    '<h1>This machine\'s certificate</h1><dl><dt>SHA-256</dt><dd><code>'
                    + MOCK_FINGERPRINT + '</code></dd><dt>Names</dt><dd>forgefirm.local '
                    + self.machine_id + '.local</dd></dl>'
                    '<p><a href="/cert.pem">Download the certificate (PEM)</a></p>').encode()
            if path == '/cert.pem':
                return 200, {'Content-Type': 'application/x-pem-file'}, (
                    '-----BEGIN CERTIFICATE-----\nbW9jaw==\n-----END CERTIFICATE-----\n'
                    ).encode()
            if path == '/licenses':
                return 200, {'Content-Type': 'text/html; charset=utf-8'}, (
                    '<!doctype html><title>ForgeFIRM licenses</title><h1>Licenses</h1>'
                    '<p><a href="/system/licenses">download the license bundle (.tar.gz)</a></p>'
                    '<pre>' + MOCK_MANIFEST + '</pre>').encode()
            if path == '/system/licenses/manifest':
                return 200, {'Content-Type': 'text/plain; charset=utf-8'}, MOCK_MANIFEST.encode()
            if path in ('/setup', '/login'):
                return T(200, 'mock: the dev server serves this page from src/ui/')
            return J(404, {'error': 'mock: no such endpoint'})

        if method != 'POST':
            return J(405, {'error': 'method not allowed'})

        form = dict(q)
        ctype = headers.get('Content-Type', '')
        if body and ctype.startswith('application/x-www-form-urlencoded'):
            form.update(parse_qsl(body.decode('utf-8', 'replace'),
                                  keep_blank_values=True))
        form.pop('token', None)

        # The controller's job-state report: loopback-only on the
        # machine, no token.
        if path == '/cool/state':
            mode = form.get('mode')
            if mode is None:
                return T(400, 'mode is required')
            if mode not in ('idle', 'run', 'cooldown'):
                return T(400, 'mode must be idle, run or cooldown')
            self.report_at = time.time()
            self.rep_armed = form.get('armed', '0') not in ('', '0')
            return J(200, {'ok': True})

        # The quiet hold for a listening (the bench tools): every fan off,
        # with pump=1 the pump and the TEC too; idle machine only.
        if path == '/cool/quiet':
            on = form.get('on')
            pump = form.get('pump')
            if on not in ('0', '1'):
                return T(400, 'on must be 0 or 1')
            if pump is not None and pump not in ('0', '1'):
                return T(400, 'pump must be 0 or 1')
            self.quiet_hold = on == '1'
            return J(200, {'quiet_hold': self.quiet_hold})

        if path == '/login':
            w = self.wiz
            if w['account'] and form.get('name') == w['account'] and \
                    form.get('password') == 'correct horse':
                return 200, {'Content-Type': 'application/json',
                             'Set-Cookie': 'ffsid=' + '0' * 64 + '; Path=/; HttpOnly'
                             }, b'{"ok":true}'
            return J(401, {'error': 'wrong name or password'})
        if path == '/logout':
            return J(200, {'ok': True})

        if not self._authorized(headers, q):
            return J(403, {'error': 'authentication required'})

        if path.startswith('/wiz/') or path in ('/system/ssh', '/system/camera-key') or \
                path == '/restore/factory-return':
            return self._wiz_post(path, form, J)

        if path == '/settings':
            print('mock: POST /settings %s' % json.dumps(form), flush=True)
            if self.diag['running']:
                return T(409, 'a diagnostic is running - settings are '
                              'locked')
            if not self.idle():
                return T(409, 'machine is not idle - settings are locked')
            known = [k for k in SETTINGS_KEYS if k in form]
            if not known:
                return T(400, 'no known setting in request')
            for k in known:
                if form[k] and not self.setting_valid(k, form[k]):
                    return T(400, 'invalid value for %s' % k)
            # cloud_enabled is the cloud step's decision: on takes the
            # typed phrase here too, off sweeps the cloud choices
            if form.get('cloud_enabled') == '1' and self.settings.get('cloud_enabled') != '1' \
                    and form.get('phrase') != 'I UNDERSTAND':
                return T(400, 'type I UNDERSTAND to turn cloud mode on')
            for k in known:
                self.settings[k] = form[k]
                self._log('%s %s' % (k, 'cleared' if not form[k] else
                                     'set' if k in SECRET_KEYS else form[k]))
            if form.get('cloud_enabled') == '0':
                if 'homing_mode' not in form and self.settings.get('homing_mode') == 'gfcloud':
                    self.settings['homing_mode'] = 'none'
                    self._log('homing_mode none (cloud mode off)')
                if 'controller_mode' not in form and self.settings.get('controller_mode') == 'cloud':
                    self.settings['controller_mode'] = 'grbl'
                    self._log('controller_mode grbl (cloud mode off)')
            return J(200, self.settings_reply())
        if path == '/mode':
            m = form.get('controller')
            if m is None:
                return T(400, 'controller is required')
            if m not in ('grbl', 'cloud'):
                return T(400, 'mode must be grbl or cloud')
            if self.diag['running']:
                return T(409, 'a diagnostic is running')
            if self.update['running']:
                return T(409, 'an update job is running')
            if not self.idle():
                return T(409, 'machine is not idle')
            self.mode = m
            self.settings['controller_mode'] = m
            self.controller = 'running'
            self.pid += 1
            self.motion = 'unverified'
            self._log('super: mode -> %s' % m)
            return J(200, self.mode_reply())
        if path == '/controller/stop':
            self.controller = 'standby'
            self.pid = 0
            self.report_at = None
            self._log('super: controller stopped')
            return J(200, {'stopped': True})
        if path == '/controller/start':
            if self.diag['running']:
                return T(409, 'a diagnostic owns the hardware')
            self.controller = 'running'
            self.pid += 1
            self.report_at = time.time()
            self._log('super: controller started pid %d' % self.pid)
            return J(200, {'started': True})
        if path.startswith('/diag/') and path != '/diag/abort':
            tool = path[6:]
            if self.update['running']:
                return T(409, 'an update job is running')
            if tool not in DIAG_TOOLS:
                return T(400, 'unknown diagnostic')
            if self.diag['running']:
                return T(409, 'a diagnostic is already running')
            if not self.idle():
                return T(409, 'machine is not idle')
            self.diag.update({'running': True, 'tool': tool, 'phase': '',
                              'elapsed_s': 0, 'log': [], 'result': None})
            self.diag_t0 = time.time()
            self.controller = 'standby'
            self.pid = 0
            self._log('diag: %s started' % tool)
            return J(202, {'started': True})
        if path == '/diag/abort':
            if self.diag['running']:
                self._diag_end(self.diag['elapsed_s'],
                               {'error': 'aborted by operator'})
            return J(200, {'aborting': True})
        if path == '/curve/record':
            if self.curve['state'] in ('waiting', 'recording'):
                return T(409, 'a recording is already running')
            if self.grbl_report['sender']['connected']:
                return T(409, 'a sender is connected to the machine - close '
                              'it before recording')
            self.curve.update({'state': 'waiting', 'reason': '',
                               'elapsed_s': 0, 'samples': 0, 'curve': '',
                               'points': []})
            self.curve_t0 = time.time()
            return J(200, self.curve)
        if path == '/curve/stop':
            if self.curve['state'] == 'waiting':
                self.curve.update({'state': 'failed', 'elapsed_s': 0,
                                   'reason': 'stopped before the ladder '
                                             'fired'})
            elif self.curve['state'] == 'recording':
                self.curve_t0 -= CURVE_RECORD_S
                self._tick()
            return J(200, self.curve)
        if path == '/boot':
            if self.diag['running']:
                return J(409, {'error': 'a diagnostic is running'})
            if not self.idle():
                return J(409, {'error': 'machine is not idle'})
            if self.update['running']:
                return J(409, {'error': 'an update job is running'})
            t = form.get('target', '')
            if t not in SLOT_TARGETS:
                return J(400, {'error': 'target must be sd, a, b, or legacy'})
            if t not in self.slots['slots']:
                return J(409, {'error': 'boot selection failed',
                               'detail': '%s is not present' % t})
            for k, s in self.slots['slots'].items():
                s['next'] = (k == t)
            self.slots['env']['mmcroot'] = SLOT_TARGETS[t]
            return J(200, {'ok': True, 'target': t,
                           'detail': 'next boot: %s' % SLOT_TARGETS[t]})
        if path == '/system/reboot':
            if self.diag['running']:
                return J(409, {'error': 'a diagnostic is running'})
            if not self.idle():
                return J(409, {'error': 'machine is not idle'})
            if self.update['running']:
                return J(409, {'error': 'an update job is running'})
            if form.get('confirm') != '1':
                return J(400, {'error': 'confirm=1 required'})
            self._log('system: reboot requested (mock, no-op)')
            return J(200, {'rebooting': True})
        if path == '/update/check':
            return J(200, {'available': True, 'version': MOCK_RELEASE,
                           'current': self.version, 'new': True})
        if path == '/update/download':
            return self._job_start('download', {}, J)
        if path == '/update/apply':
            slot, err = self._slot_target(form, J)
            if err:
                return err
            f = form.get('file', '')
            if f not in ('download', 'upload'):
                return J(400, {'error': 'file must be download or upload'})
            if not self.slots['staged'][f]['present']:
                return J(404, {'error': 'staged archive not found'})
            return self._job_start('apply', {'slot': slot, 'file': f}, J)
        if path == '/update/upload':
            if self.update['running']:
                return J(409, {'error': 'an update job is running'})
            self.slots['staged']['upload'] = {
                'present': True, 'bytes': len(body), 'version': MOCK_RELEASE}
            return J(200, {'ok': True, 'file': 'upload', 'bytes': len(body),
                           'version': MOCK_RELEASE, 'signature': 'forgefirm'})
        if path == '/restore/factory':
            slot, err = self._slot_target(form, J)
            if err:
                return err
            f = form.get('file', '')
            if not f.startswith('factory-rootfs-'):
                return J(400, {'error': 'file must be a factory-rootfs '
                                        'archive name'})
            arch = [a for a in self.slots['archives'] if a['file'] == f]
            if not arch:
                return J(404, {'error': 'archive not found'})
            return self._job_start('restore',
                                   {'slot': slot, 'archive': arch[0]}, J)
        if path == '/logs/export':
            if not self.idle():
                return T(409, 'the machine is busy; export logs when it is '
                              'idle')
            full = form.get('sanitize', '1') in ('0', 'false', 'no')
            name = 'forgefirm-logs-%s%s.tar.gz' % (
                time.strftime('%Y%m%d-%H%M%S'), '-full' if full else '')
            return 200, {'Content-Type': 'application/gzip',
                         'Content-Disposition':
                         'attachment; filename="%s"' % name,
                         }, self._export_targz()
        return J(404, {'error': 'mock: no such endpoint'})


# ------------------------------------------------------------ server
class BoundedReader:
    """read(n) over a socket file, capped at Content-Length bytes so
    http.client can stream a request body upstream without waiting for
    a socket EOF that keep-alive never delivers."""

    def __init__(self, f, remaining):
        self.f = f
        self.remaining = remaining

    def read(self, n=-1):
        if self.remaining <= 0:
            return b''
        if n is None or n < 0 or n > self.remaining:
            n = self.remaining
        data = self.f.read(n)
        self.remaining -= len(data)
        return data


class Handler(BaseHTTPRequestHandler):
    server_version = 'forgectrl-devserver'
    protocol_version = 'HTTP/1.0'      # one request per connection

    # shared state, set by main()
    cfg = None
    panel = None
    mock = None
    verbose = False
    bundled = False

    # -- entry points
    def do_GET(self):
        self._route()

    def do_POST(self):
        self._route()

    def do_HEAD(self):
        self._route()

    def do_PUT(self):
        self._route()

    def do_DELETE(self):
        self._route()

    def log_message(self, fmt, *args):     # quiet the default access log
        pass

    def _say(self, msg):
        print('%s %s' % (time.strftime('%H:%M:%S'), msg), flush=True)

    # -- routing
    def _route(self):
        u = urlsplit(self.path)
        q = dict(parse_qsl(u.query, keep_blank_values=True))
        try:
            if u.path == '/__dev/watch':
                return self._watch(q)
            if u.path == '/' and 'action' not in q and \
                    self.command in ('GET', 'HEAD'):
                return self._panel()
            if u.path in ('/setup', '/login') and self.command in ('GET', 'HEAD'):
                return self._page('wizard.html' if u.path == '/setup' else 'login.html')
            if self.command in ('GET', 'HEAD') and not self.bundled and \
                    os.path.splitext(u.path)[1] in CONTENT_TYPES and \
                    self.panel.path(u.path[1:]):
                return self._file(u.path[1:])
            if self.cfg.mock:
                return self._mock(u.path, q)
            return self._proxy(u)
        except ConnectionError:
            pass                       # browser went away; nothing to do

    def _send(self, code, headers, body):
        self.send_response(code)
        for k, v in headers.items():
            self.send_header(k, v)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def _json(self, code, obj):
        self._send(code, {'Content-Type': 'application/json'},
                   json.dumps(obj).encode())

    def _watch(self, q):
        seen = q.get('v', '')
        v = self.panel.wait_change(seen, WATCH_HOLD_S)
        self._json(200, {'v': v})

    def _token_label(self):
        if self.cfg.mock:
            return self.mock.token, 'mock backend'
        token = self.cfg.token
        label = self.cfg.upstream()[2]
        if not token:
            label += ' (no GF_TOKEN: writes will be refused)'
        return token, label

    def _page(self, name):
        """The setup and login pages as the daemon serves them, with the
        token spliced; their CSS and JS come as files (dev default). """
        token, label = self._token_label()
        try:
            page = self.panel.text(name).replace(TOKEN_MARK, token or '', 1)
        except OSError as e:
            return self._send(500, {'Content-Type': 'text/plain'},
                              ('devserver: %s\n' % e).encode())
        badge = BADGE_HTML % {'bg': '#c7760a' if self.cfg.mock else '#3d854d',
                              'label': html_escape(label)}
        k = page.rfind('</body>')
        page = page + badge if k < 0 else page[:k] + badge + page[k:]
        self._send(200, {'Content-Type': 'text/html; charset=utf-8'},
                   page.encode('utf-8'))

    def _panel(self):
        token, label = self._token_label()
        try:
            page = self.panel.render(token, label, self.cfg.mock,
                                     self.bundled)
        except (OSError, ValueError) as e:
            self._say('panel: %s' % e)
            return self._send(500, {'Content-Type': 'text/plain'},
                              ('devserver: %s\n' % e).encode())
        if self.verbose:
            self._say('panel: served %d bytes%s'
                      % (len(page), ' (bundled)' if self.bundled else ''))
        self._send(200, {'Content-Type': 'text/html; charset=utf-8'},
                   page.encode('utf-8'))

    def _file(self, name):
        """A file from src/ui/, with the token substituted in text."""
        ext = os.path.splitext(name)[1]
        data = self.panel.read(name)
        if ext in ('.html', '.css', '.js'):
            token = self._token_label()[0]
            data = data.decode('utf-8').replace(TOKEN_MARK, token or '',
                                                1).encode('utf-8')
        if self.verbose:
            self._say('panel: %s (%d B)' % (name, len(data)))
        self._send(200, {'Content-Type': CONTENT_TYPES[ext]}, data)

    def _read_body(self):
        n = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(n) if n > 0 else b''

    def _mock(self, path, q):
        body = self._read_body() if self.command in ('POST', 'PUT') \
            else b''
        code, hdrs, out = self.mock.handle(self.command, path, q,
                                           self.headers, body)
        if self.verbose or self.command != 'GET' or code >= 400:
            self._say('mock  %s %s -> %d' % (self.command, self.path, code))
        self._send(code, hdrs, out)

    # -- proxy
    def _proxy(self, u):
        ip, port, disp = self.cfg.upstream()
        streaming = u.path in STREAM_PATHS or (
            u.path == '/' and 'action=stream' in u.query)
        timeout = None if streaming else PROXY_TIMEOUT_S

        hdrs = {}
        for k, v in self.headers.items():
            if k.lower() not in DROP_REQ_HEADERS:
                hdrs[k] = v
        hdrs['Host'] = '%s:%d' % (ip, port) if port != 80 else ip
        hdrs['Connection'] = 'close'

        body = None
        n = int(self.headers.get('Content-Length') or 0)
        if n > 0:
            body = BoundedReader(self.rfile, n)
            hdrs['Content-Length'] = str(n)
        elif self.command in ('POST', 'PUT'):
            hdrs['Content-Length'] = '0'

        t0 = time.time()
        if port == 80:
            conn = http.client.HTTPConnection(ip, port, timeout=timeout)
        else:
            # The machine signs its own certificate: no verification here,
            # the dev server talks to the address the operator typed.
            ctx = ssl._create_unverified_context()
            conn = http.client.HTTPSConnection(ip, port, timeout=timeout,
                                               context=ctx)
        try:
            conn.request(self.command, self.path, body=body, headers=hdrs)
            resp = conn.getresponse()
        except (OSError, http.client.HTTPException) as e:
            conn.close()
            self._say('proxy %s %s -> unreachable (%s: %s)'
                      % (self.command, self.path, disp, e))
            return self._json(502, {'error': 'devserver: cannot reach %s '
                                             '(%s)' % (disp, e)})

        self.send_response(resp.status, resp.reason)
        for k, v in resp.getheaders():
            if k.lower() not in DROP_RESP_HEADERS:
                self.send_header(k, v)
        clen = resp.getheader('Content-Length')
        if clen is not None and not resp.chunked:
            self.send_header('Content-Length', clen)
        self.send_header('Connection', 'close')
        self.end_headers()

        sent = 0
        closed = ''
        try:
            if self.command != 'HEAD':
                while True:
                    chunk = resp.read1(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    sent += len(chunk)
                    if streaming:
                        self.wfile.flush()
        except ConnectionError:
            closed = ', client closed'    # a stream ended by the browser
        except (OSError, http.client.HTTPException) as e:
            closed = ', upstream error: %s' % e
        finally:
            conn.close()

        if self.verbose or self.command != 'GET' or resp.status >= 400 \
                or streaming:
            self._say('proxy %s %s -> %d %s (%d B, %d ms%s)'
                      % (self.command, self.path, resp.status, resp.reason,
                         sent, int((time.time() - t0) * 1000), closed))


# -------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(
        description='forgectrl panel development server (live reload; '
                    'API proxied to a machine or served by a mock).')
    ap.add_argument('--port', type=int, default=DEFAULT_PORT,
                    help='listen port (default %d)' % DEFAULT_PORT)
    ap.add_argument('--bind', default='127.0.0.1',
                    help='listen address (default 127.0.0.1; use 0.0.0.0 '
                         'to reach it from outside a container)')
    ap.add_argument('--host', help='machine address[:port] (GF_HOST)')
    ap.add_argument('--token', help='panel token (GF_TOKEN)')
    ap.add_argument('--env', help='.env file (default: <repo>/.env)')
    ap.add_argument('--mock', action='store_true',
                    help='answer the API from the built-in mock even if '
                         'GF_HOST is set')
    ap.add_argument('--bundle', action='store_true',
                    help='serve the page bundled (CSS/JS inlined) as the '
                         'daemon does, instead of as three files')
    ap.add_argument('--dump', action='store_true',
                    help='print the bundled page and exit')
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='log every request, including the polls')
    args = ap.parse_args()

    panel = Panel(UI_DIR)
    if args.dump:
        try:
            sys.stdout.buffer.write(panel.page(True).encode('utf-8'))
        except (OSError, ValueError) as e:
            sys.exit('devserver: %s' % e)
        return

    cfg = Config(args)
    Handler.cfg = cfg
    Handler.panel = panel
    Handler.mock = Mock(MOCK_TOKEN)
    Handler.verbose = args.verbose
    Handler.bundled = args.bundle

    try:
        size = len(panel.page(True))
        state = '%d bytes bundled, served %s' % (
            size, 'bundled' if args.bundle else 'as files')
    except (OSError, ValueError) as e:
        state = 'ERROR: %s' % e
    print('forgectrl panel dev server')
    print('  panel   %s/ (%s)' % (os.path.relpath(UI_DIR, ROOT)
                                   .replace(os.sep, '/'), state))
    print('  env     %s%s' % (cfg.env_path,
                              '' if os.path.exists(cfg.env_path)
                              else ' (absent)'))
    if cfg.mock:
        print('  backend mock%s' % (' (--mock)' if args.mock else
                                     ' (no GF_HOST configured)'))
    else:
        print('  backend %s%s' % (cfg.upstream()[2],
                                  '' if cfg.token else
                                  '  ** no GF_TOKEN: state-changing calls '
                                  'will be refused **'))
    print('  serve   http://%s:%d/' % (args.bind, args.port), flush=True)

    srv = ThreadingHTTPServer((args.bind, args.port), Handler)
    srv.daemon_threads = True
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
