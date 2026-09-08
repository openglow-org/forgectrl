#!/usr/bin/env python3
"""hershey_gen.py - the stroke font table for the commissioning sheet.

Reads the Hershey Sans 1-stroke (Simplex) font in SVG-font form and
writes src/font_hershey.c: the printable ASCII glyphs (32 to 126) as
polylines in the font's own units, with the advance of each glyph.
The renderer (sheet.c) scales by the cap height.

The source is the Inkscape distribution's svg_fonts/HersheySans1.svg
(the Hershey data as converted by Windell H. Oskay); pass its path.
Only "M x y" and "L x y" commands are expected; anything else stops the
generator, so the table never carries a curve it cannot draw.

Usage: hershey_gen.py <HersheySans1.svg> [src/font_hershey.c]

The Hershey Fonts were originally created by Dr. A. V. Hershey while
working at the U. S. National Bureau of Standards. The format of the
font data in the distribution this table was made from was originally
created by James Hurt, Cognition, Inc., 900 Technology Park Drive,
Billerica, MA 01821 (mit-eddie!ci-dandelion!hurt). The data may be used
by anyone for any purpose, commercial or otherwise, providing that this
acknowledgment is distributed with it.
"""
import html
import re
import sys

PEN_UP = -32768


def parse_path(d):
    """The path data as a list of polylines, each a list of (x, y)."""
    toks = d.replace(',', ' ').split()
    polys, cur = [], None
    i = 0
    cmd = None
    while i < len(toks):
        t = toks[i]
        if t in ('M', 'L'):
            cmd = t
            i += 1
            continue
        if not cmd:
            raise SystemExit('path without a command: %r' % d)
        if t[0].isalpha():
            raise SystemExit('unsupported path command %r in %r' % (t, d))
        x, y = float(toks[i]), float(toks[i + 1])
        i += 2
        if cmd == 'M':
            cur = [(x, y)]
            polys.append(cur)
            cmd = 'L'          # implicit lineto after a moveto
        else:
            cur.append((x, y))
    return [p for p in polys if len(p) >= 1]


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else 'src/font_hershey.c'
    text = open(src, encoding='utf-8').read()
    glyphs = {}
    for attrs in re.findall(r'<glyph\s+([^>]*?)/?>', text):
        m = re.search(r'unicode="([^"]*)"', attrs)
        if not m:
            continue
        u = html.unescape(m.group(1))
        if len(u) != 1 or not 32 <= ord(u) <= 126:
            continue
        adv = re.search(r'horiz-adv-x="([^"]*)"', attrs)
        d = re.search(r'\bd="([^"]*)"', attrs)
        glyphs[ord(u)] = (float(adv.group(1)) if adv else 0.0,
                          parse_path(d.group(1)) if d else [])
    missing = [c for c in range(32, 127) if c not in glyphs]
    if missing:
        raise SystemExit('glyphs missing: %r' % missing)

    pts = []            # (x, y) pairs, PEN_UP between polylines
    table = []          # (adv, first, count) per glyph
    for c in range(32, 127):
        adv, polys = glyphs[c]
        first = len(pts)
        for k, poly in enumerate(polys):
            if k:
                pts.append((PEN_UP, PEN_UP))
            for x, y in poly:
                xi, yi = int(round(x)), int(round(y))
                if abs(xi) > 4000 or abs(yi) > 4000:
                    raise SystemExit('coordinate out of range in %r' % chr(c))
                pts.append((xi, yi))
        table.append((int(round(adv)), first, len(pts) - first))

    lines = []
    w = lines.append
    w('/*')
    w(' * font_hershey.c - the stroke font of the commissioning sheet (generated)')
    w(' * Copyright 2026 514 LLC d/b/a OpenGlow')
    w(' * Written by Scott Wiederhold')
    w(' * SPDX-License-Identifier: MIT')
    w(' *')
    w(' * Hershey Sans 1-stroke (Simplex), the printable ASCII glyphs as')
    w(' * polylines in the font\'s own units (cap height %d), made by' % 662)
    w(' * tools/hershey_gen.py from the Hershey data in SVG-font form. Do not')
    w(' * edit; regenerate.')
    w(' *')
    w(' * The Hershey Fonts were originally created by Dr. A. V. Hershey')
    w(' * while working at the U. S. National Bureau of Standards. The format')
    w(' * of the font data in the distribution this table was made from was')
    w(' * originally created by James Hurt, Cognition, Inc., 900 Technology')
    w(' * Park Drive, Billerica, MA 01821 (mit-eddie!ci-dandelion!hurt). The')
    w(' * data may be used by anyone for any purpose, commercial or otherwise,')
    w(' * providing that this acknowledgment is distributed with it.')
    w(' */')
    w('#include "font_hershey.h"')
    w('')
    w('const short hershey_pts[][2] = {')
    row = []
    for x, y in pts:
        row.append('{%d,%d}' % (x, y))
        if len(row) == 8:
            w('    ' + ','.join(row) + ',')
            row = []
    if row:
        w('    ' + ','.join(row) + ',')
    w('};')
    w('')
    w('const hershey_glyph_t hershey_glyphs[HERSHEY_NGLYPH] = {')
    for k, (adv, first, count) in enumerate(table):
        c = 32 + k
        name = chr(c) if c not in (ord('\\'), ord('*'), ord('/')) else '#%d' % c
        w('    { %4d, %5d, %3d },  /* %s */' % (adv, first, count, name))
    w('};')
    w('')
    w('const unsigned hershey_npts = %d;' % len(pts))
    w('')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    print('%s: %d glyphs, %d points' % (out, len(table), len(pts)))


if __name__ == '__main__':
    main()
