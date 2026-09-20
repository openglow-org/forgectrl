#!/usr/bin/env python3
# Copyright 2026 514 LLC d/b/a OpenGlow
# Written by Scott Wiederhold
# https://community.openglow.org
# SPDX-License-Identifier:    MIT
"""Tell somebody when the machine wants them: a job waits for the button,
pauses, ends, or the controller raises an alarm.

Follows the machine's event stream with a scoped token that holds `events`
and nothing else (https://docs.forgefirm.org/technical/forgefirm/remote-api/),
and for each event worth a person's attention either POSTs one line of text
to a URL (an ntfy topic, a chat webhook that takes plain text) or runs a
command with the line as its last argument. With neither, it prints the
lines. Python 3, standard library only.

    python3 notify.py --host forgefirm-1a2b --token-file token \\
        --fingerprint 41:15:34:... --webhook https://ntfy.example/mylaser

The machine's certificate is self-signed, so there is no authority to check
it against. It is pinned instead: --fingerprint is the certificate's SHA-256
as the panel's Setup card shows it (tls_fingerprint in GET /settings), and
it is compared before anything is sent, the token included.

The stream is one per address: a second copy of this script on the same
host takes the stream from the first, which then stops and says so. A lost
connection is retried, slowly.
"""
import argparse
import hashlib
import http.client
import json
import ssl
import subprocess
import sys
import time
import urllib.request

# Every event of the stream, in words.
WORDS = {
    "job.arming": lambda d: "the job is waiting for the button on the machine",
    "job.armed": lambda d: "the job is armed",
    "job.paused": lambda d: "the job is paused (%s)" % d.get("reason", "?"),
    "job.resumed": lambda d: "the job runs again",
    "job.ended": lambda d: ("the job ended" if d.get("result") == "ended"
                            else "the job ended in an alarm"),
    "alarm": lambda d: "the controller raised alarm %s" % d.get("code", "?"),
    "lid": lambda d: "the lid is %s" % ("closed" if d.get("closed") else "open"),
    "interlock": lambda d: "the interlock loop is %s" % ("closed" if d.get("ok") else "open"),
    "cooling.verdict": lambda d: "the cooling verdict is %s" % d.get("verdict", "?"),
    "mode.changed": lambda d: "the controller mode is now %s" % d.get("mode", "?"),
    "controller.started": lambda d: "the controller started",
    "controller.stopped": lambda d: "the controller stopped (%s)" % d.get("state", "?"),
    "homing.started": lambda d: "homing started",
    "homing.completed": lambda d: "homing completed",
    "homing.failed": lambda d: "homing failed",
    "motors.released": lambda d: "the X and Y motors are released",
    "motors.energized": lambda d: "the X and Y motors are energized",
    "lease.changed": lambda d: ("%s has the machine" % d["owner"] if d.get("owner")
                                else "the machine is free"),
}
# What is worth interrupting a person for.
DEFAULT = ["job.arming", "job.paused", "job.resumed", "job.ended", "alarm", "homing.failed",
           "controller.stopped"]


class Refused(Exception):
    """The machine answered with an error status, and said why."""

    def __init__(self, status, words):
        super().__init__("%s %s" % (status, words))
        self.status, self.words = status, words


class NotTheMachine(Exception):
    """The certificate is not the one that was pinned. Nothing was sent."""


def events(host, token, fingerprint, port=443, idle_timeout=20.0):
    """GET /events as (name, data) pairs, until the machine ends the stream.
    The machine sends a comment line every 5 s, so a stream that says
    nothing for idle_timeout is a dead one (TimeoutError). fingerprint is 64
    hex digits, or None to connect to whatever answers."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE         # the pin below is the check
    conn = http.client.HTTPSConnection(host, port, timeout=idle_timeout, context=ctx)
    try:
        conn.connect()
        if fingerprint is not None:
            seen = hashlib.sha256(conn.sock.getpeercert(binary_form=True)).hexdigest()
            if seen != fingerprint:
                raise NotTheMachine("the certificate at %s reads %s, not the pinned %s" % (host, seen, fingerprint))
        conn.request("GET", "/events", headers={"Authorization": "Bearer " + token})
        resp = conn.getresponse()
        if resp.status != 200:
            text = resp.read().decode("utf-8", "replace")
            try:
                text = json.loads(text).get("error", text)
            except (ValueError, AttributeError):
                pass
            raise Refused(resp.status, text)
        name, data = None, []
        while True:
            line = resp.readline()
            if not line:
                return
            line = line.decode("utf-8", "replace").rstrip("\r\n")
            if not line:
                if name:
                    try:
                        doc = json.loads("\n".join(data)) if data else {}
                    except ValueError:
                        doc = {}
                    yield name, doc if isinstance(doc, dict) else {}
                name, data = None, []
            elif line.startswith("event:"):
                name = line[6:].strip()
            elif line.startswith("data:"):
                data.append(line[5:].strip())
    finally:
        conn.close()


def tell(args, line):
    print(time.strftime("%H:%M:%S"), line, flush=True)
    try:
        if args.webhook:
            req = urllib.request.Request(args.webhook, data=line.encode(), method="POST",
                                         headers={"Content-Type": "text/plain; charset=utf-8"})
            urllib.request.urlopen(req, timeout=10).close()
        if args.command:
            subprocess.run(args.command + [line], timeout=30, check=False)
    except (OSError, subprocess.SubprocessError) as e:
        print("  could not deliver it: %s" % e, file=sys.stderr, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", required=True, help="the machine's address or name (forgefirm-1a2b)")
    ap.add_argument("--token-file", required=True,
                    help="a file that holds the scoped token (keep it out of the command line and its history)")
    ap.add_argument("--fingerprint", help="the certificate's SHA-256, as the panel's Setup card shows it")
    ap.add_argument("--no-verify", action="store_true",
                    help="connect without checking the certificate (the token goes to whatever answers)")
    ap.add_argument("--webhook", help="POST each line here as text/plain")
    ap.add_argument("--command", nargs=argparse.REMAINDER,
                    help="run this with the line as its last argument (put it last: all that follows is the command)")
    ap.add_argument("--name", default="ForgeFIRM", help="what the lines call the machine")
    ap.add_argument("--events", help="the events to tell of, comma-separated, from: %s (default: %s)"
                    % (" ".join(WORDS), ",".join(DEFAULT)))
    args = ap.parse_args()
    wanted = set(args.events.split(",")) if args.events else set(DEFAULT)
    if wanted - set(WORDS):
        ap.error("no such event: %s" % ", ".join(sorted(wanted - set(WORDS))))
    pin = "".join(c for c in (args.fingerprint or "").lower() if c in "0123456789abcdef")
    if not args.no_verify and len(pin) != 64:
        ap.error("--fingerprint is the certificate's SHA-256 as the panel's Setup card shows it "
                 "(64 hex digits); --no-verify connects without it")
    with open(args.token_file, "r", encoding="ascii") as f:
        token = f.read().strip()

    wait = 2
    while True:
        try:
            for name, data in events(args.host, token, None if args.no_verify else pin):
                wait = 2
                if name == "hello":
                    print(time.strftime("%H:%M:%S"), "following %s" % args.host, flush=True)
                elif name == "bye":
                    print("another client at this address took the event stream: stopping", file=sys.stderr)
                    return 1
                elif name in wanted:
                    tell(args, "%s: %s" % (args.name, WORDS[name](data)))
            # The machine ended the stream: its daemon restarted, or it is going down.
            print("the stream ended; again in %d s" % wait, file=sys.stderr, flush=True)
        except Refused as e:
            # 503 is the cap on streams: somebody may give one back. Anything
            # else (403: the token) will not get better by asking again.
            if e.status != 503:
                print("refused: %s" % e.words, file=sys.stderr)
                return 2
            print("the machine's event streams are all taken; asking again in a minute", file=sys.stderr)
            wait = 60
        except NotTheMachine as e:
            print(e, file=sys.stderr)
            return 3
        except OSError as e:                # a lost connection, a machine that is off
            print("the stream ended (%s); again in %d s" % (e, wait), file=sys.stderr, flush=True)
        time.sleep(wait)
        wait = min(wait * 2, 60)


if __name__ == "__main__":
    sys.exit(main())
