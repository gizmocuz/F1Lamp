// MQTTAutoDiscovery.ino - MQTT state publishing and Home Assistant auto-discovery

const char* effectName(int idx) {
    if (idx < 0 || idx >= EFFECT_COUNT) return EFFECT_NAMES[EFFECT_SOLID];
    return EFFECT_NAMES[idx];
}

int effectIndexFromName(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < EFFECT_COUNT; i++) {
        if (strcasecmp(name, EFFECT_NAMES[i]) == 0) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------

void publishState() {
    if (!Config::mqtt_enabled || !mqttClient.connected()) return;
    DynamicJsonDocument doc(256);
    char payload[256];

    doc["state"]      = ledState ? "ON" : "OFF";
    doc["brightness"] = currentBrightness;
    doc["color_mode"] = "rgb";

    JsonObject color = doc.createNestedObject("color");
    color["r"] = Config::color_r;
    color["g"] = Config::color_g;
    color["b"] = Config::color_b;

    doc["effect"] = effectName(Config::effect);

    serializeJson(doc, payload);
    mqttClient.publish(MQTT_TOPIC_STATE, payload, false);
}

// ---------------------------------------------------------------------------

void publishAutoConfig() {
    if (!Config::mqtt_enabled) return;
    char mqttPayload[1536];   // headroom: serializeJson() truncates silently if this is too small
    DynamicJsonDocument autoconfPayload(1536);
    DynamicJsonDocument device(256);
    StaticJsonDocument<64> identifiersDoc;
    JsonArray identifiers = identifiersDoc.to<JsonArray>();

    identifiers.add(identifier);

    device["identifiers"]  = identifiers;
    device["manufacturer"] = "PA1DVB";
    device["model"]        = "F1LAMP";
    device["name"]         = identifier;
    device["sw_version"]   = app_version;

    autoconfPayload["name"]                 = String(identifier);
    autoconfPayload["unique_id"]            = String(identifier) + "_light";
    autoconfPayload["schema"]               = "json";
    autoconfPayload["device"]               = device.as<JsonObject>();
    autoconfPayload["availability_topic"]   = MQTT_TOPIC_AVAILABILITY;
    autoconfPayload["state_topic"]          = MQTT_TOPIC_STATE;
    autoconfPayload["command_topic"]        = MQTT_TOPIC_COMMAND;
    autoconfPayload["brightness"]           = true;
    autoconfPayload["brightness_scale"]     = 255;
    autoconfPayload["supported_color_modes"][0] = "rgb";
    autoconfPayload["effect"]               = true;
    for (int i = 0; i < EFFECT_COUNT; i++) {
        autoconfPayload["effect_list"][i] = EFFECT_NAMES[i];
    }
    autoconfPayload["icon"]                 = "mdi:flag-checkered";

    serializeJson(autoconfPayload, mqttPayload);
    mqttClient.publish(MQTT_TOPIC_AUTOCONF_LIGHT, mqttPayload, true);

    autoconfPayload.clear();

    // --- Payload 2: Effect select entity ---
    // The light entity above already carries effect_list, but that is only
    // reachable from the light's more-info dialog. A standalone select is far
    // easier to use from dashboards, scripts and Domoticz.
    autoconfPayload["name"]               = String(identifier) + " Effect";
    autoconfPayload["unique_id"]          = String(identifier) + "_effect";
    autoconfPayload["device"]             = device.as<JsonObject>();
    autoconfPayload["availability_topic"] = MQTT_TOPIC_AVAILABILITY;
    autoconfPayload["state_topic"]        = MQTT_TOPIC_STATE;
    autoconfPayload["value_template"]     = "{{ value_json.effect }}";
    autoconfPayload["command_topic"]      = MQTT_TOPIC_EFFECT_SET;
    for (int i = 0; i < EFFECT_COUNT; i++) {
        autoconfPayload["options"][i] = EFFECT_NAMES[i];
    }
    autoconfPayload["icon"]               = "mdi:animation-play";

    serializeJson(autoconfPayload, mqttPayload);
    mqttClient.publish(MQTT_TOPIC_AUTOCONF_EFFECT, mqttPayload, true);
}

// ---------------------------------------------------------------------------

void mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
    // Plain-text effect name from the select entity
    if (strcmp(topic, MQTT_TOPIC_EFFECT_SET) == 0) {
        char msg[24] = {0};
        const unsigned int copyLen = min((unsigned int)(sizeof(msg) - 1), length);
        memcpy(msg, payload, copyLen);
        msg[copyLen] = '\0';

        const int e = effectIndexFromName(msg);
        if (e >= 0) {
            Config::effect = e;
            markConfigDirty();
            updateLEDs();
        } else {
            Serial.printf("MQTT: unknown effect '%s'\n", msg);
        }
        publishState();
        return;
    }

    if (strcmp(topic, MQTT_TOPIC_COMMAND) != 0) return;

    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, payload, length) != DeserializationError::Ok) {
        Serial.println("MQTT: failed to parse command JSON");
        return;
    }

    if (doc.containsKey("state")) {
        const char* stateStr = doc["state"].as<const char*>();
        if (stateStr != nullptr) ledState = (strcasecmp(stateStr, "ON") == 0);
    }
    if (doc.containsKey("brightness")) {
        currentBrightness       = (uint8_t)constrain((int)doc["brightness"], 0, 255);
        Config::base_brightness = currentBrightness;
        markConfigDirty();
    }
    if (doc.containsKey("color")) {
        JsonObject c = doc["color"];
        if (c.containsKey("r")) Config::color_r = (uint8_t)constrain((int)c["r"], 0, 255);
        if (c.containsKey("g")) Config::color_g = (uint8_t)constrain((int)c["g"], 0, 255);
        if (c.containsKey("b")) Config::color_b = (uint8_t)constrain((int)c["b"], 0, 255);
        markConfigDirty();
    }
    if (doc.containsKey("effect")) {
        const int e = effectIndexFromName(doc["effect"].as<const char*>());
        if (e >= 0) {
            Config::effect = e;
            markConfigDirty();
        }
    }

    updateLEDs();
    publishState();
}
