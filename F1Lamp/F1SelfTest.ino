// F1SelfTest.ino - offline verification of the F1 feed parser
//
// F1 sessions happen a couple of dozen times a year, so the SC / VSC / red-flag
// code paths are almost never reachable when you actually want to test them.
// This replays real SignalR Core records through the SAME functions the live
// stream uses (f1HandleSseLine -> f1HandleDataLine -> f1HandleRecord), then
// checks the resulting state.
//
// The first two records below were captured verbatim from
// livetiming.formula1.com; the rest use the documented status codes.
//
// Run it with:  curl http://<lamp-ip>/api/f1_test
//
// The lamp visibly cycles green -> yellow -> safety car -> VSC -> red while the
// test runs, then returns to whatever it was doing.

struct F1TestStep {
    const char* line;         // exactly what one SSE line looks like
    F1Track     expTrack;
    F1Session   expSession;
    const char* desc;
};

static const F1TestStep F1_TESTS[] = {

    // --- captured verbatim from the live endpoint ---
    { "data: {}",
      TRK_UNKNOWN, SES_UNKNOWN, "empty frame ignored" },

    { "data: {\"type\":3,\"invocationId\":\"0\",\"result\":{"
      "\"TrackStatus\":{\"Status\":\"1\",\"Message\":\"AllClear\",\"_kf\":true},"
      "\"SessionStatus\":{\"Status\":\"Ends\",\"Started\":\"Finished\",\"_kf\":true}}}",
      TRK_CLEAR, SES_FINISHED, "Subscribe snapshot (real capture)" },

    { "data: {\"type\":6}",
      TRK_CLEAR, SES_FINISHED, "keep-alive ping changes nothing" },

    // --- session lifecycle ---
    { "data: {\"type\":1,\"target\":\"feed\",\"arguments\":[\"SessionStatus\",{\"Status\":\"Started\"},\"ts\"]}",
      TRK_CLEAR, SES_STARTED, "session starts" },

    // --- flags ---
    { "data: {\"type\":1,\"target\":\"feed\",\"arguments\":[\"TrackStatus\",{\"Status\":\"2\",\"Message\":\"Yellow\"},\"ts\"]}",
      TRK_YELLOW, SES_STARTED, "yellow flag" },

    { "data: {\"type\":1,\"target\":\"feed\",\"arguments\":[\"TrackStatus\",{\"Status\":\"4\",\"Message\":\"SCDeployed\"},\"ts\"]}",
      TRK_SC, SES_STARTED, "safety car deployed" },

    { "data: {\"type\":1,\"target\":\"feed\",\"arguments\":[\"TrackStatus\",{\"Status\":\"6\",\"Message\":\"VSCDeployed\"},\"ts\"]}",
      TRK_VSC, SES_STARTED, "virtual safety car" },

    { "data: {\"type\":1,\"target\":\"feed\",\"arguments\":[\"TrackStatus\",{\"Status\":\"7\",\"Message\":\"VSCEnding\"},\"ts\"]}",
      TRK_VSC_ENDING, SES_STARTED, "VSC ending" },

    { "data: {\"type\":1,\"target\":\"feed\",\"arguments\":[\"TrackStatus\",{\"Status\":\"5\",\"Message\":\"Red\"},\"ts\"]}",
      TRK_RED, SES_STARTED, "red flag" },

    // --- fail-safe: an unknown code must HOLD, never fall back to green ---
    { "data: {\"type\":1,\"target\":\"feed\",\"arguments\":[\"TrackStatus\",{\"Status\":\"3\",\"Message\":\"Mystery\"},\"ts\"]}",
      TRK_RED, SES_STARTED, "unknown code 3 holds previous state" },

    { "data: {\"type\":1,\"target\":\"feed\",\"arguments\":[\"TrackStatus\",{\"Status\":\"1\",\"Message\":\"AllClear\"},\"ts\"]}",
      TRK_CLEAR, SES_STARTED, "back to green" },

    // --- two records in one line, separated by 0x1E ---
    { "data: {\"type\":1,\"target\":\"feed\",\"arguments\":[\"TrackStatus\",{\"Status\":\"2\"},\"ts\"]}\036"
             "{\"type\":1,\"target\":\"feed\",\"arguments\":[\"TrackStatus\",{\"Status\":\"4\"},\"ts\"]}",
      TRK_SC, SES_STARTED, "two 0x1E-separated records in one line" },

    // --- robustness ---
    { "data: {\"type\":1,\"target\":\"feed\",\"arguments\":[\"TrackStatus\",{\"Status\":",
      TRK_SC, SES_STARTED, "truncated JSON ignored, no crash" },

    { "data: not json at all",
      TRK_SC, SES_STARTED, "garbage ignored, no crash" },

    { ": comment line",
      TRK_SC, SES_STARTED, "SSE comment ignored" },

    { "data: {\"type\":1,\"target\":\"feed\",\"arguments\":[\"WeatherData\",{\"AirTemp\":\"21\"},\"ts\"]}",
      TRK_SC, SES_STARTED, "unsubscribed topic ignored" },

    // --- session ends ---
    { "data: {\"type\":1,\"target\":\"feed\",\"arguments\":[\"SessionStatus\",{\"Status\":\"Finalised\"},\"ts\"]}",
      TRK_SC, SES_FINALISED, "session finalised" },
};

#define F1_TEST_COUNT (sizeof(F1_TESTS) / sizeof(F1_TESTS[0]))

// ---------------------------------------------------------------------------
// handleApiF1Test - GET /api/f1_test
// ---------------------------------------------------------------------------

void handleApiF1Test() {
    // Save everything the test disturbs
    const F1Feed    savedFeed     = f1Feed;
    const F1Track   savedTrack    = f1Track;
    const F1Session savedSession  = f1Session;
    const int8_t    savedIsLive   = f1IsLive;
    const bool      savedOverride = f1Override;
    const bool      savedLedState = ledState;

    String out;
    out.reserve(2048);
    out += F("F1 parser self-test\n===================\n\n");

    // Pretend we are subscribed so the lamp-mapping path is exercised too
    f1Feed    = FEED_LIVE;
    f1Track   = TRK_UNKNOWN;
    f1Session = SES_UNKNOWN;
    f1IsLive  = -1;

    int passed = 0, failed = 0;
    char lineBuf[512];

    for (size_t i = 0; i < F1_TEST_COUNT; i++) {
        const F1TestStep& t = F1_TESTS[i];

        // f1HandleDataLine() writes into the buffer, so hand it a mutable copy
        strlcpy(lineBuf, t.line, sizeof(lineBuf));
        f1HandleSseLine(lineBuf);

        const bool ok = (f1Track == t.expTrack) && (f1Session == t.expSession);
        if (ok) passed++; else failed++;

        char row[192];
        snprintf(row, sizeof(row), "%-3u %-4s %-42s track=%-11s session=%s\n",
                 (unsigned)(i + 1), ok ? "PASS" : "FAIL", t.desc,
                 f1TrackName(f1Track), f1SessionStateName(f1Session));
        out += row;

        if (!ok) {
            char exp[128];
            snprintf(exp, sizeof(exp), "        expected track=%s session=%s\n",
                     f1TrackName(t.expTrack), f1SessionStateName(t.expSession));
            out += exp;
        }
    }

    // --- BOM handling, the other thing that silently breaks parsing ---
    {
        const char* bom = "\xEF\xBB\xBF{\"Status\":\"Available\"}";
        DynamicJsonDocument doc(128);
        const bool ok = deserializeJson(doc, f1SkipBom(bom)) == DeserializationError::Ok
                        && strcmp(doc["Status"] | "", "Available") == 0;
        if (ok) passed++; else failed++;
        char row[128];
        snprintf(row, sizeof(row), "%-3u %-4s %s\n",
                 (unsigned)(F1_TEST_COUNT + 1), ok ? "PASS" : "FAIL",
                 "UTF-8 BOM stripped from StreamingStatus.json");
        out += row;
    }

    char tail[96];
    snprintf(tail, sizeof(tail), "\n%d passed, %d failed\nfree heap: %u bytes\n",
             passed, failed, (unsigned)ESP.getFreeHeap());
    out += tail;

    // Restore
    f1Feed     = savedFeed;
    f1Track    = savedTrack;
    f1Session  = savedSession;
    f1IsLive   = savedIsLive;
    f1Override = savedOverride;
    ledState   = savedLedState;
    updateLEDs();

    Serial.printf("[F1] self-test: %d passed, %d failed\n", passed, failed);
    webServer.send(failed == 0 ? 200 : 500, "text/plain; charset=utf-8", out);
}
