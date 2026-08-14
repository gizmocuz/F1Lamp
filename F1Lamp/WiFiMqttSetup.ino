// WiFiMqttSetup.ino - captive portal WiFi setup, OTA and MQTT connection

void saveConfigCallback() {
    shouldSaveConfig = true;
}

// ---------------------------------------------------------------------------

void setupWifi() {
    wifiManager.setDebugOutput(false);
    wifiManager.setSaveConfigCallback(saveConfigCallback);
    wifiManager.setConfigPortalBlocking(false);
    wifiManager.setConnectTimeout(30);       // retry saved WiFi for 30 s before opening portal
    wifiManager.setConfigPortalTimeout(180); // reboot automatically if portal unused for 3 min

    WiFi.setHostname(identifier);

    Serial.println("WiFi: connecting...");

    // Try the stored credentials ourselves first so the lamp can blink blue while
    // it waits - WiFiManager::autoConnect() blocks and would freeze the animation.
    if (WiFi.SSID().length() > 0) {
        WiFi.begin();
        const uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
            showConnectingAnimation();
            delay(20);
        }
    }

    // Not connected yet -> let WiFiManager retry and, if that fails, open the portal
    if (WiFi.status() != WL_CONNECTED && !wifiManager.autoConnect(identifier)) {
        // Saved credentials failed - portal is now open
        Serial.println("WiFi: AP portal open, waiting for credentials...");
        while (WiFi.status() != WL_CONNECTED) {
            wifiManager.process();
            showAPModeAnimation();
            delay(20);
        }
    }

    // WiFi is connected at this point
    Serial.printf("WiFi: connected.  IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    blinkConnected();
    if (Config::mqtt_enabled) {
        mqttClient.setCallback(mqttCallback);
    }

    webServer.on("/",       handleWebRoot);
    webServer.on("/set",    handleWebSet);
    webServer.on("/config", HTTP_GET,  handleWebConfig);
    webServer.on("/config", HTTP_POST, handleWebConfigSave);
    webServer.on("/reset",  handleWebReset);
    webServer.on("/api/state",      HTTP_GET,  handleApiStateGet);
    webServer.on("/api/state",      HTTP_POST, handleApiStatePost);
    webServer.on("/api/brightness", HTTP_GET,  handleApiBrightness);
    webServer.on("/api/color",      HTTP_POST, handleApiColor);
    webServer.on("/api/mqtt_test",  HTTP_GET,  handleApiMqttTest);
    webServer.on("/api/f1_test",    HTTP_GET,  handleApiF1Test);
    webServer.onNotFound(handleWebNotFound);
    webServer.begin();

    if (shouldSaveConfig) {
        Config::save();
    }
}

// ---------------------------------------------------------------------------

void setupOTA() {
    ArduinoOTA.onStart([]() {
        Serial.println("OTA: start");
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("\nOTA: end");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("OTA progress: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA error[%u]: ", error);
        if      (error == OTA_AUTH_ERROR)    Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR)     Serial.println("End Failed");
    });

    ArduinoOTA.setHostname(identifier);
    ArduinoOTA.setPassword(identifier);
    ArduinoOTA.begin();
}

// ---------------------------------------------------------------------------

void mqttReconnect() {
    if (!Config::mqtt_enabled) return;
    if (Config::mqtt_secure) {
        wifiClientSecure.setInsecure();
        mqttClient.setClient(wifiClientSecure);
    } else {
        mqttClient.setClient(wifiClient);
    }
    mqttClient.setServer(Config::mqtt_server, Config::mqtt_port);
    Serial.printf("MQTT: connecting to %s:%d (%s)...\n",
                  Config::mqtt_server, Config::mqtt_port,
                  Config::mqtt_secure ? "TLS" : "plain");
    // Pass NULL rather than "" for empty credentials - an empty username flag is
    // rejected by some brokers that otherwise allow anonymous access. This must
    // match handleApiMqttTest() or the Test button would report a false result.
    const char* user = (Config::mqtt_username[0] != '\0') ? Config::mqtt_username : nullptr;
    const char* pass = (Config::mqtt_password[0] != '\0') ? Config::mqtt_password : nullptr;

    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        if (mqttClient.connect(identifier, user, pass,
                               MQTT_TOPIC_AVAILABILITY, 1, false, AVAILABILITY_OFFLINE)) {
            Serial.println("MQTT: connected.");
            mqttClient.publish(MQTT_TOPIC_AVAILABILITY, AVAILABILITY_ONLINE, false);
            publishAutoConfig();
            publishState();
            mqttClient.subscribe(MQTT_TOPIC_COMMAND);
            mqttClient.subscribe(MQTT_TOPIC_EFFECT_SET);
            mqttClient.subscribe(MQTT_TOPIC_F1_SET);
            updateLEDs(); // clear connecting animation from the strips
            return;
        }

        Serial.printf("MQTT: attempt %d failed (state=%d), retrying in 5s...\n",
                      attempt + 1, mqttClient.state());
        const uint32_t retryStart = millis();
        while (millis() - retryStart < 5000) {
            showConnectingAnimation();
            yield();
            delay(20);
        }
    }
    Serial.println("MQTT: all attempts failed, will retry in 60s.");
    updateLEDs(); // clear connecting animation from the strips
}

// ---------------------------------------------------------------------------

void resetWifiSettingsAndReboot() {
    wifiManager.resetSettings();
    delay(3000);
    ESP.restart();
}
