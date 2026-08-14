// ============================================================
// F1Lamp - ESP32-C3 Super Mini
// Firmware for the "F1 WLED Lightbox" 3D print
// https://makerworld.com/en/models/2068365-f1-wled-lightbox
//
// Three short LED strips (11 dots total) chained on one data pin.
// Main sketch file: includes, defines, globals, setup, loop
//
// (c) PA1DVB
// ============================================================

#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include "Config.h"

// --- Hardware bootstrap defaults (overridden at runtime by Config::led_pin / strip sizes) ---
#define PIN_WS2812B   4
#define NUM_PIXELS    11   // 4 + 4 + 3

// --- Firmware identity ---
#define FIRMWARE_PREFIX      "esp32-f1lamp"
#define app_version          "2026.08.13 rev 1.0"
#define AVAILABILITY_ONLINE  "online"
#define AVAILABILITY_OFFLINE "offline"

// --- Effects ---
enum Effect {
    EFFECT_SOLID = 0,
    EFFECT_BREATHE,
    EFFECT_BLINK,
    EFFECT_CHASE,
    EFFECT_RACE_START,
    EFFECT_STRIP_CYCLE,
    EFFECT_RAINBOW,
    EFFECT_COUNT
};

const char* const EFFECT_NAMES[EFFECT_COUNT] = {
    "Solid",
    "Breathe",
    "Blink",
    "Chase",
    "Race Start",
    "Strip Cycle",
    "Rainbow"
};

// --- Formula 1 live tracking ---
// Feed lifecycle. STALE means we are still connected but have heard nothing for
// F1_STALE_MS - the lamp must NOT keep claiming a safety car in that case.
enum F1Feed {
    FEED_DISABLED = 0,   // f1_enabled is off
    FEED_IDLE,           // enabled, nothing live - polling StreamingStatus.json
    FEED_CONNECTING,     // negotiate / subscribe in progress
    FEED_LIVE,           // subscribed and receiving
    FEED_STALE           // connected but no data or ping for too long
};

// Official TrackStatus codes. 1/2/6/7 confirmed against archived 2026 sessions,
// 4/5 from the FastF1 mapping. Code 3 is never emitted - treat as unknown.
enum F1Track {
    TRK_UNKNOWN = 0,
    TRK_CLEAR,           // "1" AllClear
    TRK_YELLOW,          // "2" Yellow
    TRK_SC,              // "4" SCDeployed
    TRK_RED,             // "5" Red
    TRK_VSC,             // "6" VSCDeployed
    TRK_VSC_ENDING       // "7" VSCEnding
};

enum F1Session {
    SES_UNKNOWN = 0,
    SES_INACTIVE,
    SES_STARTED,
    SES_ABORTED,         // red-flag suspension
    SES_FINISHED,
    SES_FINALISED
};

F1Feed    f1Feed    = FEED_DISABLED;
F1Track   f1Track   = TRK_UNKNOWN;
F1Session f1Session = SES_UNKNOWN;
int8_t    f1IsLive  = -1;      // -1 = not yet determined, forces the first transition
char      f1SessionName[40] = "";

// While the F1 feed is driving the lamp these override the user's colour and
// effect WITHOUT touching Config, so the user's settings survive untouched and
// nothing extra is written to flash.
bool     f1Override = false;
uint8_t  f1R = 0, f1G = 255, f1B = 0;
int      f1Effect   = 0;
int      f1Speed    = 5;

// --- Device identifier (built from MAC address in setup) ---
char identifier[24];

// --- MQTT topic buffers ---
char MQTT_TOPIC_AVAILABILITY[128];
char MQTT_TOPIC_STATE[128];
char MQTT_TOPIC_COMMAND[128];
char MQTT_TOPIC_EFFECT_SET[128];
char MQTT_TOPIC_F1_SET[128];
char MQTT_TOPIC_AUTOCONF_LIGHT[128];
char MQTT_TOPIC_AUTOCONF_EFFECT[128];
char MQTT_TOPIC_AUTOCONF_F1SW[128];
char MQTT_TOPIC_AUTOCONF_F1SENS[128];

// --- Hardware objects ---
Adafruit_NeoPixel ws2812b(NUM_PIXELS, PIN_WS2812B, NEO_GRB + NEO_KHZ800);
WiFiClient        wifiClient;
WiFiClientSecure  wifiClientSecure;
PubSubClient      mqttClient;
WebServer         webServer(80);
WiFiManager       wifiManager;

// --- State ---
bool     ledState                  = true;
uint8_t  currentBrightness         = 128;
bool     effectReset               = true;  // set by updateLEDs() to restart the running effect
bool     shouldSaveConfig          = false;
uint32_t lastMqttConnectionAttempt = 0;

// Deferred config save - MQTT/REST clients may change colour or brightness rapidly
// (fades, automations); collect the changes and write SPIFFS at most once per 10 s.
bool     configDirty               = false;
uint32_t lastConfigChange          = 0;

void markConfigDirty() {
    configDirty      = true;
    lastConfigChange = millis();
}

// ============================================================
// setup
// ============================================================
void setup() {
    Serial.begin(115200);

    // Load config before LED init so the strip starts on the correct pin/length
    if (!SPIFFS.begin(false)) {
        Serial.println("SPIFFS mount failed - formatting (config will reset to defaults)");
        SPIFFS.format();
        SPIFFS.begin(false);
    }
    Config::load();
    currentBrightness = Config::base_brightness;
    ledState          = Config::power_on_boot;

    // Single begin() with the correct pin, length and type already set
    ws2812b.updateLength(totalPixels());
    ws2812b.setPin(Config::led_pin);
    ws2812b.updateType((neoPixelType)(Config::pixel_color_order |
                       (Config::pixel_khz == 400 ? NEO_KHZ400 : NEO_KHZ800)));
    ws2812b.begin();
    ws2812b.clear();
    ws2812b.show();

    // Build device identifier from MAC - WiFi.mode() required before macAddress() on ESP32
    WiFi.mode(WIFI_STA);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(identifier, sizeof(identifier), "F1LAMP-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    Serial.printf("App version:  %s\n", app_version);
    Serial.printf("LED pin:      GPIO%d,  pixels: %d (%d+%d+%d)\n",
                  Config::led_pin, totalPixels(),
                  Config::strip1_pixels, Config::strip2_pixels, Config::strip3_pixels);
    Serial.printf("CPU freq:     %u MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Device ID:    %s\n", identifier);

    // Build all MQTT topic strings
    snprintf(MQTT_TOPIC_AVAILABILITY,   sizeof(MQTT_TOPIC_AVAILABILITY)   - 1, "%s/%s/status",  FIRMWARE_PREFIX, identifier);
    snprintf(MQTT_TOPIC_STATE,          sizeof(MQTT_TOPIC_STATE)          - 1, "%s/%s/state",   FIRMWARE_PREFIX, identifier);
    snprintf(MQTT_TOPIC_COMMAND,        sizeof(MQTT_TOPIC_COMMAND)        - 1, "%s/%s/command", FIRMWARE_PREFIX, identifier);
    snprintf(MQTT_TOPIC_EFFECT_SET,     sizeof(MQTT_TOPIC_EFFECT_SET)     - 1, "%s/%s/effect/set", FIRMWARE_PREFIX, identifier);
    snprintf(MQTT_TOPIC_AUTOCONF_LIGHT, sizeof(MQTT_TOPIC_AUTOCONF_LIGHT) - 1, "%s/light/%s/%s/config",
             Config::mqtt_discovery_prefix, FIRMWARE_PREFIX, identifier);
    snprintf(MQTT_TOPIC_AUTOCONF_EFFECT, sizeof(MQTT_TOPIC_AUTOCONF_EFFECT) - 1, "%s/select/%s_%s_effect/config",
             Config::mqtt_discovery_prefix, FIRMWARE_PREFIX, identifier);
    snprintf(MQTT_TOPIC_F1_SET,          sizeof(MQTT_TOPIC_F1_SET)          - 1, "%s/%s/f1/set", FIRMWARE_PREFIX, identifier);
    snprintf(MQTT_TOPIC_AUTOCONF_F1SW,   sizeof(MQTT_TOPIC_AUTOCONF_F1SW)   - 1, "%s/switch/%s_%s_f1/config",
             Config::mqtt_discovery_prefix, FIRMWARE_PREFIX, identifier);
    snprintf(MQTT_TOPIC_AUTOCONF_F1SENS, sizeof(MQTT_TOPIC_AUTOCONF_F1SENS) - 1, "%s/sensor/%s_%s_f1track/config",
             Config::mqtt_discovery_prefix, FIRMWARE_PREFIX, identifier);

    Serial.printf("MQTT availability: %s\n", MQTT_TOPIC_AVAILABILITY);

    // Connect WiFi (shows AP or connecting animation)
    setupWifi();
    Serial.printf("WiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());

    // Belt-and-suspenders: ensure keep-alive and buffer size are set
    mqttClient.setKeepAlive(10);
    mqttClient.setBufferSize(2048);

    // OTA
    setupOTA();

    // First MQTT connect - shows connecting animation on the strips
    if (Config::mqtt_enabled) mqttReconnect();

    // Show initial LED state
    updateLEDs();

    Serial.println("Setup complete.");
}

// ============================================================
// loop
// ============================================================
void loop() {
    ArduinoOTA.handle();
    mqttClient.loop();
    webServer.handleClient();
    handleEffect();
    f1Poll();

    const uint32_t now = millis();
    if (Config::mqtt_enabled && !mqttClient.connected() && now - lastMqttConnectionAttempt >= 60000) {
        lastMqttConnectionAttempt = now;
        mqttReconnect();
    }

    if (configDirty && now - lastConfigChange >= 10000) {
        configDirty = false;
        Config::save();
    }
}
