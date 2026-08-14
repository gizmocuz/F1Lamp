#!/usr/bin/env python3
"""
F1 live timing simulator for the F1Lamp firmware.

Stands in for livetiming.formula1.com so the safety-car, VSC and red-flag code
paths can be exercised without waiting for a race weekend. It speaks the same
protocol the real service does:

    GET  /static/StreamingStatus.json      -> {"Status":"Available"|"Offline"}
    POST /signalrcore/negotiate            -> connectionToken + AWSALB cookies
    GET  /signalrcore?id=<token>           -> Server-Sent Events stream
    POST /signalrcore?id=<token>           -> handshake / Subscribe

...including the two details that trip up real implementations: the AWS load
balancer sticky cookie, and 0x1E record separators.

It also checks that the client does the right thing, and draws the running
session in the console.

Usage
-----
    python f1_sim.py                       # race scenario on port 8080
    python f1_sim.py --scenario stale      # stop sending, verify staleness
    python f1_sim.py --speed 4             # 4x faster
    python f1_sim.py --list                # show scenarios

Then on the lamp's /config page set:
    F1 server        <this machine's IP> : 8080
    F1 server TLS    unchecked
    Track live F1    checked

No third-party packages required.
"""

import argparse
import json
import os
import random
import socket
import string
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RS = "\x1e"  # SignalR Core record separator

# --------------------------------------------------------------------------- #
# Console colours
# --------------------------------------------------------------------------- #

if os.name == "nt":
    os.system("")  # enable ANSI escape processing on Windows terminals


class C:
    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    RED = "\033[91m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    BLUE = "\033[94m"
    MAGENTA = "\033[95m"
    CYAN = "\033[96m"
    GREY = "\033[90m"
    WHITE = "\033[97m"
    ON_RED = "\033[41m"
    ON_GREEN = "\033[42m"
    ON_YELLOW = "\033[43m"


# --------------------------------------------------------------------------- #
# Track status vocabulary (matches the real feed)
# --------------------------------------------------------------------------- #

TRACK = {
    "1": ("AllClear",    "GREEN",       C.GREEN,   "G", "green, solid"),
    "2": ("Yellow",      "YELLOW",      C.YELLOW,  "Y", "amber, solid"),
    "4": ("SCDeployed",  "SAFETY CAR",  C.YELLOW,  "S", "amber, BLINK (F <-> 1)"),
    "5": ("Red",         "RED FLAG",    C.RED,     "R", "red, solid"),
    "6": ("VSCDeployed", "VSC",         C.YELLOW,  "V", "amber, breathe slow"),
    "7": ("VSCEnding",   "VSC ENDING",  C.YELLOW,  "v", "amber, breathe fast"),
}

# --------------------------------------------------------------------------- #
# Scenarios: (at_second, kind, value)
#   kind: "stream" Available/Offline | "session" <status> | "track" <code>
#         "note" free text | "kill" drop SSE connections | "mute" stop sending
# --------------------------------------------------------------------------- #

SCENARIOS = {
    "race": {
        "desc": "Full grand prix: green, yellows, safety car, VSC, red flag, finish",
        "script": [
            (0,   "stream",  "Available"),
            (0,   "session", "Inactive"),
            (0,   "track",   "1"),
            (6,   "note",    "Formation lap"),
            (8,   "session", "Started"),
            (10,  "note",    "Lights out!"),
            (20,  "track",   "2"),
            (24,  "note",    "Debris at turn 4"),
            (32,  "track",   "1"),
            (45,  "track",   "4"),
            (46,  "note",    "Safety car deployed"),
            (62,  "track",   "1"),
            (75,  "track",   "6"),
            (76,  "note",    "VSC for marshals on track"),
            (86,  "track",   "7"),
            (90,  "track",   "1"),
            (105, "track",   "5"),
            (106, "note",    "RED FLAG - session suspended"),
            (108, "session", "Aborted"),
            (125, "session", "Started"),
            (126, "track",   "1"),
            (140, "note",    "Chequered flag"),
            (142, "session", "Finished"),
            (148, "session", "Finalised"),
            (150, "stream",  "Offline"),
            (165, "note",    "Idle - lamp should be OFF. Ctrl-C to stop."),
        ],
    },
    "quali": {
        "desc": "Short qualifying session with one red flag",
        "script": [
            (0,  "stream",  "Available"),
            (0,  "session", "Inactive"),
            (0,  "track",   "1"),
            (5,  "session", "Started"),
            (18, "track",   "2"),
            (26, "track",   "1"),
            (40, "track",   "5"),
            (41, "note",    "Crash at turn 7 - red flag"),
            (58, "track",   "1"),
            (70, "session", "Finished"),
            (76, "session", "Finalised"),
            (78, "stream",  "Offline"),
        ],
    },
    "stale": {
        "desc": "Goes silent mid-race: the lamp must NOT keep showing safety car",
        "script": [
            (0,  "stream",  "Available"),
            (0,  "session", "Started"),
            (0,  "track",   "1"),
            (12, "track",   "4"),
            (13, "note",    "Safety car out..."),
            (20, "mute",    ""),
            (20, "note",    "SERVER GOES SILENT - no data, no pings."),
            (21, "note",    "Lamp should go stale within 45 s and drop the link."),
            (90, "note",    "Expect a reconnect attempt by now."),
        ],
    },
    "flaky": {
        "desc": "Repeatedly drops the connection to exercise reconnect + backoff",
        "script": [
            (0,  "stream",  "Available"),
            (0,  "session", "Started"),
            (0,  "track",   "1"),
            (15, "kill",    ""),
            (15, "note",    "Connection dropped"),
            (35, "track",   "2"),
            (45, "kill",    ""),
            (45, "note",    "Dropped again"),
            (70, "track",   "4"),
            (85, "kill",    ""),
            (110, "track",  "1"),
        ],
    },
    "offline": {
        "desc": "Nothing live at all - lamp must sit dark and just poll",
        "script": [
            (0, "stream", "Offline"),
            (0, "note",   "No session. Lamp should be OFF and poll every 2 min."),
        ],
    },
}


# --------------------------------------------------------------------------- #
# Simulator state
# --------------------------------------------------------------------------- #

class Sim:
    def __init__(self, scenario, speed):
        self.lock = threading.RLock()
        self.scenario = scenario
        self.speed = speed
        self.t0 = time.time()
        self.streaming = "Offline"
        self.session = "Inactive"
        self.track = "1"
        self.muted = False
        self.subscribers = []          # list of queues (one per SSE client)
        self.timeline = []             # (sim_second, char, colour)
        self.events = []               # rolling log
        self.tx_bytes = 0
        self.pings = 0
        self.tokens = {}               # token -> {"cookie": str, "handshaken": bool}
        self.client_ip = None
        self.checks = {
            "streaming_polled":  [None, "StreamingStatus.json polled"],
            "negotiate":         [None, "negotiate called"],
            "cookie_replayed":   [None, "ALB sticky cookie replayed on SSE GET"],
            "sse_accept":        [None, "SSE GET sends Accept: text/event-stream"],
            "handshake":         [None, "handshake {protocol:json,version:1} + 0x1E"],
            "subscribe":         [None, "Subscribe topics = TrackStatus, SessionStatus"],
            "no_racecontrol":    [None, "RaceControlMessages NOT subscribed (bandwidth)"],
            "reconnect":         [None, "reconnected after a drop"],
        }
        self.connect_count = 0
        self.requests = 0

    # -- time ------------------------------------------------------------- #
    def now(self):
        return (time.time() - self.t0) * self.speed

    # -- logging ---------------------------------------------------------- #
    def log(self, msg, colour=C.WHITE):
        with self.lock:
            self.events.append((self.now(), msg, colour))
            del self.events[:-14]

    def check(self, key, ok, detail=""):
        with self.lock:
            if self.checks[key][0] is not True or ok is False:
                self.checks[key][0] = ok
            if detail:
                self.checks[key][1] = detail

    # -- SSE -------------------------------------------------------------- #
    def broadcast(self, payload):
        """Send one SignalR record to every connected SSE client."""
        with self.lock:
            if self.muted:
                return
            frame = "data: " + json.dumps(payload, separators=(",", ":")) + RS + "\n\n"
            data = frame.encode()
            # only count what a client will really receive
            self.tx_bytes += len(data) * len(self.subscribers)
            for q in list(self.subscribers):
                q.append(data)

    def feed(self, topic, obj):
        self.broadcast({"type": 1, "target": "feed",
                        "arguments": [topic, obj, time.strftime("%Y-%m-%dT%H:%M:%S")]})

    def snapshot(self):
        with self.lock:
            name, _, _, _, _ = TRACK[self.track]
            return {
                "TrackStatus": {"Status": self.track, "Message": name, "_kf": True},
                "SessionStatus": {"Status": self.session, "_kf": True},
            }

    # -- state changes ---------------------------------------------------- #
    def set_track(self, code):
        with self.lock:
            self.track = code
        name, label, colour, _, lamp = TRACK[code]
        self.log("TrackStatus -> %s (%s)" % (code, name), colour)
        self.feed("TrackStatus", {"Status": code, "Message": name})

    def set_session(self, status):
        with self.lock:
            self.session = status
        self.log("SessionStatus -> %s" % status, C.CYAN)
        self.feed("SessionStatus", {"Status": status})

    def set_stream(self, status):
        with self.lock:
            self.streaming = status
        self.log("StreamingStatus -> %s" % status, C.MAGENTA)

    def kill_streams(self):
        with self.lock:
            for q in list(self.subscribers):
                q.append(None)          # sentinel: close
            self.subscribers = []


# --------------------------------------------------------------------------- #
# HTTP handler
# --------------------------------------------------------------------------- #

def make_handler(sim):

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass                          # we do our own console output

        # ---------------------------------------------------------------- #
        def _path(self):
            return self.path.split("?", 1)[0]

        def _query(self):
            if "?" not in self.path:
                return {}
            q = {}
            for part in self.path.split("?", 1)[1].split("&"):
                if "=" in part:
                    k, v = part.split("=", 1)
                    q[k] = v
            return q

        def _send(self, code, body=b"", ctype="application/json"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if body:
                self.wfile.write(body)

        # ---------------------------------------------------------------- #
        def do_GET(self):
            sim.client_ip = self.client_address[0]
            with sim.lock:
                sim.requests += 1
            p = self._path()

            if p == "/static/StreamingStatus.json":
                sim.check("streaming_polled", True)
                with sim.lock:
                    status = sim.streaming
                # real service prefixes a UTF-8 BOM - keep the client honest
                body = b"\xef\xbb\xbf" + json.dumps({"Status": status}).encode()
                sim.log("GET  StreamingStatus -> %s" % status, C.GREY)
                self._send(200, body)
                return

            if p == "/signalrcore":
                self._sse()
                return

            sim.log("GET  %s -> 404 (unexpected path)" % p, C.RED)
            self._send(404, b'{"error":"not found"}')

        # ---------------------------------------------------------------- #
        def do_POST(self):
            sim.client_ip = self.client_address[0]
            with sim.lock:
                sim.requests += 1
            p = self._path()

            if p == "/signalrcore/negotiate":
                token = "".join(random.choice(string.ascii_letters + string.digits)
                                for _ in range(22))
                cookie = "AWSALB=" + "".join(
                    random.choice(string.ascii_letters + string.digits) for _ in range(48))
                cookie_cors = "AWSALBCORS=" + cookie.split("=", 1)[1]
                with sim.lock:
                    sim.tokens[token] = {"cookie": cookie, "handshaken": False}
                sim.check("negotiate", True)
                sim.log("POST negotiate -> token %s..." % token[:8], C.GREY)

                body = json.dumps({
                    "negotiateVersion": 1,
                    "connectionId": token,
                    "connectionToken": token,
                    "availableTransports": [
                        {"transport": "WebSockets", "transferFormats": ["Text", "Binary"]},
                        {"transport": "ServerSentEvents", "transferFormats": ["Text"]},
                        {"transport": "LongPolling", "transferFormats": ["Text", "Binary"]},
                    ],
                }).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Set-Cookie", cookie + "; Path=/; HttpOnly")
                self.send_header("Set-Cookie", cookie_cors + "; Path=/; Secure; SameSite=None")
                self.end_headers()
                self.wfile.write(body)
                return

            if p == "/signalrcore":
                self._signalr_send()
                return

            sim.log("POST %s -> 404 (unexpected path)" % p, C.RED)
            self._send(404, b'{"error":"not found"}')

        # ---------------------------------------------------------------- #
        def _token_ok(self):
            """Mimic the AWS ALB: without the sticky cookie the request lands on
            the wrong backend and the connection is unknown."""
            tok = self._query().get("id", "")
            tok = tok.replace("%2F", "/").replace("%2B", "+").replace("%3D", "=")
            with sim.lock:
                entry = sim.tokens.get(tok)
            if entry is None:
                return None, None
            sent = self.headers.get("Cookie", "")
            if entry["cookie"].split("=", 1)[0] not in sent:
                return None, entry
            return tok, entry

        # ---------------------------------------------------------------- #
        def _signalr_send(self):
            length = int(self.headers.get("Content-Length", 0) or 0)
            raw = self.rfile.read(length).decode("utf-8", "replace") if length else ""

            tok, entry = self._token_ok()
            if tok is None:
                sim.check("cookie_replayed", False,
                          "ALB cookie MISSING on POST -> would 404 against the real service")
                sim.log("POST /signalrcore  NO/BAD COOKIE -> 404", C.RED)
                self._send(404, b"No Connection with that ID", "text/plain")
                return

            for rec in raw.split(RS):
                rec = rec.strip()
                if not rec:
                    continue
                try:
                    msg = json.loads(rec)
                except ValueError:
                    sim.log("POST unparseable record: %r" % rec[:40], C.RED)
                    continue

                if msg.get("protocol") == "json":
                    entry["handshaken"] = True
                    sim.check("handshake", True)
                    sim.log("POST handshake OK", C.GREY)

                elif msg.get("target") == "Subscribe":
                    topics = []
                    args = msg.get("arguments") or []
                    if args and isinstance(args[0], list):
                        topics = args[0]
                    sim.log("POST Subscribe %s" % ", ".join(topics), C.GREY)
                    want = {"TrackStatus", "SessionStatus"}
                    sim.check("subscribe", want.issubset(set(topics)),
                              "Subscribe topics = %s" % (", ".join(topics) or "(none)"))
                    sim.check("no_racecontrol", "RaceControlMessages" not in topics)
                    # completion + initial snapshot, exactly like the real service
                    sim.broadcast({"type": 3, "invocationId": msg.get("invocationId", "0"),
                                   "result": sim.snapshot()})

            self._send(200, b"")

        # ---------------------------------------------------------------- #
        def _sse(self):
            tok, entry = self._token_ok()
            if tok is None:
                sim.check("cookie_replayed", False,
                          "ALB cookie MISSING on SSE GET -> real service would reject")
                sim.log("SSE GET  NO/BAD COOKIE -> rejected", C.RED)
                self._send(200, b"No Connection with that ID", "text/plain")
                return

            sim.check("cookie_replayed", True)
            accept = self.headers.get("Accept", "")
            sim.check("sse_accept", "text/event-stream" in accept)

            with sim.lock:
                sim.connect_count += 1
                if sim.connect_count > 1:
                    sim.check("reconnect", True)
                q = []
                sim.subscribers.append(q)

            sim.log("SSE  stream opened (#%d)" % sim.connect_count, C.GREEN)

            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()

            try:
                self.wfile.write(b":\n\ndata: {}" + RS.encode() + b"\n\n")
                self.wfile.flush()
                last_ping = time.time()
                while True:
                    while q:
                        item = q.pop(0)
                        if item is None:
                            raise ConnectionError("closed by scenario")
                        self.wfile.write(item)
                        self.wfile.flush()
                    # keep-alive ping every 15 real seconds, like the real feed
                    if time.time() - last_ping >= 15:
                        last_ping = time.time()
                        with sim.lock:
                            muted = sim.muted
                        if not muted:
                            self.wfile.write(b"data: {\"type\":6}" + RS.encode() + b"\n\n")
                            self.wfile.flush()
                            with sim.lock:
                                sim.pings += 1
                    time.sleep(0.05)
            except Exception:
                pass
            finally:
                with sim.lock:
                    if q in sim.subscribers:
                        sim.subscribers.remove(q)
                sim.log("SSE  stream closed", C.YELLOW)

    return Handler


# --------------------------------------------------------------------------- #
# Console rendering
# --------------------------------------------------------------------------- #

def render(sim, port, width=78):
    with sim.lock:
        t = sim.now()
        streaming, session, track = sim.streaming, sim.session, sim.track
        timeline = list(sim.timeline)
        events = list(sim.events)
        checks = {k: list(v) for k, v in sim.checks.items()}
        nsub, tx, pings, muted = len(sim.subscribers), sim.tx_bytes, sim.pings, sim.muted
        cip = sim.client_ip
        nreq = sim.requests

    name, label, colour, ch, lamp = TRACK[track]
    live = (streaming == "Available" and session == "Started")

    out = []
    bar = "=" * width
    out.append(C.BOLD + C.WHITE + bar + C.RESET)
    out.append(C.BOLD + "  F1 LIVE TIMING SIMULATOR".ljust(46) +
               ("t=%6.1fs  %s  %.1fx" % (t, sim.scenario, sim.speed)).rjust(width - 46) + C.RESET)
    out.append(C.BOLD + C.WHITE + bar + C.RESET)
    out.append("")
    out.append("  listening  http://0.0.0.0:%d      client: %s" %
               (port, cip or C.DIM + "none yet" + C.RESET))
    out.append("")
    out.append("  Streaming  %-14s Session  %-12s Track  %s%s%s" % (
        (C.GREEN if streaming == "Available" else C.GREY) + streaming + C.RESET,
        (C.CYAN if session == "Started" else C.GREY) + session + C.RESET,
        colour + C.BOLD, label, C.RESET))
    out.append("")
    if muted:
        out.append("  " + C.RED + C.BOLD + "SERVER MUTED - lamp should mark the feed stale" + C.RESET)
    elif live:
        out.append("  Lamp should be:  " + colour + C.BOLD + "###" + C.RESET + "  " + lamp)
    else:
        out.append("  Lamp should be:  " + C.GREY + "--- OFF (no session live)" + C.RESET)
    out.append("")

    # timeline strip
    out.append("  " + C.DIM + "G green  Y yellow  S safety car  V vsc  v ending  R red  . no session" + C.RESET)
    strip = ""
    for _, c, col in timeline[-(width - 4):]:
        strip += col + c + C.RESET
    out.append("  " + strip)
    out.append("")
    out.append("  SSE clients %-4d  connects %-4d  requests %-5d  TX %-8s  pings %d" %
               (nsub, sim.connect_count, nreq, "%d B" % tx, pings))
    out.append("")

    if nreq == 0:
        out.append("  " + C.ON_RED + C.WHITE + C.BOLD +
                   " NO REQUESTS RECEIVED - the lamp has not reached this server " + C.RESET)
        out.append("")
        out.append("   1. Windows Firewall: allow Python on " + C.BOLD + "Private" + C.RESET +
                   " networks (most common cause)")
        out.append("   2. On the lamp /config:  F1 server = " + C.BOLD + "%s : %d" % (local_ip(), port) + C.RESET)
        out.append("   3. " + C.BOLD + "Untick" + C.RESET + " 'F1 server uses TLS' - this server is plain HTTP")
        out.append("   4. Tick 'Track live F1', then press Save")
        out.append("   5. From a phone on the same WiFi, open:")
        out.append("        " + C.CYAN + "http://%s:%d/static/StreamingStatus.json" % (local_ip(), port) + C.RESET)
        out.append("      If that fails, it is the network or firewall, not the lamp.")
        out.append("")
    elif nsub == 0:
        out.append("  " + C.YELLOW + "Requests seen, but no SSE stream open yet." + C.RESET +
                   " Streaming must be Available for the lamp to connect.")
        out.append("")

    out.append(C.BOLD + "  PROTOCOL CHECKS" + C.RESET)
    for key, (ok, desc) in checks.items():
        if ok is True:
            mark = C.GREEN + "[PASS]" + C.RESET
        elif ok is False:
            mark = C.RED + "[FAIL]" + C.RESET
        else:
            mark = C.GREY + "[ .. ]" + C.RESET
        out.append("   %s %s" % (mark, desc))
    out.append("")

    out.append(C.BOLD + "  EVENT LOG" + C.RESET)
    for et, msg, col in events[-12:]:
        out.append("   %s%7.1fs%s  %s%s%s" % (C.DIM, et, C.RESET, col, msg, C.RESET))

    out.append("")
    out.append(C.DIM + "  Ctrl-C to stop" + C.RESET)

    sys.stdout.write("\033[H\033[2J" + "\n".join(out) + "\n")
    sys.stdout.flush()


# --------------------------------------------------------------------------- #
# Scenario driver
# --------------------------------------------------------------------------- #

def run_scenario(sim, script):
    pending = sorted(script, key=lambda s: s[0])
    idx = 0
    last_tick = -1
    while True:
        t = sim.now()

        while idx < len(pending) and pending[idx][0] <= t:
            _, kind, value = pending[idx]
            idx += 1
            if kind == "stream":
                sim.set_stream(value)
            elif kind == "session":
                sim.set_session(value)
            elif kind == "track":
                sim.set_track(value)
            elif kind == "note":
                sim.log(value, C.BOLD + C.WHITE)
            elif kind == "kill":
                sim.kill_streams()
                sim.log("dropped all SSE connections", C.RED)
            elif kind == "mute":
                with sim.lock:
                    sim.muted = True

        # one timeline cell per simulated second
        sec = int(t)
        if sec != last_tick:
            last_tick = sec
            with sim.lock:
                if sim.streaming == "Available" and sim.session == "Started":
                    _, _, col, ch, _ = TRACK[sim.track]
                else:
                    col, ch = C.GREY, "."
                sim.timeline.append((sec, ch, col))
                del sim.timeline[:-400]

        time.sleep(0.05 / max(sim.speed, 0.1))


# --------------------------------------------------------------------------- #

def local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def main():
    ap = argparse.ArgumentParser(description="F1 live timing simulator for F1Lamp")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--scenario", default="race", choices=sorted(SCENARIOS))
    ap.add_argument("--speed", type=float, default=1.0, help="time multiplier")
    ap.add_argument("--loop", action="store_true", help="restart the scenario when it ends")
    ap.add_argument("--list", action="store_true", help="list scenarios and exit")
    args = ap.parse_args()

    if args.list:
        for k, v in sorted(SCENARIOS.items()):
            print("  %-9s %s" % (k, v["desc"]))
        return 0

    sim = Sim(args.scenario, args.speed)
    script = SCENARIOS[args.scenario]["script"]

    server = ThreadingHTTPServer(("0.0.0.0", args.port), make_handler(sim))
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()

    sim.log("simulator up on %s:%d" % (local_ip(), args.port), C.BOLD + C.GREEN)
    sim.log("scenario '%s': %s" % (args.scenario, SCENARIOS[args.scenario]["desc"]), C.WHITE)

    driver = threading.Thread(target=run_scenario, args=(sim, script), daemon=True)
    driver.start()

    end = max(s[0] for s in script) + 20
    try:
        while True:
            render(sim, args.port)
            if args.loop and sim.now() > end:
                sim.t0 = time.time()
                with sim.lock:
                    sim.muted = False
                    sim.timeline = []
                driver_new = threading.Thread(target=run_scenario, args=(sim, script), daemon=True)
                driver_new.start()
            time.sleep(0.25)
    except KeyboardInterrupt:
        pass

    # final report
    print("\n" + C.BOLD + "Final protocol checks" + C.RESET)
    failed = 0
    for key, (ok, desc) in sim.checks.items():
        if ok is True:
            print("  " + C.GREEN + "PASS" + C.RESET + "  " + desc)
        elif ok is False:
            failed += 1
            print("  " + C.RED + "FAIL" + C.RESET + "  " + desc)
        else:
            print("  " + C.GREY + "n/a " + C.RESET + "  " + desc)
    print("\n%d byte(s) streamed, %d ping(s), %d connection(s)"
          % (sim.tx_bytes, sim.pings, sim.connect_count))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
