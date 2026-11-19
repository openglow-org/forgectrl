#!/usr/bin/env python3
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# SPDX-License-Identifier: MIT
"""Parse every script of the bundled pages the daemon serves.

embed.cmake bundles each page under src/ui/ into one self-contained page
in build/ui/: the page's own inline scripts and every script it names,
inlined. A syntax error anywhere in one of them stops that script, and a
page whose script does not run comes up empty while every C test passes.
This runs `node --check` on each inline script block of each bundled
page and fails on any error. It also reads what a page must carry
(MUST_CARRY): index.html holds the extension card and its script, and
defines loadExt before the call panel.js runs while the page parses, so
a later ext.js is a broken tab.

It holds one bound between the daemon and a page (linger_failure): a
package check's reservation of the machine's button outlasts its prompt
by WIZPKG_LINGER_S (src/wizpkg.h), which must exceed what the Setup page
can go on showing the question (src/ui/wizard.js: DARK_FRESH_MS after the
request of the last poll that showed it, checked on every frame, with
50 ms for the frame) by a second at least.

With --embed it also runs src/ui/embed.cmake, as the build runs it, on
the login page with the dev-image mark taken out, and fails unless the
script refuses it, saying the page lacks the mark: main.c puts a dev
image's bar in place of the mark, so a page bundled without it would
hide a dev image.

Usage: check_ui_js.py [--embed] [BUNDLE_DIR]   (default build/ui)
The node binary is NODE from the environment, else `node` on the PATH;
cmake (for --embed) is CMAKE, else `cmake` on the PATH. No node, no
pages, no scripts, or no cmake with --embed is a failure, never a pass.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

SCRIPT = re.compile(r"<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>", re.S | re.I)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEV_MARK = "<!--FFDEV-->"
# The login page's sources, as embed.cmake inlines them
LOGIN_SOURCES = ("login.html", "login.js", "theme.css", "wizard.css", os.path.join("vendor", "bootstrap.min.css"))

# page: (the strings it carries, the (first, then) pairs whose first
# occurrences come in that order)
MUST_CARRY = {
    "index.html": (("extpkgs", "extswitch", "extfile", "extstaged", "extinstallphrase", "extAct", "loadExt"),
                   (("function loadExt", "loadExt();"),)),
}


def carry_failures(name, html):
    """What a bundled page lacks of MUST_CARRY, one line each."""
    strings, order = MUST_CARRY.get(name, ((), ()))
    out = ["%s carries no %s" % (name, s) for s in strings if s not in html]
    for first, then in order:
        a, b = html.find(first), html.find(then)
        if a < 0 or b < 0 or a > b:
            out.append("%s has no %s before %s" % (name, first, then))
    return out


LINGER = (os.path.join("src", "wizpkg.h"), re.compile(r"#define\s+WIZPKG_LINGER_S\s+([0-9]+(?:\.[0-9]+)?)\b"))
DARK_FRESH = (os.path.join("src", "ui", "wizard.js"), re.compile(r"var\s+DARK_FRESH_MS\s*=\s*([0-9]+)\s*;"))
DARK_FRAME_SLACK_MS = 50
LINGER_MARGIN_MS = 1000


def linger_failure():
    """Why WIZPKG_LINGER_S does not outlast the Setup page's question by
    LINGER_MARGIN_MS, or None when it does."""
    got = []
    for rel, pat in (LINGER, DARK_FRESH):
        with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
            m = pat.findall(f.read())
        if len(m) != 1:
            return "%s: %d definitions of %s, not one" % (rel, len(m), pat.pattern)
        got.append(float(m[0]))
    linger_ms, fresh_ms = got[0] * 1000.0, got[1]
    if linger_ms < fresh_ms + DARK_FRAME_SLACK_MS + LINGER_MARGIN_MS:
        return ("WIZPKG_LINGER_S %.0f ms does not outlast the Setup page's question (DARK_FRESH_MS %.0f ms + %d ms) "
                "by %d ms" % (linger_ms, fresh_ms, DARK_FRAME_SLACK_MS, LINGER_MARGIN_MS))
    return None


def embed_failure(tmp):
    """Why embed.cmake did not refuse the login page without the dev-image
    mark, or None when it refused it for that."""
    cmake = os.environ.get("CMAKE") or shutil.which("cmake")
    if not cmake:
        return "--embed: no cmake: set CMAKE or put cmake on the PATH"
    src = os.path.join(ROOT, "src", "ui")
    ui = os.path.join(tmp, "unmarked")
    for f in LOGIN_SOURCES:
        os.makedirs(os.path.dirname(os.path.join(ui, f)), exist_ok=True)
        shutil.copyfile(os.path.join(src, f), os.path.join(ui, f))
    page = os.path.join(ui, "login.html")
    with open(page, encoding="utf-8") as f:
        html = f.read()
    if DEV_MARK not in html:
        return "src/ui/login.html carries no %s to take out" % DEV_MARK
    with open(page, "w", encoding="utf-8") as f:
        f.write(html.replace(DEV_MARK, ""))
    out = os.path.join(tmp, "unmarked-login.html")
    r = subprocess.run([cmake, "-DUI_DIR=" + ui, "-DPAGE=login", "-DOUT_HTML=" + out, "-DOUT_C=" + out + ".c",
                        "-P", os.path.join(src, "embed.cmake")], capture_output=True, text=True)
    if r.returncode == 0 or os.path.exists(out):
        return "embed.cmake bundled a login page that lacks the dev-image mark %s" % DEV_MARK
    if "lacks the dev-image mark" not in r.stdout + r.stderr:
        return "embed.cmake refused the unmarked login page, but not for the mark: %s" % (r.stderr or r.stdout).strip()
    return None


def main():
    args = sys.argv[1:]
    embed = "--embed" in args
    args = [a for a in args if a != "--embed"]
    bundle = args[0] if args else os.path.join("build", "ui")
    node = os.environ.get("NODE") or shutil.which("node")
    if not node:
        print("check_ui_js: no node: set NODE or put node on the PATH")
        return 2
    pages = sorted(glob.glob(os.path.join(bundle, "*.html")))
    if not pages:
        print("check_ui_js: no bundled pages under %s" % bundle)
        return 2
    failures = 0
    lacking = 0
    checked = 0
    with tempfile.TemporaryDirectory(prefix="check-ui-js-") as tmp:
        for page in pages:
            with open(page, encoding="utf-8", errors="replace") as f:
                html = f.read()
            blocks = SCRIPT.findall(html)
            for i, body in enumerate(blocks):
                if not body.strip():
                    continue
                path = os.path.join(tmp, "%s-%d.js" % (os.path.basename(page), i))
                with open(path, "w", encoding="utf-8") as f:
                    f.write(body)
                r = subprocess.run([node, "--check", path], capture_output=True, text=True)
                checked += 1
                if r.returncode != 0:
                    failures += 1
                    print("FAIL %s script %d:\n%s" % (os.path.basename(page), i,
                                                      (r.stderr or r.stdout).strip()))
            print("%-24s %d script(s)" % (os.path.basename(page), len(blocks)))
            for why in carry_failures(os.path.basename(page), html):
                lacking += 1
                print("FAIL %s" % why)
        if embed:
            why = embed_failure(tmp)
            if why:
                lacking += 1
                print("FAIL %s" % why)
            else:
                print("embed.cmake refuses a page that lacks the dev-image mark")
    for name in sorted(set(MUST_CARRY) - {os.path.basename(p) for p in pages}):
        lacking += 1
        print("FAIL %s is not in the bundle" % name)
    why = linger_failure()
    if why:
        lacking += 1
        print("FAIL %s" % why)
    else:
        print("WIZPKG_LINGER_S outlasts the Setup page's question")
    if not checked:
        print("check_ui_js: no scripts in the bundled pages")
        return 2
    print("%s: %d script(s) checked, %d failed; %d page check(s) failed"
          % ("FAIL" if failures or lacking else "PASS", checked, failures, lacking))
    return 1 if failures or lacking else 0


if __name__ == "__main__":
    sys.exit(main())
