// LEDControl.ino - LED strip control logic and effects for F1Lamp
//
// The three strips are chained on a single data line, so pixel 0 of strip 2
// is at index strip1_pixels, and pixel 0 of strip 3 is at strip1+strip2.

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

int totalPixels() {
    const int t = Config::strip1_pixels + Config::strip2_pixels + Config::strip3_pixels;
    return (t < 1) ? 1 : t;
}

int stripOffset(int idx) {
    if (idx <= 0) return 0;
    if (idx == 1) return Config::strip1_pixels;
    return Config::strip1_pixels + Config::strip2_pixels;
}

int stripLength(int idx) {
    if (idx <= 0) return Config::strip1_pixels;
    if (idx == 1) return Config::strip2_pixels;
    return Config::strip3_pixels;
}

// While the F1 feed drives the lamp these return the F1 override instead of the
// user's saved settings, so Config is never touched and nothing extra is
// written to flash. Clearing f1Override restores the user's look instantly.
uint32_t currentColor() {
    if (f1Override) return ws2812b.Color(f1R, f1G, f1B);
    return ws2812b.Color(Config::color_r, Config::color_g, Config::color_b);
}

int currentEffect() {
    return f1Override ? f1Effect : Config::effect;
}

int currentSpeed() {
    return f1Override ? f1Speed : Config::effect_speed;
}

// The logo has two glyphs: strips 1 + 2 together form the "F", strip 3 forms the "1".
int glyphFLength() {
    return Config::strip1_pixels + Config::strip2_pixels;
}

// Light the "F" (strips 1 + 2) or the "1" (strip 3). Assumes the caller has
// already set the brightness and cleared the buffer.
void fillGlyphF() {
    const int end = glyphFLength();
    for (int i = 0; i < end; i++) ws2812b.setPixelColor(i, currentColor());
}

void fillGlyph1() {
    const int start = glyphFLength();
    const int end   = start + Config::strip3_pixels;
    for (int i = start; i < end; i++) ws2812b.setPixelColor(i, currentColor());
}

// ---------------------------------------------------------------------------
// updateLEDs - render the "base" frame and restart the running effect
// ---------------------------------------------------------------------------

void updateLEDs() {
    effectReset = true;
    ws2812b.setBrightness(currentBrightness);
    ws2812b.clear();

    if (!ledState) {
        ws2812b.show();
        return;
    }

    // Animated effects draw their first frame on the next handleEffect() call;
    // only Solid has a static frame to paint here.
    if (currentEffect() == EFFECT_SOLID) {
        ws2812b.fill(currentColor(), 0, totalPixels());
    }
    ws2812b.show();
}

// ---------------------------------------------------------------------------
// handleEffect - non-blocking effect renderer, called every loop()
// ---------------------------------------------------------------------------

void handleEffect() {
    static uint32_t lastStep = 0;
    static uint16_t phase    = 0;

    if (effectReset) {
        phase       = 0;
        lastStep    = 0;
        effectReset = false;
    }

    // Static frames (Solid, or the strip switched off) used to be written once
    // and never again, so a frame corrupted on the wire stayed corrupted until
    // something else redrew - the classic "one LED stays lit after Off".
    // Re-send once a second so a glitch heals itself. ~330 us/s of RMT time and
    // no extra LED current, since the pixels are already lit.
    if (!ledState || currentEffect() == EFFECT_SOLID) {
        static uint32_t lastRefresh = 0;
        const uint32_t nowMs = millis();
        if (nowMs - lastRefresh >= 1000) {
            lastRefresh = nowMs;
            ws2812b.setBrightness(currentBrightness);
            ws2812b.clear();
            if (ledState) ws2812b.fill(currentColor(), 0, totalPixels());
            ws2812b.show();
        }
        return;
    }

    const int n   = totalPixels();
    const int spd = constrain(currentSpeed(), 1, 10);

    uint32_t interval;
    switch (currentEffect()) {
        case EFFECT_BREATHE:
        case EFFECT_RAINBOW:     interval = 25; break;
        case EFFECT_CHASE:       interval = map(spd, 1, 10,  300,  30); break;
        case EFFECT_BLINK:       interval = map(spd, 1, 10, 1500, 100); break;
        case EFFECT_RACE_START:  interval = map(spd, 1, 10, 1200, 120); break;
        case EFFECT_STRIP_CYCLE: interval = map(spd, 1, 10, 1500, 120); break;
        default:                 interval = 100; break;
    }

    const uint32_t now = millis();
    if (lastStep != 0 && now - lastStep < interval) return;
    lastStep = now;

    switch (currentEffect()) {

        case EFFECT_BREATHE: {
            // Triangle wave over 512 steps: 0 -> 255 -> 0
            const uint16_t p   = phase % 512;
            const uint8_t  lvl = (p < 256) ? (uint8_t)p : (uint8_t)(511 - p);
            ws2812b.setBrightness((uint8_t)((uint16_t)lvl * currentBrightness / 255));
            ws2812b.fill(currentColor(), 0, n);
            ws2812b.show();
            phase += spd * 2;
            break;
        }

        case EFFECT_BLINK: {
            // Alternate between the two glyphs of the logo: "F" then "1"
            ws2812b.setBrightness(currentBrightness);
            ws2812b.clear();
            if ((phase % 2) == 0) fillGlyphF();
            else                  fillGlyph1();
            ws2812b.show();
            phase++;
            break;
        }

        case EFFECT_CHASE: {
            ws2812b.setBrightness(currentBrightness);
            ws2812b.clear();
            ws2812b.setPixelColor(phase % n, currentColor());
            ws2812b.show();
            phase++;
            break;
        }

        case EFFECT_RACE_START: {
            // F1 start procedure: lights come on one by one, hold, then "lights out".
            // n steps to fill + 1 hold step + 3 dark steps.
            const uint16_t total = n + 4;
            const uint16_t p     = phase % total;
            ws2812b.setBrightness(currentBrightness);
            ws2812b.clear();
            if (p < (uint16_t)n) {
                for (uint16_t i = 0; i <= p; i++) ws2812b.setPixelColor(i, currentColor());
            } else if (p == (uint16_t)n) {
                ws2812b.fill(currentColor(), 0, n);   // all lights on - hold
            }                                          // p > n -> lights out
            ws2812b.show();
            phase++;
            break;
        }

        case EFFECT_STRIP_CYCLE: {
            const int s = phase % 3;
            ws2812b.setBrightness(currentBrightness);
            ws2812b.clear();
            const int off = stripOffset(s);
            const int len = stripLength(s);
            for (int i = 0; i < len; i++) ws2812b.setPixelColor(off + i, currentColor());
            ws2812b.show();
            phase++;
            break;
        }

        case EFFECT_RAINBOW: {
            ws2812b.setBrightness(currentBrightness);
            for (int i = 0; i < n; i++) {
                const uint16_t hue = (uint16_t)(((uint32_t)i * 65536UL) / n + (uint32_t)phase * 256UL);
                ws2812b.setPixelColor(i, ws2812b.gamma32(ws2812b.ColorHSV(hue)));
            }
            ws2812b.show();
            phase += spd;
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Connection status animations
// ---------------------------------------------------------------------------

// Two green blinks - WiFi connected / config applied
void blinkConnected() {
    const int n = totalPixels();
    for (int b = 0; b < 2; b++) {
        ws2812b.setBrightness(currentBrightness);
        ws2812b.clear();
        ws2812b.fill(ws2812b.Color(0, 255, 0), 0, n);
        ws2812b.show();
        delay(200);
        ws2812b.clear();
        ws2812b.show();
        delay(200);
    }
}

// Slow blue breathing - captive portal (AP mode) is open
void showAPModeAnimation() {
    static uint32_t apAnimLast = 0;
    const uint32_t now = millis();
    if (now - apAnimLast < 20) return;
    apAnimLast = now;

    const uint32_t t = now % 2000;
    const uint8_t  lvl = (t < 1000) ? (uint8_t)(t * 255 / 1000)
                                    : (uint8_t)((2000 - t) * 255 / 1000);
    ws2812b.setBrightness((uint8_t)((uint16_t)lvl * currentBrightness / 255));
    ws2812b.clear();
    ws2812b.fill(ws2812b.Color(0, 0, 255), 0, totalPixels());
    ws2812b.show();
}

// Blue blink - trying to connect to WiFi or MQTT
void showConnectingAnimation() {
    static uint32_t connAnimLast = 0;
    static bool     connOn       = false;
    const uint32_t now = millis();
    if (now - connAnimLast < 300) return;
    connAnimLast = now;

    connOn = !connOn;
    ws2812b.setBrightness(currentBrightness);
    ws2812b.clear();
    if (connOn) ws2812b.fill(ws2812b.Color(0, 0, 255), 0, totalPixels());
    ws2812b.show();
}
