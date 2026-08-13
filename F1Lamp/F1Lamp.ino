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

// --- Device identifier (built from MAC address in setup) ---
char identifier[24];

// --- MQTT topic buffers ---
char MQTT_TOPIC_AVAILABILITY[128];
char MQTT_TOPIC_STATE[128];
char MQTT_TOPIC_COMMAND[128];
char MQTT_TOPIC_EFFECT_SET[128];
char MQTT_TOPIC_AUTOCONF_LIGHT[128];
char MQTT_TOPIC_AUTOCONF_EFFECT[128];

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
