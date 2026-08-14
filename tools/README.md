# Development tools

> **These are development-only. Nothing in this folder is needed to build, flash or run the lamp.**
> A finished device talks to the real F1 timing service and never touches anything here.

## `f1_sim.py` — F1 live timing simulator

Stands in for `livetiming.formula1.com` so the safety-car, VSC and red-flag paths can be exercised without waiting for a race weekend — F1 runs about two dozen sessions a year, and none of them happen when you want to test.

It speaks the same protocol the real service does, including the two details that break naive implementations: the **AWS load balancer sticky cookie** and **`0x1E` record separators**. Requests that fail to replay the cookie get the same `404 / No Connection with that ID` the real service gives.

No third-party packages — standard library only.

```bash
python f1_sim.py                    # full race on port 8080
python f1_sim.py --list             # show scenarios
python f1_sim.py --scenario stale   # go silent mid-race
python f1_sim.py --speed 4          # 4x faster
python f1_sim.py --loop             # restart when finished
```

| Scenario | What it exercises |
|---|---|
| `race` | Green, yellows, safety car, VSC, red flag, restart, chequered flag, finish |
| `quali` | Short session with one red flag |
| `stale` | **Goes silent mid-safety-car** — the lamp must stop claiming SC within 45 s |
| `flaky` | Repeatedly drops the connection — reconnect and backoff |
| `offline` | Nothing live — lamp must sit dark and just poll |

While running it draws the session in the console: current state, what the lamp *should* be showing, a timeline strip, byte counts, a live protocol checklist and an event log.

```
==============================================================================
  F1 LIVE TIMING SIMULATOR                               t=  78.1s  race  8.0x
==============================================================================

  listening  http://0.0.0.0:8080      client: 192.168.1.42

  Streaming  Available   Session  Started   Track  VSC

  Lamp should be:  ###  amber, breathe slow

  G green  Y yellow  S safety car  V vsc  v ending  R red  . no session
  GGGGGGGGGGGYYYYYYYYYYYYGGGGGGGGGGGGGSSSSSSSSSSSSSSSSSGGGGGGGGGGGGGVVVV

  SSE clients 1     connects 1     TX 946 B     pings 5

  PROTOCOL CHECKS
   [PASS] StreamingStatus.json polled
   [PASS] negotiate called
   [PASS] ALB sticky cookie replayed on SSE GET
   [PASS] SSE GET sends Accept: text/event-stream
   [PASS] handshake {protocol:json,version:1} + 0x1E
   [PASS] Subscribe topics = TrackStatus, SessionStatus
   [PASS] RaceControlMessages NOT subscribed (bandwidth)
   [ .. ] reconnected after a drop

  EVENT LOG
      45.0s  TrackStatus -> 4 (SCDeployed)
      46.0s  Safety car deployed
      62.0s  TrackStatus -> 1 (AllClear)
      75.0s  TrackStatus -> 6 (VSCDeployed)
```

### Pointing the lamp at it

On the lamp's `/config` page, under **Formula 1 live tracking**:

| Field | Value |
|---|---|
| F1 server | `<your PC's IP>` : `8080` |
| F1 server uses TLS | **unchecked** (the simulator is plain HTTP) |
| Track live F1 | checked |

The status and configuration pages both show a **DEV MODE** banner whenever the host is not the real service, so a test setting cannot quietly survive into normal use.

**To go back to normal**, set the host to `livetiming.formula1.com`, port `443`, TLS **on**. The banner disappears.

## `f1_sim_selftest.py` — verifies the simulator

Drives `f1_sim.py` with a client that mimics `F1Client.ino` step for step — poll, negotiate, capture cookie, open SSE, handshake, Subscribe, parse `0x1E` records — and asserts the whole exchange, including that a request *without* the ALB cookie is correctly rejected.

```bash
python f1_sim_selftest.py
```
```
  PASS  StreamingStatus reachable and parses   -> Available
  PASS  response carries a UTF-8 BOM (as the real service does)
  PASS  negotiate sets ALB sticky cookie
  PASS  request without ALB cookie is rejected (404)   got 404
  PASS  SAFETY CAR delivered (code 4)
  ...
  20 checks, 0 failed
```

It starts and stops the simulator itself on a spare port, and exits non-zero on failure, so it works in CI.

## See also

`GET /api/f1_test` on the device runs an offline parser self-test with recorded records — no simulator or network needed. See the main [README](../README.md#self-test--apif1_test).
