#!/usr/bin/env python3
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# https://community.openglow.org
# SPDX-License-Identifier:    MIT

"""Read the sensors logger's records out of a log export.

    senslog_read.py BUNDLE [--csv DIR] [--summary]

BUNDLE is an export (.tar.gz), an unpacked export directory, or one or
more sensors log files (sensors.log, sensors.log.N.gz). The records are
src/senslogfmt.h's; the schema is on the docs site (technical/forgefirm/
logging.md, "The sensors logger"). Every record gets a wall-clock time
where its boot's offset is known: from the boot's clock record, else
from its latest stamped line (a stamp before the clock is set is 1970 or
a date saved at an earlier shutdown). --csv writes one
table per record type and one per flight-recorder dump; --summary (the
default) prints the inventory, the jobs, the flow verdicts and the dumps.
Standard library only.
"""

import argparse
import csv
import gzip
import io
import os
import re
import sys
import tarfile
from datetime import datetime, timezone

TYPES = ("beat", "job", "flow", "inv", "cfg", "clock", "fr", "frc")
# A log line: the rsyslog stamp, the program, the level, the record.
LOG_LINE = re.compile(r"^(\S+) (\S+?)(?:\[\d+\])? ([A-Z]+) (.*)$")
SET_YEAR = 2020         # a stamp before this was written before the clock was set


def parse_record(text):
    """One record's text -> dict, or None. "-" reads as None; msg runs to
    the end of the line."""
    head, _, rest = text.partition(" ")
    if head not in TYPES:
        return None
    rec = {"type": head}
    while rest:
        if rest.startswith("msg="):
            rec["msg"] = rest[4:]
            break
        tok, _, rest = rest.partition(" ")
        k, eq, v = tok.partition("=")
        if not eq:
            continue
        rec[k] = None if v == "-" else v
    return rec


def parse_line(line):
    """A log line (or a bare record) -> (stamp or None, record or None)."""
    line = line.rstrip("\r\n")
    m = LOG_LINE.match(line)
    if m and m.group(2) == "sensors":
        return m.group(1), parse_record(m.group(4))
    return None, parse_record(line)


def num(v):
    """A record value as a number (None when unread or not a number)."""
    if v is None:
        return None
    try:
        return int(v, 0) if v.startswith("0x") else float(v)
    except ValueError:
        return None


def stat(v):
    """A job's min/mean/max value -> (min, mean, max), or None."""
    if v is None:
        return None
    parts = v.split("/")
    return tuple(num(p) for p in parts) if len(parts) == 3 else None


def _stamp_epoch(stamp):
    try:
        return datetime.fromisoformat(stamp).timestamp()
    except (TypeError, ValueError):
        return None


def _iso(t):
    return datetime.fromtimestamp(t, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# ------------------------------------------------------------- sources

def _rotation_order(name):
    """sensors.log.3.gz, .2.gz, .1.gz, then sensors.log: oldest first."""
    m = re.search(r"\.log\.(\d+)(?:\.gz)?$", name)
    return (0, -int(m.group(1))) if m else (1, 0)


def _lines_of(name, data):
    if name.endswith(".gz"):
        data = gzip.decompress(data)
    return io.StringIO(data.decode("utf-8", "replace"))


def read_sources(paths):
    """Every line of the sensors logs among paths, oldest file first."""
    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, names in os.walk(p):
                for n in names:
                    if re.match(r"sensors\.log(\.\d+(\.gz)?)?$", n):
                        with open(os.path.join(root, n), "rb") as f:
                            files.append((n, f.read()))
        elif tarfile.is_tarfile(p):
            with tarfile.open(p, "r:*") as tar:
                for m in tar.getmembers():
                    n = os.path.basename(m.name)
                    if m.isfile() and re.match(r"sensors\.log(\.\d+(\.gz)?)?$", n):
                        files.append((n, tar.extractfile(m).read()))
        else:
            with open(p, "rb") as f:
                files.append((os.path.basename(p), f.read()))
    files.sort(key=lambda nf: _rotation_order(nf[0]))
    for n, data in files:
        yield from _lines_of(n, data)


# ------------------------------------------------------------- records

def load(lines):
    """Records in order, each with its boot's wall clock where known."""
    recs, offsets, clocked = [], {}, set()
    for line in lines:
        stamp, rec = parse_line(line)
        if not rec:
            continue
        mono = num(rec.get("mono"))
        boot = rec.get("boot")
        rec["_stamp"] = stamp
        recs.append(rec)
        if mono is None or boot is None:
            continue
        if rec["type"] == "clock":
            to = _stamp_epoch(rec.get("to", "").replace("Z", "+00:00"))
            if to is not None:
                offsets[boot] = to - mono
                clocked.add(boot)
        elif boot not in clocked:
            # Before the clock is set a stamp is 1970 or a date saved at an
            # earlier shutdown; the boot's latest stamp is the set clock's.
            t = _stamp_epoch(stamp)
            if t is not None and datetime.fromtimestamp(t, timezone.utc).year >= SET_YEAR:
                offsets[boot] = t - mono
    for rec in recs:
        mono, boot = num(rec.get("mono")), rec.get("boot")
        if mono is not None and boot in offsets:
            rec["wall"] = _iso(offsets[boot] + mono)
    return recs


def dumps(recs):
    """The flight-recorder dumps: {(boot, ev): {"head": rec, "channels": {name: [values]}, "t0": .., "t1": ..}}."""
    out = {}
    for r in recs:
        if r["type"] not in ("fr", "frc"):
            continue
        key = (r.get("boot"), r.get("ev"))
        d = out.setdefault(key, {"head": None, "channels": {}})
        if r["type"] == "fr":
            d["head"] = r
        else:
            vals = (r.get("vals") or "").split(",") if r.get("vals") else []
            d["channels"][r.get("ch")] = [None if v == "-" else v for v in vals]
            d["t0"], d["t1"] = num(r.get("t0")), num(r.get("t1"))
    return out


# ------------------------------------------------------------- outputs

def write_csv(recs, outdir):
    os.makedirs(outdir, exist_ok=True)
    written = []
    for t in ("beat", "job", "flow", "inv", "cfg", "clock"):
        rows = [r for r in recs if r["type"] == t]
        if not rows:
            continue
        keys = ["wall"]
        for r in rows:
            for k in r:
                if k not in keys and not k.startswith("_") and k != "type":
                    keys.append(k)
        path = os.path.join(outdir, t + ".csv")
        with open(path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
            w.writeheader()
            for r in rows:
                w.writerow({k: ("" if r.get(k) is None else r.get(k)) for k in keys})
        written.append(path)
    for (boot, ev), d in dumps(recs).items():
        chans = d["channels"]
        if not chans:
            continue
        n = max(len(v) for v in chans.values())
        t0 = d.get("t0") or 0.0
        path = os.path.join(outdir, "fr_%s_%s.csv" % (boot, ev))
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["mono"] + list(chans))
            for i in range(n):
                w.writerow([round(t0 + i, 1)] + [("" if i >= len(v) or v[i] is None else v[i])
                                                  for v in chans.values()])
        written.append(path)
    return written


def summary(recs, out=sys.stdout):
    inv = [r for r in recs if r["type"] == "inv"]
    if inv:
        last = inv[-1]
        out.write("inventory (%s, %s):\n  %s\n" % (last.get("why"), last.get("wall", "undated"),
                  " ".join("%s=%s" % (k, v) for k, v in last.items()
                           if k not in ("type", "v", "boot", "mono", "why", "wall") and not k.startswith("_"))))
    jobs = [r for r in recs if r["type"] == "job"]
    out.write("jobs: %d\n" % len(jobs))
    for j in jobs:
        up = stat(j.get("up")) or (None, None, None)
        out.write("  %s run %ss cool %ss lit %ss coolant up %s->%s peak %s, exhaust %s rpm, beam rise %s%s\n" % (
            j.get("wall", "boot %s +%s" % (j.get("boot"), j.get("start"))), j.get("run_s"), j.get("cool_s"),
            j.get("lit_s"), j.get("up_start"), j.get("up_end"), up[2], j.get("exhaust_rpm"),
            j.get("beam_rise"), (" verdicts " + j["verdicts"]) if j.get("verdicts") else ""))
    flows = [r for r in recs if r["type"] == "flow"]
    out.write("flow verdicts: %d\n" % len(flows))
    for f in flows:
        out.write("  %s %s %s%s rise %s (limit %s, duty %s%%) from %s/%s rest_dt %s\n" % (
            f.get("wall", "-"), f.get("src"), f.get("verdict"),
            (" " + f["trial"]) if f.get("trial") else "", f.get("rise"), f.get("limit"),
            f.get("duty"), f.get("base_down"), f.get("base_up"), f.get("rest_dt")))
    ds = dumps(recs)
    out.write("flight-recorder dumps: %d\n" % len(ds))
    for (boot, ev), d in ds.items():
        h = d["head"] or {}
        out.write("  boot %s ev %s %s: %s\n" % (boot, ev, h.get("wall", "-"), h.get("msg", "")))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("bundle", nargs="+", help="an export (.tar.gz), its directory, or sensors log files")
    ap.add_argument("--csv", metavar="DIR", help="write one table per record type into DIR")
    ap.add_argument("--summary", action="store_true", help="print the summary (the default without --csv)")
    a = ap.parse_args(argv)
    recs = load(read_sources(a.bundle))
    if not recs:
        print("no sensors records found", file=sys.stderr)
        return 1
    if a.csv:
        for p in write_csv(recs, a.csv):
            print(p)
    if a.summary or not a.csv:
        summary(recs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
