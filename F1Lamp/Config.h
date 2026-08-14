#pragma once

#include <ArduinoJson.h>
#include <FS.h>

namespace Config {
    char mqtt_server[80]    = "example.tld";
    char mqtt_username[24]  = "";
    char mqtt_password[24]  = "";
    char mqtt_discovery_prefix[32] = "homeassistant";
    bool mqtt_enabled       = false;
    bool mqtt_secure        = false;
    int  mqtt_port          = 1883;

    // --- LED strips ---
    // Three short strips chained on one data line (4 + 4 + 3 = 11 dots by default)
    int  strip1_pixels      = 4;
    int  strip2_pixels      = 4;
    int  strip3_pixels      = 3;

    int  led_pin            = 4;    // data pin (matches PIN_WS2812B compile-time default)
    uint8_t pixel_color_order = 82; // NEO_GRB=82, NEO_RGB=6, NEO_RBG=9, NEO_BRG=88, NEO_BGR=54, NEO_GBR=98
    int  pixel_khz          = 800;  // 800 (WS2812B) or 400 (WS2811)

    // --- Appearance ---
    uint8_t color_r         = 255;  // F1 red
    uint8_t color_g         = 0;
    uint8_t color_b         = 0;
    uint8_t base_brightness = 128;
    int     effect          = 0;    // index into EFFECT_NAMES
    int     effect_speed    = 5;    // 1 (slow) .. 10 (fast)
    bool    power_on_boot   = true; // lamp turns on by itself after a power cycle

    // --- Formula 1 live tracking ---
    // When enabled the lamp mirrors the official F1 track status and switches
    // itself off whenever no session is running.
    bool    f1_enabled      = false;
    // Point these at the bundled simulator (tools/f1_sim.py) to test without
    // waiting for a race weekend. Defaults are the real F1 timing service.
    char    f1_host[64]     = "livetiming.formula1.com";
    int     f1_port         = 443;
    bool    f1_tls          = true;

    void save() {
        DynamicJsonDocument json(1024);
        json["mqtt_server"]    = mqtt_server;
        json["mqtt_username"]  = mqtt_username;
        json["mqtt_password"]  = mqtt_password;
        json["mqtt_discovery_prefix"] = mqtt_discovery_prefix;
        json["mqtt_enabled"]   = mqtt_enabled;
        json["mqtt_secure"]    = mqtt_secure;
        json["mqtt_port"]      = mqtt_port;

        json["strip1_pixels"]  = strip1_pixels;
        json["strip2_pixels"]  = strip2_pixels;
        json["strip3_pixels"]  = strip3_pixels;

        json["led_pin"]           = led_pin;
        json["pixel_color_order"] = pixel_color_order;
        json["pixel_khz"]         = pixel_khz;

        json["color_r"]         = color_r;
        json["color_g"]         = color_g;
        json["color_b"]         = color_b;
        json["base_brightness"] = base_brightness;
        json["effect"]          = effect;
        json["effect_speed"]    = effect_speed;
        json["power_on_boot"]   = power_on_boot;
        json["f1_enabled"]      = f1_enabled;
        json["f1_host"]         = f1_host;
        json["f1_port"]         = f1_port;
        json["f1_tls"]          = f1_tls;

        File configFile = SPIFFS.open("/config.json", "w");
        if (!configFile) {
            return;
        }

        serializeJson(json, configFile);
        configFile.close();
    }

    void load() {
        // SPIFFS is mounted by setup() before load() is called
        if (SPIFFS.exists("/config.json")) {
            File configFile = SPIFFS.open("/config.json", "r");

            if (configFile) {
                const size_t size = configFile.size();
                std::unique_ptr<char[]> buf(new char[size + 1]);

                configFile.readBytes(buf.get(), size);
                buf.get()[size] = '\0';
                DynamicJsonDocument json(1024);

                if (DeserializationError::Ok == deserializeJson(json, buf.get())) {
                    strlcpy(mqtt_server,   json["mqtt_server"]   | "", sizeof(mqtt_server));
                    strlcpy(mqtt_username, json["mqtt_username"] | "", sizeof(mqtt_username));
                    strlcpy(mqtt_password, json["mqtt_password"] | "", sizeof(mqtt_password));
                    strlcpy(mqtt_discovery_prefix, json["mqtt_discovery_prefix"] | "homeassistant", sizeof(mqtt_discovery_prefix));
                    mqtt_enabled    = json["mqtt_enabled"] | false;
                    mqtt_secure     = json["mqtt_secure"]  | false;
                    mqtt_port       = json["mqtt_port"]    | 1883;

                    strip1_pixels   = json["strip1_pixels"] | 4;
                    strip2_pixels   = json["strip2_pixels"] | 4;
                    strip3_pixels   = json["strip3_pixels"] | 3;

                    led_pin           = json["led_pin"] | 4;
                    pixel_color_order = (uint8_t)(json["pixel_color_order"] | 82);
                    pixel_khz         = json["pixel_khz"] | 800;

                    color_r         = (uint8_t)(json["color_r"] | 255);
                    color_g         = (uint8_t)(json["color_g"] | 0);
                    color_b         = (uint8_t)(json["color_b"] | 0);
                    base_brightness = (uint8_t)(json["base_brightness"] | 128);
                    effect          = json["effect"]        | 0;
                    effect_speed    = json["effect_speed"]  | 5;
                    power_on_boot   = json["power_on_boot"] | true;
                    f1_enabled      = json["f1_enabled"]    | false;
                    strlcpy(f1_host, json["f1_host"] | "livetiming.formula1.com", sizeof(f1_host));
                    f1_port         = json["f1_port"]       | 443;
                    f1_tls          = json["f1_tls"]        | true;
                }
            }
        }
    }
} // namespace Config
