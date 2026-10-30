# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# https://community.openglow.org
# SPDX-License-Identifier:    MIT

"""The bundle reader against the records the daemon writes.

tests/senslog_records.txt is the golden file senslog_test proves the
formatters write, so a format change reaches this test the moment the
golden file is rewritten. Proven here: every golden record parses with
its values; a boot's lines written before the clock was set are re-dated
from its clock record; the tables come out one per record type and one
per flight-recorder dump; an export's rotated files are read oldest
first."""
import gzip
import io
import os
import shutil
import sys
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import senslog_read as sr  # noqa: E402

GOLDEN = os.path.join(HERE, "senslog_records.txt")


def golden():
    with open(GOLDEN) as f:
        return f.read().splitlines()


class Records(unittest.TestCase):
    def test_every_golden_record_parses(self):
        recs = [sr.parse_record(l) for l in golden()]
        self.assertTrue(all(recs))
        by = {}
        for r in recs:
            by.setdefault(r["type"], []).append(r)
        beat = by["beat"][0]
        self.assertEqual(sr.num(beat["down"]), 21.43)
        self.assertEqual(sr.num(beat["sw"]), 0x28)
        self.assertEqual(beat["ir"], "50,49,52,54")
        self.assertIsNone(by["beat"][1]["down"])              # unread
        cal = [f for f in by["flow"] if f["src"] == "cal"][0]
        self.assertEqual((cal["trial"], cal["laser"]), ("2/3", None))
        self.assertTrue(by["fr"][0]["msg"].startswith("verdict SUSPECT: COOLANT FLOW SUSPECT: heater rise"))
        job = by["job"][0]
        self.assertEqual(sr.stat(job["up"])[2], 25.2)
        self.assertEqual(job["verdicts"], "SUSPECT")


class Redating(unittest.TestCase):
    def test_lines_before_the_clock_is_set_get_the_boots_wall_time(self):
        # Before the clock is set a line is stamped 1970 or with a date saved
        # at an earlier shutdown (boot bbbb0002's first line, which has no
        # clock record); the clock record, else the boot's latest stamp, dates
        # the boot.
        lines = [
            "1970-01-01T00:00:13.000000+00:00 sensors[400] INFO beat v=1 boot=aaaa0001 mono=13.0 ph=idle",
            "1970-01-01T00:00:41.000000+00:00 sensors[400] INFO clock v=1 boot=aaaa0001 mono=41.0 "
            "from=1970-01-01T00:00:41Z to=2026-10-30T18:30:00Z",
            "2026-10-30T18:31:00.000000+00:00 sensors[400] INFO beat v=1 boot=aaaa0001 mono=101.0 ph=idle",
            "2026-10-21T19:01:19.000000+00:00 sensors[400] INFO beat v=1 boot=bbbb0002 mono=15.0 ph=idle",
            "2026-10-30T21:00:15.000000+00:00 sensors[400] INFO beat v=1 boot=bbbb0002 mono=75.0 ph=idle",
            "1970-01-01T00:00:15.000000+00:00 sensors[400] INFO beat v=1 boot=cccc0003 mono=15.0 ph=idle",
            "1970-01-01T00:00:16.000000+00:00 forgectrl[401] INFO cool: engine started",
        ]
        recs = sr.load(lines)
        self.assertEqual([r.get("wall") for r in recs],
                         ["2026-10-30T18:29:32Z", "2026-10-30T18:30:00Z", "2026-10-30T18:31:00Z",
                          "2026-10-30T20:59:15Z", "2026-10-30T21:00:15Z", None])


class Tables(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="senslog.")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

    def test_one_table_per_type_and_per_dump(self):
        paths = sr.write_csv(sr.load(golden()), self.dir)
        names = sorted(os.path.basename(p) for p in paths)
        self.assertEqual(names, ["beat.csv", "cfg.csv", "clock.csv", "flow.csv", "fr_fcce7554_1.csv",
                                 "inv.csv", "job.csv"])
        with open(os.path.join(self.dir, "fr_fcce7554_1.csv")) as f:
            rows = f.read().splitlines()
        self.assertEqual(rows[0], "mono,down,vd")
        self.assertEqual(rows[1], "3000.0,22.00,OK")

    def test_an_export_is_read_oldest_file_first(self):
        old = "beat v=1 boot=aaaa0001 mono=1.0 ph=idle\n"
        new = "beat v=1 boot=aaaa0001 mono=2.0 ph=idle\n"
        tgz = os.path.join(self.dir, "export.tar.gz")
        with tarfile.open(tgz, "w:gz") as tar:
            for name, data in (("logs/sensors/sensors.log", new.encode()),
                               ("logs/sensors/sensors.log.1.gz", gzip.compress(old.encode())),
                               ("logs/forgectrl/forgectrl.log", b"not a record\n")):
                info = tarfile.TarInfo(name)
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        recs = sr.load(sr.read_sources([tgz]))
        self.assertEqual([r["mono"] for r in recs], ["1.0", "2.0"])


if __name__ == "__main__":
    unittest.main()
