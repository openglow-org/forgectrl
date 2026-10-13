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
page and fails on any error.

Usage: check_ui_js.py [BUNDLE_DIR]   (default build/ui)
The node binary is NODE from the environment, else `node` on the PATH.
No node, no pages, or no scripts is a failure, never a pass.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

SCRIPT = re.compile(r"<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>", re.S | re.I)


def main():
    bundle = sys.argv[1] if len(sys.argv) > 1 else os.path.join("build", "ui")
    node = os.environ.get("NODE") or shutil.which("node")
    if not node:
        print("check_ui_js: no node: set NODE or put node on the PATH")
        return 2
    pages = sorted(glob.glob(os.path.join(bundle, "*.html")))
    if not pages:
        print("check_ui_js: no bundled pages under %s" % bundle)
        return 2
    failures = 0
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
    if not checked:
        print("check_ui_js: no scripts in the bundled pages")
        return 2
    print("%s: %d script(s) checked, %d failed" % ("FAIL" if failures else "PASS", checked, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
