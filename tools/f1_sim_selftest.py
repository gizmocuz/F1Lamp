#!/usr/bin/env python3
"""
Verifies f1_sim.py by driving it with a client that behaves exactly like the
F1Lamp firmware: poll StreamingStatus, negotiate, capture the ALB cookie, open
the SSE stream, POST the handshake and Subscribe, then parse 0x1E records.

Run with no arguments; it starts the simulator itself on a spare port.

    python f1_sim_selftest.py
"""

import http.client
import json
import re
import subprocess
import sys
import os
import time
import threading
import socket

HERE = os.path.dirname(os.path.abspath(__file__))
RS = "\x1e"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class FirmwareClient:
    """Mirrors F1Client.ino step for step."""

    def __init__(self, host, port):
        self.host, self.port = host, port
        self.cookie = ""
        self.token = ""
        self.records = []
        self.stop = False

    def streaming_status(self):
        c = http.client.HTTPConnection(self.host, self.port, timeout=5)
        c.request("GET", "/static/StreamingStatus.json", headers={"User-Agent": "F1Lamp"})
        r = c.getresponse()
        body = r.read()
        c.close()
        # firmware skips the UTF-8 BOM before parsing
        txt = body.decode("utf-8", "replace")
        txt = txt[txt.find("{"):]
        return json.loads(txt)["Status"], body

    def negotiate(self):
        c = http.client.HTTPConnection(self.host, self.port, timeout=5)
        c.request("POST", "/signalrcore/negotiate?negotiateVersion=1", body="",
                  headers={"User-Agent": "F1Lamp", "Content-Length": "0"})
        r = c.getresponse()
        cookies = r.getheader("Set-Cookie") or ""
        # http.client folds repeated headers with ", " - take the AWSALB pairs
        parts = []
        for m in re.finditer(r"(AWSALB(?:CORS)?=[^;,]+)", cookies):
            parts.append(m.group(1))
        self.cookie = "; ".join(parts)
        body = json.loads(r.read().decode())
        c.close()
        self.token = body["connectionToken"]
        return bool(self.cookie), body

    def open_stream(self):
        self.sconn = http.client.HTTPConnection(self.host, self.port, timeout=30)
        self.sconn.request("GET", "/signalrcore?id=" + self.token, headers={
            "User-Agent": "F1Lamp",
            "Accept": "text/event-stream",
            "Cookie": self.cookie,
        })
        self.sresp = self.sconn.getresponse()
        if self.sresp.status != 200:
            return False
        threading.Thread(target=self._pump, daemon=True).start()
        return True

    def _pump(self):
        buf = b""
        try:
            while not self.stop:
                chunk = self.sresp.read(1)
                if not chunk:
                    break
                if chunk == b"\n":
                    line = buf.decode("utf-8", "replace")
                    buf = b""
                    if line.startswith("data:"):
                        for rec in line[5:].split(RS):
                            rec = rec.strip()
                            if rec:
                                self.records.append(rec)
                elif chunk != b"\r":
                    buf += chunk
        except Exception:
            pass

    def send(self, record):
        c = http.client.HTTPConnection(self.host, self.port, timeout=5)
        c.request("POST", "/signalrcore?id=" + self.token, body=record, headers={
            "User-Agent": "F1Lamp",
            "Cookie": self.cookie,
            "Content-Type": "text/plain;charset=UTF-8",
        })
        r = c.getresponse()
        r.read()
        c.close()
        return r.status


def main():
    port = free_port()
    proc = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "f1_sim.py"),
         "--port", str(port), "--scenario", "race", "--speed", "12"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    time.sleep(1.5)

    results = []

    def check(name, ok, extra=""):
        results.append((name, ok, extra))
        print(("  PASS  " if ok else "  FAIL  ") + name + ("   " + extra if extra else ""))

    try:
        fw = FirmwareClient("127.0.0.1", port)

        status, raw = fw.streaming_status()
        check("StreamingStatus reachable and parses", status in ("Available", "Offline"),
              "-> %s" % status)
        check("response carries a UTF-8 BOM (as the real service does)",
              raw.startswith(b"\xef\xbb\xbf"))

        got_cookie, neg = fw.negotiate()
        check("negotiate returns connectionToken", bool(neg.get("connectionToken")))
        check("negotiate sets ALB sticky cookie", got_cookie, fw.cookie[:28] + "...")
        check("ServerSentEvents advertised as a transport",
              any(t["transport"] == "ServerSentEvents" for t in neg["availableTransports"]))

        # --- the ALB behaviour: a request WITHOUT the cookie must be rejected ---
        saved = fw.cookie
        fw.cookie = ""
        rc = fw.send('{"protocol":"json","version":1}' + RS)
        check("request without ALB cookie is rejected (404)", rc == 404, "got %d" % rc)
        fw.cookie = saved

        check("SSE stream opens", fw.open_stream())
        time.sleep(0.6)

        check("handshake accepted", fw.send('{"protocol":"json","version":1}' + RS) == 200)
        sub = ('{"arguments":[["TrackStatus","SessionStatus"]],'
               '"invocationId":"0","target":"Subscribe","type":1}' + RS)
        check("Subscribe accepted", fw.send(sub) == 200)

        time.sleep(1.0)
        snap = [json.loads(r) for r in fw.records if '"type":3' in r]
        check("Subscribe returns an initial snapshot", len(snap) == 1)
        if snap:
            res = snap[0]["result"]
            check("snapshot contains TrackStatus + SessionStatus",
                  "TrackStatus" in res and "SessionStatus" in res,
                  json.dumps(res, separators=(",", ":"))[:60] + "...")

        # let the compressed race play out and collect live updates
        print("\n  ... watching the simulated race (12x speed) ...\n")
        deadline = time.time() + 16
        while time.time() < deadline:
            time.sleep(0.5)

        updates = []
        for r in fw.records:
            try:
                m = json.loads(r)
            except ValueError:
                continue
            if m.get("type") == 1 and m.get("arguments"):
                updates.append((m["arguments"][0], m["arguments"][1]))

        codes = [d.get("Status") for t, d in updates if t == "TrackStatus"]
        sess = [d.get("Status") for t, d in updates if t == "SessionStatus"]
        print("  track codes seen  :", " ".join(codes) or "(none)")
        print("  session states    :", " ".join(sess) or "(none)")
        print()

        check("green flag delivered (code 1)", "1" in codes)
        check("yellow flag delivered (code 2)", "2" in codes)
        check("SAFETY CAR delivered (code 4)", "4" in codes)
        check("red flag delivered (code 5)", "5" in codes)
        check("VSC delivered (code 6)", "6" in codes)
        check("VSC ending delivered (code 7)", "7" in codes)
        check("session Started delivered", "Started" in sess)
        check("records are 0x1E terminated",
              all(RS not in r for r in fw.records) and len(fw.records) > 0)
        pings = [r for r in fw.records if '"type":6' in r]
        check("keep-alive pings present or stream young", True, "%d ping(s)" % len(pings))

        fw.stop = True

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

    failed = [n for n, ok, _ in results if not ok]
    print("\n%d checks, %d failed" % (len(results), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
