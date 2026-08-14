# F1Lamp

An ESP32-C3 Super Mini firmware for the **[F1 WLED Lightbox](https://makerworld.com/en/models/2068365-f1-wled-lightbox#profileId-2233736)** 3D print. It drives three short WS2812B strips (11 LEDs in total) chained on a single data line, and gives you on/off, colour, brightness and a handful of animations — over a built-in web interface, a REST API, or MQTT with auto-discovery for **Domoticz** and Home Assistant.

**Author:** PA1DVB

<p align="center">
  <img src="images/f1lamp.jpg" alt="F1 WLED Lightbox" width="520"><br>
  <sub>Enclosure photo and 3D model: <a href="https://makerworld.com/en/models/2068365-f1-wled-lightbox#profileId-2233736">F1 WLED Lightbox</a> by <b>JoppeDC</b> on MakerWorld,<br>
  licensed <a href="https://creativecommons.org/licenses/by-nc-sa/4.0/">CC BY-NC-SA</a>. Reproduced with attribution — <b>not</b> covered by this repository's MIT licence.<br>
  The model files themselves are not distributed here; download them from MakerWorld.</sub>
</p>

---

## Hardware

| Component | Value |
|-----------|-------|
| MCU | ESP32-C3 Super Mini |
| LED strips | 3 × WS2812B (or WS2811), chained on one data line |
| Default LED count | 4 + 4 + 3 = **11** |
| Segment layout | Strips **1 + 2** light the **"F"**, strip **3** lights the **"1"** |
| Default data pin | GPIO 4 |
| Default colour | Red (`#ff0000`) |

**Where to buy the ESP32-C3 Super Mini:**
- [AliExpress](https://aliexpress.com/item/1005007783677682.html)
- Search for **"ESP32-C3 Super Mini"** on Amazon

**3D model:** [F1 WLED Lightbox](https://makerworld.com/en/models/2068365-f1-wled-lightbox#profileId-2233736) by **JoppeDC** on MakerWorld

### Wiring

```
ESP32-C3 Super Mini            LED strips (chained)
  5V  ──────────────────────►  +5V   (strip 1 → strip 2 → strip 3)
  GND ──────────────────────►  GND
  GPIO4 ───[330 Ω]*─────────►  DIN of strip 1     * optional, see below
                               DOUT strip 1 → DIN strip 2
                               DOUT strip 2 → DIN strip 3
```

The three strips are one continuous chain as far as the firmware is concerned: pixel 0 of strip 2 sits at index `strip1_pixels`, pixel 0 of strip 3 at `strip1_pixels + strip2_pixels`. Only the **per-strip LED counts** need to be configured; you can set any of them to 0 if you wire fewer strips.

The firmware knows the logo is made of two glyphs, and effects use that:

| Glyph | Strips | Default LEDs |
|-------|--------|--------------|
| **F** | strip 1 + strip 2 | 4 + 4 = 8 |
| **1** | strip 3 | 3 |

If your build wires the strips in a different order, just swap the LED counts on the configuration page until **Strip Cycle** lights the segments you expect.

A 330 Ω resistor in the data line and a 470 µF capacitor across 5 V / GND are recommended but not required at this small LED count — 11 LEDs at full white draw roughly 0.6 A, well within what the ESP32-C3's 5 V pin can pass from USB.

### Level shifter (recommended)

The ESP32-C3 drives its GPIOs at **3.3 V**, but a WS2812B wants a logic high of
**0.7 x VDD** - that is **3.5 V** on a 5 V supply. Driving the strip directly is
therefore out of spec, and it shows: the same lamp can work on one USB charger
and produce random per-LED colours, flicker or a "rainbow" on another, because
chargers differ by a couple of hundred millivolts.

A single **74AHCT125** (quad buffer) fixes it properly. The `HCT` family is the
point - its input threshold is about 2 V, so 3.3 V drives it cleanly, while its
output swings the full 5 V the strip wants. Plain `HC` has a ~3.5 V threshold
and will **not** help. An `SN74HCT245` works equally well.

```
   ESP32-C3 SuperMini                74AHCT125                    LED strip 1
                                  (DIP-14, top view)

                              ┌────────┬─┬────────┐
        5V  ─────────┬────────┤14  VCC └─┘        │
                     │        │                   │
                     │   ┌────┤1   1OE            │
       GND  ────┬────┼───┘    │                   │
                │    │        │                   │
     GPIO4  ────┼────┼────────┤2   1A             │
                │    │        │                   │
                │    │        │3   1Y             ├──[330R]*─► DIN
                │    │        │                   │
                │    ├─ 0.1uF ┤7   GND            │
                └────┼────────┤                   │
                     │        │                   │
                     │        │4,10,13  OE ───────┼──► 5V   (unused gates off)
                     │        │5, 9,12  A  ───────┼──► GND  (unused inputs tied)
                     │        └───────────────────┘
                     │
                     └──────────────────────────────► strip +5V
       GND ─────────────────────────────────────────► strip GND
```

| 74AHCT125 pin | Connect to |
|---|---|
| 14 `VCC` | +5 V (same rail as the strip) |
| 7 `GND` | GND (common with the ESP32) |
| 1 `1OE` | **GND** - active low, ties the buffer permanently on |
| 2 `1A` | ESP32 **GPIO4** (data out) |
| 3 `1Y` | strip **DIN** (optionally via a 330 Ohm series resistor) |
| 4, 10, 13 (`2OE`,`3OE`,`4OE`) | +5 V, to disable the three unused outputs |
| 5, 9, 12 (`2A`,`3A`,`4A`) | GND, so unused inputs do not float |
| across 14 and 7 | 0.1 uF ceramic decoupling capacitor |

Keep the data wire short, and give the strip its own 470-1000 uF electrolytic
across +5 V / GND. With the buffer fitted the lamp behaves identically on any
5 V supply.

**\* The 330 Ohm series resistor is optional.** It damps reflections on the data
line and limits current into DIN if the strip is powered while the ESP32 is not.
On a short lead with 11 LEDs the lamp works fine without it - leaving it out is
not a cause of wrong colours. Worth fitting alongside the buffer, since it is
the same joint, but it is belt-and-braces rather than a fix.

**Zero-part alternative for testing:** move the strip's `+` from `5V` to `3V3`.
At 3.3 V supply the threshold drops to ~2.3 V and the 3.3 V data works cleanly.
The LEDs are dimmer and the board's regulator only has ~500 mA to give, so keep
the brightness down - but it is a quick way to confirm that marginal logic
levels, and not the firmware, are causing bad colours.

---

## First Boot — WiFi Setup

On first boot (or after a WiFi reset) the device opens a captive portal access point named after its device ID (e.g. `F1LAMP-AABBCCDDEEFF`). Connect to it with any phone or computer and fill in:

- WiFi SSID / password (selected from a scan list)

All other settings (MQTT, LEDs, colour, effects) are configured through the web interface once WiFi is connected.

The portal closes automatically after 180 seconds of inactivity and reboots the device. If the saved WiFi network is slow to boot, the device waits up to 30 seconds before opening the portal.

| Situation | LEDs |
|-----------|------|
| Connecting to WiFi or MQTT | **Blue blink** (~300 ms on / off) |
| Captive portal open (AP mode) | **Blue breathing** (slow fade in / out) |
| WiFi connected | **Two green blinks** |

---

## Web Interface

Once connected, browse to the device IP address (shown on the serial monitor, or find it in your router's DHCP list).

### Status page — `/`

Displays the device ID, firmware version, current state, colour and effect.

Controls:

- **Turn ON / Turn OFF** buttons
- **Colour picker** — applies and saves immediately
- **Effect** dropdown — applies and saves immediately
- **Brightness slider** — updates the lamp live while you drag, saved when you release

The state badge is refreshed every 3 seconds so changes made through MQTT or the API show up without reloading the page.

### Configuration page — `/config`

| Setting | Description |
|---------|-------------|
| MQTT Enabled | Enable MQTT auto-discovery (Domoticz / Home Assistant) |
| MQTT Server | Broker hostname or IP address |
| MQTT Port | Broker port (default 1883; use 8883 for TLS) |
| MQTT Username | Leave blank if not required |
| MQTT Password | Leave blank if not required |
| MQTT Secure (TLS) | Enable TLS encryption (certificate errors are ignored) |
| MQTT Discovery Prefix | Home Assistant / Domoticz discovery prefix (default `homeassistant`) |
| **Test connection** | Button — tries a real broker login using the values currently in the form and reports the result inline. Nothing is saved and a running MQTT session is left untouched. |
| Strip 1 / 2 — LEDs | LEDs in each half of the **"F"** (0–100 each) |
| Strip 3 — LEDs | LEDs in the **"1"** (0–100) |
| LED data pin (GPIO) | Data pin — dropdown restricted to valid ESP32-C3 pins |
| Pixel color order | RGB byte order of the strip (default NEO_GRB) |
| LED signal speed | 800 KHz (WS2812B) or 400 KHz (WS2811) |
| Colour | Colour picker (default F1 red) |
| Effect | Effect selection (see below) |
| Effect speed | 1 (slow) – 10 (fast) |
| Brightness | Overall brightness (1–255) |
| Turn on after power-up | Whether the lamp lights up by itself after a power cycle |
| **Track live F1** | Mirror the live F1 track status; lamp is held off while no session runs |
| F1 server / TLS | **Development only** - defaults to `livetiming.formula1.com` : 443 with TLS. Only change this to point at the local simulator |

All changes take effect immediately after saving — no reboot needed. Enabling or disabling MQTT blinks the lamp twice to confirm.

### WiFi reset — `/reset`

Clears saved WiFi credentials and reboots into the captive portal.

---

## Formula 1 live tracking

Enable **Track live F1** on the configuration page (or via MQTT / REST) and the lamp mirrors the official F1 race-control status.

**While no session is running the lamp is switched off.** You can still turn it on by hand — from the web UI, the REST API or MQTT — and it stays on until F1 next changes state, because the feed only drives the lamp on a *transition*.

| Track status | Colour | Effect |
|---|---|---|
| Green / all clear | Green | Solid |
| Yellow | Amber | Solid |
| **Safety car** | Amber | **Blink** (alternating F / 1) |
| **Virtual safety car** | Amber | Breathe, slow |
| VSC ending | Amber | Breathe, fast |
| **Red flag** | Red | Solid |

Your own colour, effect and brightness settings are never overwritten — the F1 status is applied as a temporary override, and the moment tracking stops or the session ends your lamp goes straight back to how you had it. Nothing extra is written to flash.

### How it works

Two unauthenticated endpoints on F1's own timing server, no API key and no intermediary server:

| Purpose | Endpoint | Cost |
|---|---|---|
| Is anything live? | `livetiming.formula1.com/static/StreamingStatus.json` | 23 bytes, polled every 2 min |
| Live status | `livetiming.formula1.com/signalrcore` (SignalR Core over SSE) | **~6 bytes/second** |

Only `TrackStatus` and `SessionStatus` are subscribed, which keeps every message under ~200 bytes. `RaceControlMessages` is deliberately *not* subscribed — its first message replays the entire session history (14 kB and growing), which would need a streaming parser for no real benefit here. The cost of that choice is no sector-level yellow flags.

> **Note:** this is F1's own undocumented feed, used the same way FastF1 and MultiViewer use it. It is fine for a personal project, but it is not a licensed or supported API and F1 can change it without warning — the older `/signalr` endpoint already returns 401.

### Reliability

If no data *or* keep-alive ping arrives for 45 seconds (pings normally arrive every ~15 s), the feed is marked **stale**, the connection is dropped and the lamp stops claiming a safety car — it reverts and reconnects with exponential backoff (5 s → 5 min). An unrecognised status code holds the last known good state rather than falling back to green.

### Self-test — `GET /api/f1_test`

F1 sessions are rare, so the safety-car, VSC and red-flag paths are usually untestable when you want to test them. This endpoint replays 18 recorded SignalR records — including two captured verbatim from the live feed — through the exact same parser the live stream uses, and checks the resulting state:

```bash
curl http://192.168.1.42/api/f1_test
```
```
1   PASS empty frame ignored                          track=unknown     session=unknown
2   PASS Subscribe snapshot (real capture)            track=clear       session=finished
...
10  PASS unknown code 3 holds previous state          track=red         session=started
12  PASS two 0x1E-separated records in one line       track=safety_car  session=started

18 passed, 0 failed
```

Returns HTTP 200 when everything passes, 500 otherwise. The lamp visibly cycles green → yellow → safety car → VSC → red as it runs, then returns to what it was doing.

---

## Effects

| # | Name | Description |
|---|------|-------------|
| 0 | **Solid** | All LEDs on in the selected colour (default) |
| 1 | **Breathe** | Smooth fade in / out between off and the set brightness |
| 2 | **Blink** | Alternates between the two glyphs — **F**, then **1**, then **F** … |
| 3 | **Chase** | A single dot runs through all 11 LEDs |
| 4 | **Race Start** | F1 start procedure — the lights come on one by one, hold, then "lights out"; repeats |
| 5 | **Strip Cycle** | Lights one of the three strips at a time |
| 6 | **Rainbow** | Hue sweep across all LEDs (ignores the colour setting) |

Effects are rendered non-blocking from `loop()`, so the web interface and MQTT stay responsive while an animation runs. **Effect speed** (1–10) controls the step interval of every animated effect.

---

## REST API

The REST API works regardless of whether MQTT is enabled. All endpoints return JSON.

### `GET /api/state`

```json
{
  "state": "on",
  "brightness": 128,
  "color": { "r": 255, "g": 0, "b": 0 },
  "effect": 0,
  "effect_name": "Solid",
  "effect_speed": 5,
  "pixels": 11,
  "ip": "192.168.1.42",
  "rssi": -62
}
```

```bash
curl http://192.168.1.42/api/state
```

---

### `POST /api/state`

Sets one or more values. Only the fields you include are changed.

| Field | Type | Values |
|-------|------|--------|
| `state` | string | `"on"` \| `"off"` \| `"toggle"` |
| `brightness` | integer | 0–255 |
| `color` | object | `{"r":0-255,"g":0-255,"b":0-255}` |
| `effect` | integer or string | effect index (0–6) or name (e.g. `"Race Start"`) |
| `effect_speed` | integer | 1–10 |

**Response:** same format as `GET /api/state` with the updated values.

```bash
# Red, full brightness, solid
curl -X POST http://192.168.1.42/api/state \
  -H "Content-Type: application/json" \
  -d '{"state":"on","brightness":255,"color":{"r":255,"g":0,"b":0},"effect":"Solid"}'

# Start the race-start animation
curl -X POST http://192.168.1.42/api/state \
  -H "Content-Type: application/json" \
  -d '{"state":"on","effect":"Race Start","effect_speed":8}'

# Toggle
curl -X POST http://192.168.1.42/api/state \
  -H "Content-Type: application/json" -d '{"state":"toggle"}'
```

Changes made through this endpoint are written to flash at most once every 10 seconds, so rapid colour fades from an automation will not wear out the flash.

---

### `GET /api/mqtt_test`

Attempts a real broker login and reports whether it worked. This is what the **Test** button on the configuration page calls. Optional query parameters override the stored configuration for the duration of the test only — nothing is written to flash, and an already-running MQTT session is not disturbed (the test uses its own client ID, `<ID>-test`).

| Parameter | Description |
|-----------|-------------|
| `server` | Broker hostname or IP |
| `port` | Broker port |
| `username` / `password` | Credentials; empty means anonymous |
| `secure` | `1` for TLS, `0` for plain |

```json
{ "ok": false, "state": 4, "message": "Bad username or password" }
```

`state` is the raw PubSubClient state code; `message` is the human-readable form of it (`Cannot reach broker`, `Bad username or password`, `Not authorised`, `Broker unavailable`, `Connection timed out`, …).

```bash
curl "http://192.168.1.42/api/mqtt_test?server=192.168.1.10&port=1883&username=hass&password=secret&secure=0"
```

---

### `GET /api/brightness?v=<0-255>`

Live brightness preview — applies immediately but is **not** saved. Used by the sliders in the web interface while dragging.

---

### `POST /api/color`

Fills all LEDs with a single RGB colour immediately. Useful for verifying the **Pixel color order** setting — send pure red, green or blue and check what actually lights up. The effect is temporary; the next state change or animation frame restores the normal display.

```bash
curl -X POST http://192.168.1.42/api/color \
  -H "Content-Type: application/json" -d '{"r":255,"g":0,"b":0}'
```

---

## MQTT / Domoticz / Home Assistant

MQTT is **disabled by default**. Enable it on the Configuration page, fill in the broker details, and press **Save**. The lamp blinks twice to confirm; no reboot is needed.

When enabled, the firmware publishes MQTT Auto Discovery configs and registers **four entities** on one device. Both **Domoticz** and Home Assistant consume the same discovery format, so no extra configuration is needed on either:

1. A **light** — on/off, brightness, RGB colour and the effect list.
2. A **select** — the effect on its own, so it can be used directly from a dashboard, script or Domoticz without digging into the light's more-info dialog or writing a template.
3. A **switch** — Formula 1 live tracking on/off.
4. A **sensor** — the current F1 track status (`clear` / `yellow` / `safety_car` / `vsc` / `vsc_ending` / `red` / `unknown`).

All four are driven by the same state topic, so they always agree.

| Topic | Description |
|-------|-------------|
| `esp32-f1lamp/<ID>/status` | Availability (`online` / `offline`) |
| `esp32-f1lamp/<ID>/state` | State JSON (read) |
| `esp32-f1lamp/<ID>/command` | Command JSON (write) — light entity |
| `esp32-f1lamp/<ID>/effect/set` | Effect name as plain text (write) — select entity |

**State payload:**
```json
{
  "state": "ON",
  "brightness": 128,
  "color_mode": "rgb",
  "color": { "r": 255, "g": 0, "b": 0 },
  "effect": "Solid"
}
```

**Command payload** (sent by Domoticz or Home Assistant, JSON schema):
```json
{ "state": "ON", "brightness": 200, "color": { "r": 255, "g": 0, "b": 0 }, "effect": "Race Start" }
```

```bash
# via the light entity (JSON)
mosquitto_pub -h <broker> \
  -t "esp32-f1lamp/F1LAMP-AABBCCDDEEFF/command" \
  -m '{"state":"ON","effect":"Race Start"}'

# via the select entity (plain effect name)
mosquitto_pub -h <broker> \
  -t "esp32-f1lamp/F1LAMP-AABBCCDDEEFF/effect/set" \
  -m "Race Start"
```

The select accepts any name from the effect list exactly as spelled in the table above (matching is case-insensitive). An unknown name is ignored and logged to the serial port.

The device ID is `F1LAMP-` followed by the full 6-byte MAC address in uppercase hex (visible on the status page and in the serial log).

---

## OTA Firmware Update

OTA (Over-The-Air) updates are supported via the Arduino IDE or `espota.py`.

- **Hostname:** device ID (e.g. `F1LAMP-AABBCCDDEEFF`)
- **Password:** same as the device ID

In the Arduino IDE the device appears under **Tools → Port → Network ports** after a few seconds.

---

## Serial Log

On boot the device prints a summary to the serial port (115200 baud):

```
App version:  2026.08.13 rev 1.0
LED pin:      GPIO4,  pixels: 11 (4+4+3)
CPU freq:     160 MHz
Device ID:    F1LAMP-AABBCCDDEEFF
MQTT availability: esp32-f1lamp/F1LAMP-AABBCCDDEEFF/status
WiFi: connecting...
WiFi: connected.  IP: 192.168.1.42  RSSI: -58 dBm
```

---

## Configuration Storage

All settings are stored in SPIFFS as `/config.json`. If the filesystem cannot be mounted it is formatted automatically and all settings reset to defaults.

| Setting | Default |
|---------|---------|
| mqtt_enabled | false |
| mqtt_secure | false |
| mqtt_server | `example.tld` |
| mqtt_port | 1883 |
| mqtt_username | *(empty)* |
| mqtt_password | *(empty)* |
| mqtt_discovery_prefix | `homeassistant` |
| strip1_pixels | 4 |
| strip2_pixels | 4 |
| strip3_pixels | 3 |
| led_pin | 4 |
| pixel_color_order | 82 (NEO_GRB) |
| pixel_khz | 800 |
| color_r / g / b | 255 / 0 / 0 (red) |
| base_brightness | 128 |
| effect | 0 (Solid) |
| effect_speed | 5 |
| power_on_boot | true |
| f1_enabled | false |
| f1_host | `livetiming.formula1.com` |
| f1_port | 443 |
| f1_tls | true |

---

## Dependencies (Arduino Libraries)

Install via the Arduino Library Manager:

| Library | Purpose |
|---------|---------|
| Adafruit NeoPixel | WS2812B LED control |
| ArduinoJson | JSON serialisation |
| ArduinoOTA | OTA firmware updates |
| PubSubClient | MQTT client |
| WiFiManager (tzapu) | Captive portal WiFi setup |
| WebServer (built-in) | HTTP server |
| SPIFFS (built-in) | Filesystem for config storage |

Board: **ESP32C3 Dev Module** (or compatible) via the ESP32 Arduino core.

> **USB CDC On Boot must be Enabled** (`Tools -> USB CDC On Boot -> Enabled`, or
> `CDCOnBoot=cdc` on the arduino-cli FQBN). With it disabled, `Serial` is routed to
> UART0 on GPIO20/21 instead of the USB port and you get no serial output at all.

> **Partition scheme:** The sketch exceeds the default 1.25 MB app partition because `WiFiClientSecure` (used for MQTT over TLS) pulls in mbedTLS. In the Arduino IDE select **Tools → Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** before uploading. This keeps OTA support and leaves ample room for `config.json` in SPIFFS.

---

## Project Structure

```
F1Lamp/
├── F1Lamp.ino               # Main file: globals, setup(), loop()
├── Config.h                 # All persistent settings + SPIFFS load/save
├── F1Client.ino             # F1 live timing: StreamingStatus poll + SignalR Core over SSE
├── F1SelfTest.ino           # Offline parser self-test (GET /api/f1_test)
├── LEDControl.ino           # LED rendering, effects, connection animations
├── MQTTAutoDiscovery.ino    # MQTT state publishing, HA auto-discovery, command callback
├── WebServer.ino            # Web UI handlers + REST API
└── WiFiMqttSetup.ino        # WiFi captive portal, OTA, MQTT reconnect

tools/                       # development only - not needed to build or run
├── f1_sim.py                # F1 live timing simulator with console display
└── f1_sim_selftest.py       # verifies the simulator against a firmware-alike client
```

---

## Licence

The firmware in this repository is **MIT** licensed — see [LICENSE](LICENSE).

One file is excluded from that licence:

| File | Licence |
|------|---------|
| `images/f1lamp.jpg` | [CC BY-NC-SA](https://creativecommons.org/licenses/by-nc-sa/4.0/) — photo of the [F1 WLED Lightbox](https://makerworld.com/en/models/2068365-f1-wled-lightbox#profileId-2233736) model by **JoppeDC** on MakerWorld, reproduced with attribution. **Not for commercial use.** |

The 3D model files are not distributed here. This firmware is independent software that drives the LEDs inside the printed enclosure — it is not an adaptation of the model, so the ShareAlike term does not reach the source code.
