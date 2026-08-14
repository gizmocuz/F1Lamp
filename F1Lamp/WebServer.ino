// WebServer.ino - Web UI handlers + REST API

// ---------------------------------------------------------------------------
// handleWebRoot - Main status and control page
// ---------------------------------------------------------------------------

void handleWebRoot() {
    char colorHex[8];
    snprintf(colorHex, sizeof(colorHex), "#%02x%02x%02x",
             Config::color_r, Config::color_g, Config::color_b);

    String html;
    html.reserve(4096);

    html += R"rawhtml(<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><link rel="icon" href="data:,"><title>F1 Lamp</title><style>body { font-family: Helvetica; max-width: 480px; margin: 0 auto; padding: 12px; text-align: center; background: #111; color: #eee; }h1 { font-size: 1.4em; color: #E10600; }a { color: #7ac6ff; }.state-on  { display: inline-block; padding: 4px 16px; border-radius: 4px; background: #E10600; color: #fff; font-weight: bold; }.state-off { display: inline-block; padding: 4px 16px; border-radius: 4px; background: #555;    color: #fff; font-weight: bold; }.swatch { display: inline-block; width: 22px; height: 22px; border-radius: 4px; border: 1px solid #666; vertical-align: middle; }input[type=range] { width: 80%; }select, input[type=color] { font-size: 1em; padding: 2px; }.btn { display: inline-block; background: #333; color: #fff; padding: 10px 28px; border: none; border-radius: 4px; font-size: 1em; cursor: pointer; text-decoration: none; margin: 4px; }.btn-on  { background: #E10600; }.btn-off { background: #555; }hr { margin: 16px 0; border: 0; border-top: 1px solid #333; }</style></head><body><h1>F1 Lamp</h1><p><b>Device:</b> )rawhtml";
    html += identifier;
    html += R"rawhtml(</p><p><b>Firmware:</b> )rawhtml";
    html += app_version;
    html += R"rawhtml(</p><p><b>Author:</b> PA1DVB</p><hr>)rawhtml";

    // State indicator
    html += R"rawhtml(<p><b>State:</b> <span id="st" class=")rawhtml";
    html += ledState ? "state-on" : "state-off";
    html += R"rawhtml(">)rawhtml";
    html += ledState ? "ON" : "OFF";
    html += R"rawhtml(</span></p>)rawhtml";

    // Colour + effect summary
    html += R"rawhtml(<p><b>Colour:</b> <span class="swatch" style="background:)rawhtml";
    html += colorHex;
    html += R"rawhtml("></span> )rawhtml";
    html += colorHex;
    html += R"rawhtml(</p><p><b>Effect:</b> )rawhtml";
    html += effectName(Config::effect);
    html += R"rawhtml(</p>)rawhtml";

    // F1 live tracking status
    if (Config::f1_enabled) {
        if (!f1UsingRealService()) {
            html += R"rawhtml(<p style="background:#5a3a00;border:1px solid #c78400;border-radius:4px;padding:6px"><b>DEV MODE</b><br>F1 data is coming from <b>)rawhtml";
            html += Config::f1_host;
            html += R"rawhtml(</b>, not the real F1 service.<br><a href="/config">Fix on the configuration page</a></p>)rawhtml";
        }
        html += R"rawhtml(<p><b>F1 tracking:</b> <span style="color:#4caf50">on</span> &mdash; feed <b>)rawhtml";
        html += f1FeedName(f1Feed);
        html += R"rawhtml(</b></p>)rawhtml";
        if (f1IsSessionLive()) {
            html += R"rawhtml(<p><b>Track status:</b> )rawhtml";
            html += f1TrackName(f1Track);
            html += R"rawhtml( &nbsp;|&nbsp; <b>Session:</b> )rawhtml";
            html += f1SessionStateName(f1Session);
            html += R"rawhtml(</p>)rawhtml";
        } else {
            html += R"rawhtml(<p><small>No session live &mdash; lamp is held off.</small></p>)rawhtml";
        }
    }

    html += R"rawhtml(<hr>)rawhtml";

    // On / off
    html += R"rawhtml(<p><a class="btn btn-on" href="/set?state=on">Turn ON</a> <a class="btn btn-off" href="/set?state=off">Turn OFF</a></p>)rawhtml";

    // Colour picker - submits on release
    html += R"rawhtml(<form method="GET" action="/set"><p><b>Colour:</b> <input type="color" name="color" value=")rawhtml";
    html += colorHex;
    html += R"rawhtml(" onchange="this.form.submit()"></p></form>)rawhtml";

    // Effect selector
    html += R"rawhtml(<form method="GET" action="/set"><p><b>Effect:</b> <select name="effect" onchange="this.form.submit()">)rawhtml";
    for (int i = 0; i < EFFECT_COUNT; i++) {
        html += R"rawhtml(<option value=")rawhtml" + String(i) + R"rawhtml(")rawhtml";
        if (Config::effect == i) html += R"rawhtml( selected)rawhtml";
        html += String(">") + EFFECT_NAMES[i] + R"rawhtml(</option>)rawhtml";
    }
    html += R"rawhtml(</select></p></form>)rawhtml";

    // Brightness - live preview while dragging, saved on release
    html += R"rawhtml(<p><b>Brightness:</b> <span id="bv">)rawhtml"
        + String(currentBrightness) +
        R"rawhtml(</span><br><input type="range" min="1" max="255" value=")rawhtml"
        + String(currentBrightness) +
        R"rawhtml(" oninput="document.getElementById('bv').textContent=this.value;fetch('/api/brightness?v='+this.value)" onchange="fetch('/set?brightness='+this.value)"></p>)rawhtml";

    html += R"rawhtml(<hr><p><a href="/config">Configuration</a></p>)rawhtml";

    // Poll the state badge so external (MQTT) changes show up without a full reload
    html += R"rawhtml(<script>setInterval(function(){fetch('/api/state').then(function(r){return r.json()}).then(function(j){var e=document.getElementById('st');e.textContent=j.state.toUpperCase();e.className=(j.state=='on')?'state-on':'state-off';})},3000);</script></body></html>)rawhtml";

    webServer.send(200, "text/html; charset=utf-8", html);
}

// ---------------------------------------------------------------------------
// handleWebSet - Apply state / colour / effect / brightness, then redirect to /
// ---------------------------------------------------------------------------

void handleWebSet() {
    bool changed = false;

    if (webServer.hasArg("state")) {
        const String s = webServer.arg("state");
        if      (s == "toggle") ledState = !ledState;
        else                    ledState = (s == "on");
    }
    if (webServer.hasArg("color")) {
        String c = webServer.arg("color");
        if (c.startsWith("#")) c = c.substring(1);
        if (c.length() == 6) {
            Config::color_r = (uint8_t)strtol(c.substring(0, 2).c_str(), nullptr, 16);
            Config::color_g = (uint8_t)strtol(c.substring(2, 4).c_str(), nullptr, 16);
            Config::color_b = (uint8_t)strtol(c.substring(4, 6).c_str(), nullptr, 16);
            changed = true;
        }
    }
    if (webServer.hasArg("effect")) {
        const int e = webServer.arg("effect").toInt();
        if (e >= 0 && e < EFFECT_COUNT) {
            Config::effect = e;
            changed = true;
        }
    }
    if (webServer.hasArg("brightness")) {
        currentBrightness       = (uint8_t)constrain(webServer.arg("brightness").toInt(), 0, 255);
        Config::base_brightness = currentBrightness;
        changed = true;
    }

    if (changed) {
        Config::save();
        configDirty = false;
    }

    updateLEDs();
    publishState();
    webServer.sendHeader("Location", "/");
    webServer.send(302, "text/plain", "");
}

// ---------------------------------------------------------------------------
// handleWebConfig - Configuration form (GET)
// ---------------------------------------------------------------------------

void handleWebConfig() {
    char colorHex[8];
    snprintf(colorHex, sizeof(colorHex), "#%02x%02x%02x",
             Config::color_r, Config::color_g, Config::color_b);

    String html;
    html.reserve(6144);

    html += R"rawhtml(<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><link rel="icon" href="data:,"><title>F1 Lamp - Configuration</title><style>body { font-family: Helvetica; max-width: 480px; margin: 0 auto; padding: 12px; background: #111; color: #eee; }h1 { font-size: 1.4em; text-align: center; color: #E10600; }a { color: #7ac6ff; }table { width: 100%; border-collapse: collapse; }td { padding: 4px 4px; vertical-align: middle; }td:first-child { width: 55%; font-weight: bold; }input[type=number], input[type=color] { width: 80px; }input[type=range] { width: 100%; }input, select { font-size: 1em; }.btn { display: inline-block; background: #E10600; color: #fff; padding: 10px 28px; border: none; border-radius: 4px; font-size: 1em; cursor: pointer; text-decoration: none; }hr { margin: 16px 0; border: 0; border-top: 1px solid #333; }small { color: #999; }</style></head><body><h1>Configuration</h1><form method="POST" action="/config"><table>)rawhtml";

    // --- MQTT section ---
    html += R"rawhtml(<tr><td colspan="2"><b>MQTT / Home Assistant</b></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>MQTT Enabled:</td><td><input type="checkbox" name="mqtt_enabled")rawhtml";
    if (Config::mqtt_enabled) html += R"rawhtml( checked)rawhtml";
    html += R"rawhtml(></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>MQTT Server:</td><td><input type="text" name="mqtt_server" value=")rawhtml";
    html += Config::mqtt_server;
    html += R"rawhtml(" maxlength="79" style="width:200px"></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>MQTT Port:</td><td><input type="number" name="mqtt_port" value=")rawhtml"
        + String(Config::mqtt_port) +
        R"rawhtml(" min="1" max="65535" style="width:90px"></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>MQTT Username:</td><td><input type="text" name="mqtt_username" value=")rawhtml";
    html += Config::mqtt_username;
    html += R"rawhtml(" maxlength="23" style="width:160px"></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>MQTT Password:</td><td><input type="password" name="mqtt_password" value=")rawhtml";
    html += Config::mqtt_password;
    html += R"rawhtml(" maxlength="23" autocomplete="off" style="width:160px"></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>MQTT Secure (TLS):</td><td><input type="checkbox" name="mqtt_secure")rawhtml";
    if (Config::mqtt_secure) html += R"rawhtml( checked)rawhtml";
    html += R"rawhtml(></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>MQTT Discovery Prefix:</td><td><input type="text" name="mqtt_discovery_prefix" value=")rawhtml";
    html += Config::mqtt_discovery_prefix;
    html += R"rawhtml(" maxlength="31" style="width:160px"></td></tr>)rawhtml";

    // Test the broker login using the values currently in the form (nothing is saved)
    html += R"rawhtml(<tr><td>Test connection:</td><td><button type="button" id="mqttbtn" style="background:#333;color:#fff;border:0;border-radius:4px;padding:6px 18px;font-size:1em;cursor:pointer" onclick="mqttTest()">Test</button><br><small id="mqttres"></small></td></tr>)rawhtml";

    // --- LED hardware section ---
    html += R"rawhtml(<tr><td colspan="2"><hr></td></tr><tr><td colspan="2"><b>LED strips</b></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>Strip 1 - LEDs ("F"):</td><td><input type="number" name="strip1_pixels" value=")rawhtml"
        + String(Config::strip1_pixels) +
        R"rawhtml(" min="0" max="100"></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>Strip 2 - LEDs ("F"):</td><td><input type="number" name="strip2_pixels" value=")rawhtml"
        + String(Config::strip2_pixels) +
        R"rawhtml(" min="0" max="100"></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>Strip 3 - LEDs ("1"):</td><td><input type="number" name="strip3_pixels" value=")rawhtml"
        + String(Config::strip3_pixels) +
        R"rawhtml(" min="0" max="100"><br><small>Total: )rawhtml"
        + String(totalPixels()) +
        R"rawhtml( LEDs. Strips 1 + 2 form the "F", strip 3 forms the "1"; all chained on one data line.</small></td></tr>)rawhtml";

    // led_pin - dropdown restricted to valid ESP32-C3 Super Mini GPIO pins
    html += R"rawhtml(<tr><td>LED data pin (GPIO):</td><td><select name="led_pin">)rawhtml";
    int validPins[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 20, 21};
    for (int pin : validPins) {
        html += R"rawhtml(<option value=")rawhtml" + String(pin) + R"rawhtml(")rawhtml";
        if (Config::led_pin == pin) html += R"rawhtml( selected)rawhtml";
        html += R"rawhtml(>GPIO )rawhtml" + String(pin) + R"rawhtml(</option>)rawhtml";
    }
    html += R"rawhtml(</select></td></tr>)rawhtml";

    // pixel color order
    {
        struct { const char* label; uint8_t val; } orders[] = {
            {"NEO_GRB (most strips)", 82},
            {"NEO_RGB", 6},
            {"NEO_RBG", 9},
            {"NEO_BRG", 88},
            {"NEO_BGR", 54},
            {"NEO_GBR", 98},
        };
        html += R"rawhtml(<tr><td>Pixel color order:</td><td><select name="pixel_color_order">)rawhtml";
        for (auto& o : orders) {
            html += R"rawhtml(<option value=")rawhtml" + String(o.val) + R"rawhtml(")rawhtml";
            if (Config::pixel_color_order == o.val) html += R"rawhtml( selected)rawhtml";
            html += String(">") + o.label + R"rawhtml(</option>)rawhtml";
        }
        html += R"rawhtml(</select><br><small>Wrong colours? Try another order - POST /api/color to test.</small></td></tr>)rawhtml";
    }

    // pixel kHz
    html += R"rawhtml(<tr><td>LED signal speed:</td><td><select name="pixel_khz">)rawhtml";
    html += R"rawhtml(<option value="800")rawhtml";
    if (Config::pixel_khz == 800) html += R"rawhtml( selected)rawhtml";
    html += R"rawhtml(>800 KHz (WS2812B)</option>)rawhtml";
    html += R"rawhtml(<option value="400")rawhtml";
    if (Config::pixel_khz == 400) html += R"rawhtml( selected)rawhtml";
    html += R"rawhtml(>400 KHz (WS2811)</option>)rawhtml";
    html += R"rawhtml(</select></td></tr>)rawhtml";

    // --- Appearance section ---
    html += R"rawhtml(<tr><td colspan="2"><hr></td></tr><tr><td colspan="2"><b>Appearance</b></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>Colour:</td><td><input type="color" name="color" value=")rawhtml";
    html += colorHex;
    html += R"rawhtml("></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>Effect:</td><td><select name="effect">)rawhtml";
    for (int i = 0; i < EFFECT_COUNT; i++) {
        html += R"rawhtml(<option value=")rawhtml" + String(i) + R"rawhtml(")rawhtml";
        if (Config::effect == i) html += R"rawhtml( selected)rawhtml";
        html += String(">") + EFFECT_NAMES[i] + R"rawhtml(</option>)rawhtml";
    }
    html += R"rawhtml(</select></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>Effect speed ()rawhtml"
        + String(Config::effect_speed) +
        R"rawhtml():</td><td><input type="range" name="effect_speed" value=")rawhtml"
        + String(Config::effect_speed) +
        R"rawhtml(" min="1" max="10" oninput="this.parentElement.previousElementSibling.textContent='Effect speed ('+this.value+'):'"><br><small>1 = slow, 10 = fast</small></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>Brightness ()rawhtml"
        + String(Config::base_brightness) +
        R"rawhtml():</td><td><input type="range" name="base_brightness" value=")rawhtml"
        + String(Config::base_brightness) +
        R"rawhtml(" min="1" max="255" oninput="this.parentElement.previousElementSibling.textContent='Brightness ('+this.value+'):';fetch('/api/brightness?v='+this.value)"></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>Turn on after power-up:</td><td><input type="checkbox" name="power_on_boot")rawhtml";
    if (Config::power_on_boot) html += R"rawhtml( checked)rawhtml";
    html += R"rawhtml(></td></tr>)rawhtml";

    // --- F1 live tracking section ---
    html += R"rawhtml(<tr><td colspan="2"><hr></td></tr><tr><td colspan="2"><b>Formula 1 live tracking</b></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>Track live F1:</td><td><input type="checkbox" name="f1_enabled")rawhtml";
    if (Config::f1_enabled) html += R"rawhtml( checked)rawhtml";
    html += R"rawhtml(><br><small>Mirrors the official F1 track status: green / yellow / safety car (blink) / VSC (breathe) / red.<br>While no session is running the lamp is switched <b>off</b>. You can still turn it on by hand or over MQTT.</small></td></tr>)rawhtml";

    if (Config::f1_enabled) {
        html += R"rawhtml(<tr><td>Feed state:</td><td>)rawhtml";
        html += f1FeedName(f1Feed);
        html += R"rawhtml( &nbsp;|&nbsp; track: )rawhtml";
        html += f1TrackName(f1Track);
        html += R"rawhtml(</td></tr>)rawhtml";
    }

    if (!f1UsingRealService()) {
        html += R"rawhtml(<tr><td colspan="2"><div style="background:#5a3a00;border:1px solid #c78400;border-radius:4px;padding:6px"><b>DEV MODE</b> &mdash; pointed at a test server, not the real F1 service. Set the host back to <code>livetiming.formula1.com</code>, port <code>443</code>, TLS on.</div></td></tr>)rawhtml";
    }
    html += R"rawhtml(<tr><td>F1 server:</td><td><input type="text" name="f1_host" value=")rawhtml";
    html += Config::f1_host;
    html += R"rawhtml(" maxlength="63" style="width:200px"> : <input type="number" name="f1_port" value=")rawhtml"
        + String(Config::f1_port) +
        R"rawhtml(" min="1" max="65535" style="width:70px"><br><small>Leave as <code>livetiming.formula1.com</code> : 443. Only change this for development, to point at <code>tools/f1_sim.py</code>.</small></td></tr>)rawhtml";

    html += R"rawhtml(<tr><td>F1 server uses TLS:</td><td><input type="checkbox" name="f1_tls")rawhtml";
    if (Config::f1_tls) html += R"rawhtml( checked)rawhtml";
    html += R"rawhtml(><br><small>On for the real service. Off for the simulator (plain HTTP).</small></td></tr>)rawhtml";

    html += R"rawhtml(</table><br><div style="text-align:center"><button class="btn" type="submit">Save</button></div></form><hr><p style="text-align:center"><a href="/">Back to status</a> &nbsp;|&nbsp; <a href="/reset" style="color:#E74C3C" onclick="return confirm('Reset WiFi settings and reboot into AP mode?')">Reset WiFi</a></p>)rawhtml";

    html += R"rawhtml(<script>function mqttTest(){var f=document.forms[0];var r=document.getElementById('mqttres');var b=document.getElementById('mqttbtn');var q='?server='+encodeURIComponent(f.mqtt_server.value)+'&port='+encodeURIComponent(f.mqtt_port.value)+'&username='+encodeURIComponent(f.mqtt_username.value)+'&password='+encodeURIComponent(f.mqtt_password.value)+'&secure='+(f.mqtt_secure.checked?'1':'0');b.disabled=true;r.style.color='#999';r.textContent='Testing...';fetch('/api/mqtt_test'+q).then(function(x){return x.json()}).then(function(j){r.style.color=j.ok?'#4caf50':'#E74C3C';r.textContent=j.message;b.disabled=false;}).catch(function(){r.style.color='#E74C3C';r.textContent='Request failed';b.disabled=false;});}</script></body></html>)rawhtml";

    webServer.send(200, "text/html; charset=utf-8", html);
}

// ---------------------------------------------------------------------------
// handleWebConfigSave - Persist settings (POST), then redirect to /
// ---------------------------------------------------------------------------

void handleWebConfigSave() {
    const bool wasMqttEnabled = Config::mqtt_enabled;

    // Checkboxes: present means checked, absent means unchecked
    Config::mqtt_enabled  = webServer.hasArg("mqtt_enabled");
    Config::mqtt_secure   = webServer.hasArg("mqtt_secure");
    Config::power_on_boot = webServer.hasArg("power_on_boot");
    // Remember the old F1 settings so we can tell whether the client needs
    // restarting - otherwise it would sit out the remaining 2 min poll timer
    // (or up to a 5 min backoff) before noticing the new server.
    char       oldF1Host[sizeof(Config::f1_host)];
    strlcpy(oldF1Host, Config::f1_host, sizeof(oldF1Host));
    const bool oldF1Enabled = Config::f1_enabled;
    const int  oldF1Port    = Config::f1_port;
    const bool oldF1Tls     = Config::f1_tls;

    Config::f1_enabled    = webServer.hasArg("f1_enabled");
    Config::f1_tls        = webServer.hasArg("f1_tls");

    if (webServer.hasArg("f1_host")) {
        String h = webServer.arg("f1_host"); h.trim();
        if (h.length() > 0) strlcpy(Config::f1_host, h.c_str(), sizeof(Config::f1_host));
    }
    if (webServer.hasArg("f1_port"))
        Config::f1_port = constrain((int)webServer.arg("f1_port").toInt(), 1, 65535);

    const bool f1Changed = (Config::f1_enabled != oldF1Enabled)
                        || (Config::f1_port    != oldF1Port)
                        || (Config::f1_tls     != oldF1Tls)
                        || (strcmp(Config::f1_host, oldF1Host) != 0);

    if (webServer.hasArg("mqtt_server")) {
        String s = webServer.arg("mqtt_server"); s.trim();
        strlcpy(Config::mqtt_server, s.c_str(), sizeof(Config::mqtt_server));
    }
    if (webServer.hasArg("mqtt_port"))
        Config::mqtt_port = constrain((int)webServer.arg("mqtt_port").toInt(), 1, 65535);
    if (webServer.hasArg("mqtt_username")) {
        String u = webServer.arg("mqtt_username"); u.trim();
        strlcpy(Config::mqtt_username, u.c_str(), sizeof(Config::mqtt_username));
    }
    if (webServer.hasArg("mqtt_password") && webServer.arg("mqtt_password").length() > 0)
        strlcpy(Config::mqtt_password, webServer.arg("mqtt_password").c_str(), sizeof(Config::mqtt_password));
    if (webServer.hasArg("mqtt_discovery_prefix")) {
        String p = webServer.arg("mqtt_discovery_prefix"); p.trim();
        if (p.length() > 0)
            strlcpy(Config::mqtt_discovery_prefix, p.c_str(), sizeof(Config::mqtt_discovery_prefix));
    }

    // --- LED strips ---
    int s1 = Config::strip1_pixels, s2 = Config::strip2_pixels, s3 = Config::strip3_pixels;
    if (webServer.hasArg("strip1_pixels")) s1 = constrain((int)webServer.arg("strip1_pixels").toInt(), 0, 100);
    if (webServer.hasArg("strip2_pixels")) s2 = constrain((int)webServer.arg("strip2_pixels").toInt(), 0, 100);
    if (webServer.hasArg("strip3_pixels")) s3 = constrain((int)webServer.arg("strip3_pixels").toInt(), 0, 100);
    if (s1 + s2 + s3 >= 1) {   // at least one LED in total
        Config::strip1_pixels = s1;
        Config::strip2_pixels = s2;
        Config::strip3_pixels = s3;
    }

    if (webServer.hasArg("led_pin")) {
        const int p = webServer.arg("led_pin").toInt();
        int validLedPins[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 20, 21};
        for (int vp : validLedPins) { if (p == vp) { Config::led_pin = p; break; } }
    }
    if (webServer.hasArg("pixel_color_order")) {
        const uint8_t co = (uint8_t)webServer.arg("pixel_color_order").toInt();
        uint8_t validOrders[] = {6, 9, 82, 88, 54, 98};
        for (uint8_t v : validOrders) { if (co == v) { Config::pixel_color_order = co; break; } }
    }
    if (webServer.hasArg("pixel_khz")) {
        const int k = webServer.arg("pixel_khz").toInt();
        if (k == 400 || k == 800) Config::pixel_khz = k;
    }

    // --- Appearance ---
    if (webServer.hasArg("color")) {
        String c = webServer.arg("color");
        if (c.startsWith("#")) c = c.substring(1);
        if (c.length() == 6) {
            Config::color_r = (uint8_t)strtol(c.substring(0, 2).c_str(), nullptr, 16);
            Config::color_g = (uint8_t)strtol(c.substring(2, 4).c_str(), nullptr, 16);
            Config::color_b = (uint8_t)strtol(c.substring(4, 6).c_str(), nullptr, 16);
        }
    }
    if (webServer.hasArg("effect")) {
        const int e = webServer.arg("effect").toInt();
        if (e >= 0 && e < EFFECT_COUNT) Config::effect = e;
    }
    if (webServer.hasArg("effect_speed"))
        Config::effect_speed = constrain((int)webServer.arg("effect_speed").toInt(), 1, 10);
    if (webServer.hasArg("base_brightness")) {
        Config::base_brightness = (uint8_t)constrain(webServer.arg("base_brightness").toInt(), 0, 255);
        currentBrightness = Config::base_brightness;
    }

    Config::save();
    configDirty = false;

    // Drop any running F1 connection and clear the poll/backoff timers so the
    // new settings are used on the very next loop() instead of up to 5 min later
    if (f1Changed) {
        Serial.println("[F1] settings changed - restarting client");
        f1Shutdown();
    }

    // Apply hardware changes immediately
    ws2812b.updateLength(totalPixels());
    ws2812b.updateType((neoPixelType)(Config::pixel_color_order |
                       (Config::pixel_khz == 400 ? NEO_KHZ400 : NEO_KHZ800)));
    if ((uint8_t)Config::led_pin != ws2812b.getPin()) {
        ws2812b.setPin(Config::led_pin);
        ws2812b.begin(); // re-init RMT only when the pin actually changes
    }

    // Handle MQTT state transitions
    if (Config::mqtt_enabled != wasMqttEnabled) {
        if (!Config::mqtt_enabled && mqttClient.connected()) {
            mqttClient.publish(MQTT_TOPIC_AVAILABILITY, AVAILABILITY_OFFLINE, false);
            mqttClient.disconnect();
        }
        if (Config::mqtt_enabled) {
            mqttClient.setCallback(mqttCallback);
            mqttReconnect();
        }
        blinkConnected();
    }

    updateLEDs();
    publishState();
    webServer.sendHeader("Location", "/");
    webServer.send(302, "text/plain", "");
}

// ---------------------------------------------------------------------------
// handleApiMqttTest - GET /api/mqtt_test - try a real broker login and report back
//
// Uses the values passed as query parameters (i.e. whatever is currently typed
// into the configuration form) so the connection can be tested before saving.
// Anything omitted falls back to the stored configuration.
// ---------------------------------------------------------------------------

void handleApiMqttTest() {
    char server[80];
    char username[24];
    char password[24];
    int  port   = Config::mqtt_port;
    bool secure = Config::mqtt_secure;

    strlcpy(server,   Config::mqtt_server,   sizeof(server));
    strlcpy(username, Config::mqtt_username, sizeof(username));
    strlcpy(password, Config::mqtt_password, sizeof(password));

    if (webServer.hasArg("server")) {
        String s = webServer.arg("server"); s.trim();
        strlcpy(server, s.c_str(), sizeof(server));
    }
    if (webServer.hasArg("port"))
        port = constrain((int)webServer.arg("port").toInt(), 1, 65535);
    if (webServer.hasArg("username")) {
        String u = webServer.arg("username"); u.trim();
        strlcpy(username, u.c_str(), sizeof(username));
    }
    if (webServer.hasArg("password") && webServer.arg("password").length() > 0)
        strlcpy(password, webServer.arg("password").c_str(), sizeof(password));
    if (webServer.hasArg("secure"))
        secure = (webServer.arg("secure") == "1" || webServer.arg("secure") == "true");

    if (server[0] == '\0') {
        webServer.send(200, "application/json",
                       "{\"ok\":false,\"state\":0,\"message\":\"No broker address set\"}");
        return;
    }

    // Separate client objects so an already-running MQTT session is left alone
    WiFiClient       testPlain;
    WiFiClientSecure testSecure;
    PubSubClient     testClient;

    if (secure) {
        testSecure.setInsecure();
        testClient.setClient(testSecure);
    } else {
        testClient.setClient(testPlain);
    }
    testClient.setServer(server, port);
    testClient.setSocketTimeout(5);

    // Distinct client ID - brokers kick the first session on a duplicate ID
    char clientId[32];
    snprintf(clientId, sizeof(clientId), "%s-test", identifier);

    Serial.printf("MQTT test: connecting to %s:%d (%s)...\n",
                  server, port, secure ? "TLS" : "plain");

    const char* user = (username[0] != '\0') ? username : nullptr;
    const char* pass = (password[0] != '\0') ? password : nullptr;

    const bool ok = testClient.connect(clientId, user, pass);
    const int  st = testClient.state();
    testClient.disconnect();

    const char* msg;
    if (ok) {
        msg = "Connected successfully";
    } else {
        switch (st) {
            case -4: msg = "Connection timed out";                       break;
            case -3: msg = "Connection lost";                            break;
            case -2: msg = "Cannot reach broker (check host / port / TLS)"; break;
            case -1: msg = "Disconnected";                               break;
            case  1: msg = "Broker rejected the protocol version";       break;
            case  2: msg = "Broker rejected the client ID";              break;
            case  3: msg = "Broker unavailable";                         break;
            case  4: msg = "Bad username or password";                   break;
            case  5: msg = "Not authorised";                             break;
            default: msg = "Connection failed";                          break;
        }
    }

    Serial.printf("MQTT test: %s (state=%d)\n", msg, st);

    DynamicJsonDocument doc(192);
    doc["ok"]      = ok;
    doc["state"]   = st;
    doc["message"] = msg;

    char payload[192];
    serializeJson(doc, payload);
    webServer.send(200, "application/json", payload);
}

// ---------------------------------------------------------------------------
// handleWebReset - Clear WiFi credentials and reboot into AP mode
// ---------------------------------------------------------------------------

void handleWebReset() {
    webServer.send(200, "text/plain", "Resetting WiFi settings. Device will restart into AP mode.");
    delay(500);
    resetWifiSettingsAndReboot();
}

// ---------------------------------------------------------------------------
// handleApiStateGet - GET /api/state -> JSON state
// ---------------------------------------------------------------------------

void handleApiStateGet() {
    DynamicJsonDocument doc(640);

    doc["state"]       = ledState ? "on" : "off";
    doc["brightness"]  = currentBrightness;

    JsonObject color = doc.createNestedObject("color");
    color["r"] = Config::color_r;
    color["g"] = Config::color_g;
    color["b"] = Config::color_b;

    doc["effect"]       = Config::effect;
    doc["effect_name"]  = effectName(Config::effect);
    doc["effect_speed"] = Config::effect_speed;
    doc["pixels"]       = totalPixels();

    JsonObject f1 = doc.createNestedObject("f1");
    f1["enabled"] = Config::f1_enabled;
    f1["live"]    = f1IsSessionLive();
    f1["feed"]    = f1FeedName(f1Feed);
    f1["track"]   = f1TrackName(f1Track);
    f1["session"] = f1SessionStateName(f1Session);

    doc["ip"]           = WiFi.localIP().toString();
    doc["rssi"]         = WiFi.RSSI();

    char payload[512];
    serializeJson(doc, payload);
    webServer.send(200, "application/json", payload);
}

// ---------------------------------------------------------------------------
// handleApiStatePost - POST /api/state with JSON body -> apply + return state
// ---------------------------------------------------------------------------

void handleApiStatePost() {
    if (!webServer.hasArg("plain") || webServer.arg("plain").length() == 0) {
        webServer.send(400, "application/json", "{\"error\":\"empty body\"}");
        return;
    }

    DynamicJsonDocument doc(640);
    if (deserializeJson(doc, webServer.arg("plain")) != DeserializationError::Ok) {
        webServer.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }

    bool changed = false;

    if (doc.containsKey("state")) {
        const char* s = doc["state"].as<const char*>();
        if (s) {
            if (strcasecmp(s, "toggle") == 0) ledState = !ledState;
            else                              ledState = (strcasecmp(s, "on") == 0);
        }
    }
    if (doc.containsKey("brightness")) {
        currentBrightness       = (uint8_t)constrain((int)doc["brightness"], 0, 255);
        Config::base_brightness = currentBrightness;
        changed = true;
    }
    if (doc.containsKey("color")) {
        JsonObject c = doc["color"];
        if (c.containsKey("r")) Config::color_r = (uint8_t)constrain((int)c["r"], 0, 255);
        if (c.containsKey("g")) Config::color_g = (uint8_t)constrain((int)c["g"], 0, 255);
        if (c.containsKey("b")) Config::color_b = (uint8_t)constrain((int)c["b"], 0, 255);
        changed = true;
    }
    if (doc.containsKey("effect")) {
        int e = -1;
        if (doc["effect"].is<const char*>()) e = effectIndexFromName(doc["effect"].as<const char*>());
        else                                 e = doc["effect"].as<int>();
        if (e >= 0 && e < EFFECT_COUNT) {
            Config::effect = e;
            changed = true;
        }
    }
    if (doc.containsKey("effect_speed")) {
        Config::effect_speed = constrain((int)doc["effect_speed"], 1, 10);
        changed = true;
    }
    if (doc.containsKey("f1_enabled")) {
        Config::f1_enabled = doc["f1_enabled"].as<bool>();   // applied by f1Poll() next loop()
        changed = true;
    }

    if (changed) markConfigDirty();

    updateLEDs();
    publishState();
    handleApiStateGet(); // respond with updated state
}

// ---------------------------------------------------------------------------
// handleApiBrightness - GET /api/brightness?v=<0-255> - live preview while dragging
// (not persisted; use /set?brightness= or POST /api/state to save)
// ---------------------------------------------------------------------------

void handleApiBrightness() {
    if (webServer.hasArg("v")) {
        currentBrightness = (uint8_t)constrain(webServer.arg("v").toInt(), 0, 255);
        updateLEDs();
    }
    webServer.send(200, "text/plain", "");
}

// ---------------------------------------------------------------------------
// handleApiColor - POST /api/color {"r":255,"g":0,"b":0} - fill strips with a
// solid colour immediately (temporary; used to verify the pixel colour order)
// ---------------------------------------------------------------------------

void handleApiColor() {
    if (!webServer.hasArg("plain") || webServer.arg("plain").length() == 0) {
        webServer.send(400, "application/json", "{\"error\":\"empty body\"}");
        return;
    }

    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, webServer.arg("plain")) != DeserializationError::Ok) {
        webServer.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }

    const uint8_t r = (uint8_t)constrain((int)doc["r"], 0, 255);
    const uint8_t g = (uint8_t)constrain((int)doc["g"], 0, 255);
    const uint8_t b = (uint8_t)constrain((int)doc["b"], 0, 255);

    ws2812b.setBrightness(currentBrightness);
    ws2812b.clear();
    ws2812b.fill(ws2812b.Color(r, g, b), 0, totalPixels());
    ws2812b.show();

    char payload[64];
    snprintf(payload, sizeof(payload), "{\"r\":%d,\"g\":%d,\"b\":%d}", r, g, b);
    webServer.send(200, "application/json", payload);
}

// ---------------------------------------------------------------------------
// handleWebNotFound - 404 for unregistered routes
// ---------------------------------------------------------------------------

void handleWebNotFound() {
    webServer.send(404, "text/plain", "Not found: " + webServer.uri());
}
