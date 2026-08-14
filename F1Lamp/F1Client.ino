// F1Client.ino - Formula 1 live status from the official timing feed
//
// Two data paths, both unauthenticated:
//
//   1. https://livetiming.formula1.com/static/StreamingStatus.json   (23 bytes)
//      Polled while idle. {"Status":"Offline"} vs {"Status":"Available"}.
//
//   2. https://livetiming.formula1.com/signalrcore                   (~6 bytes/s)
//      SignalR Core over Server-Sent Events. Opened only while the feed is
//      available. We subscribe to TrackStatus + SessionStatus only, which keeps
//      every message under ~200 bytes. (RaceControlMessages is deliberately NOT
//      subscribed - its initial payload replays the whole session history and
//      runs to 14 kB and upwards.)
//
// Two things bite anyone implementing this:
//   * The feed sits behind an AWS load balancer. The negotiate response sets
//     AWSALB / AWSALBCORS cookies and every later request MUST replay them, or
//     the server answers "No Connection with that ID".
//   * SignalR Core records are terminated by 0x1E, not by a newline.

#define F1_IDLE_POLL_MS   120000UL   // StreamingStatus poll interval while idle
#define F1_DEV_POLL_MS    8000UL     // ...and when pointed at the dev simulator
#define F1_DEV_RETRY_MAX  5000UL     // dev: retry every 5 s, no backoff ramp
#define F1_STALE_MS       45000UL    // no data/ping for this long -> stale (pings are ~15 s)
#define F1_RETRY_MIN_MS   5000UL
#define F1_RETRY_MAX_MS   300000UL
#define F1_RS             '\x1e'     // SignalR Core record separator

// The SSE stream can run over TLS (the real service) or plain HTTP (the local
// simulator), so we keep one of each and point f1Stream at whichever is in use.
static WiFiClientSecure f1StreamTls;
static WiFiClient       f1StreamPlain;
static WiFiClient*      f1Stream = nullptr;
static char     f1Cookie[384] = "";          // "AWSALB=..; AWSALBCORS=..;"
static char     f1TokenEnc[192] = "";        // URL-encoded connection token
static uint32_t f1LastPoll    = 0;
static uint32_t f1LastData    = 0;
static uint32_t f1RetryDelay  = F1_RETRY_MIN_MS;
static uint32_t f1NextRetry   = 0;
static char     f1Buf[768];                  // SSE line assembly
static size_t   f1BufLen      = 0;

// Link diagnostics, surfaced by GET /api/state so the feed can be debugged
// without a serial cable attached.
uint32_t f1RxBytes   = 0;
uint32_t f1RxRecords = 0;
uint32_t f1Connects  = 0;
uint32_t f1Drops     = 0;
char     f1LastError[48] = "";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

const char* f1TrackName(F1Track t) {
    switch (t) {
        case TRK_CLEAR:       return "clear";
        case TRK_YELLOW:      return "yellow";
        case TRK_SC:          return "safety_car";
        case TRK_RED:         return "red";
        case TRK_VSC:         return "vsc";
        case TRK_VSC_ENDING:  return "vsc_ending";
        default:              return "unknown";
    }
}

const char* f1SessionStateName(F1Session s) {
    switch (s) {
        case SES_INACTIVE:  return "inactive";
        case SES_STARTED:   return "started";
        case SES_ABORTED:   return "aborted";
        case SES_FINISHED:  return "finished";
        case SES_FINALISED: return "finalised";
        default:            return "unknown";
    }
}

const char* f1FeedName(F1Feed f) {
    switch (f) {
        case FEED_IDLE:       return "idle";
        case FEED_CONNECTING: return "connecting";
        case FEED_LIVE:       return "live";
        case FEED_STALE:      return "stale";
        default:              return "disabled";
    }
}

uint32_t f1SinceData()      { return millis() - f1LastData; }
int      f1StreamConnected() { return f1Stream ? (int)f1Stream->connected() : -1; }
int      f1StreamAvailable() { return f1Stream ? (int)f1Stream->available()  : -1; }

bool f1IsSessionLive() {
    return f1Feed == FEED_LIVE && f1Session == SES_STARTED;
}

// False when pointed at the development simulator instead of the real service.
// The UI warns about this so a test setting cannot quietly survive into normal
// use, where it would look like F1 tracking simply never triggers.
bool f1UsingRealService() {
    return strcmp(Config::f1_host, "livetiming.formula1.com") == 0
        && Config::f1_port == 443 && Config::f1_tls;
}

static void f1UrlEncode(const char* in, char* out, size_t outSize) {
    static const char* hex = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < outSize; i++) {
        const unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

// ---------------------------------------------------------------------------
// Lamp mapping
// ---------------------------------------------------------------------------

static void f1SetOverride(uint8_t r, uint8_t g, uint8_t b, int effect, int speed) {
    f1R = r; f1G = g; f1B = b;
    f1Effect = effect;
    f1Speed  = speed;
    f1Override = true;
}

static void f1ApplyTrackToLamp() {
    switch (f1Track) {
        case TRK_CLEAR:      f1SetOverride(  0, 255,   0, EFFECT_SOLID,   5); break;
        case TRK_YELLOW:     f1SetOverride(255, 170,   0, EFFECT_SOLID,   5); break;
        case TRK_SC:         f1SetOverride(255, 170,   0, EFFECT_BLINK,   4); break;
        case TRK_VSC:        f1SetOverride(255, 170,   0, EFFECT_BREATHE, 3); break;
        case TRK_VSC_ENDING: f1SetOverride(255, 170,   0, EFFECT_BREATHE, 9); break;
        case TRK_RED:        f1SetOverride(255,   0,   0, EFFECT_SOLID,   5); break;
        default:
            // Unknown status: hold the last known good display rather than
            // guessing green.
            return;
    }
    updateLEDs();
}

// Drive the lamp on session start/stop. Only acts on a TRANSITION, so the user
// can still override manually over MQTT or the web UI in between.
static void f1SetLive(bool live) {
    if (f1IsLive == (int8_t)live) return;
    f1IsLive = (int8_t)live;

    if (live) {
        Serial.println("[F1] session live - lamp follows track status");
        ledState = true;
        f1ApplyTrackToLamp();
    } else {
        Serial.println("[F1] no session live - lamp off");
        f1Override = false;
        ledState   = false;
        updateLEDs();
    }
    publishState();
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

// Picks TLS or plain according to Config::f1_tls and connects. Returns the
// connected client, or nullptr. Both objects are supplied by the caller so
// they live for as long as the request does.
static WiFiClient* f1Connect(WiFiClientSecure& tls, WiFiClient& plain) {
    WiFiClient* c;
    if (Config::f1_tls) {
        tls.setInsecure();
        c = &tls;
    } else {
        c = &plain;
    }
    c->setTimeout(10);
    if (!c->connect(Config::f1_host, Config::f1_port)) {
        c->stop();          // a failed connect can still leave a socket behind
        return nullptr;
    }
    return c;
}

// ---------------------------------------------------------------------------
// HTTP plumbing
// ---------------------------------------------------------------------------

// Reads one CRLF line with an explicit deadline.
//
// Deliberately avoids Stream::readStringUntil(): that relies on the Stream
// timeout and on connected(), and ESP32 reports connected() == false as soon as
// the peer closes even while data is still sitting in the RX buffer. Since we
// send "Connection: close", that race silently truncated larger responses.
static bool f1ReadLine(WiFiClient& c, char* buf, size_t bufSize, uint32_t deadline) {
    size_t n = 0;
    while ((int32_t)(millis() - deadline) < 0) {
        if (!c.available()) {
            if (!c.connected()) break;      // closed AND drained
            delay(1);
            continue;
        }
        const int ch = c.read();
        if (ch < 0)    continue;
        if (ch == '\r') continue;
        if (ch == '\n') { buf[n] = '\0'; return true; }
        if (n < bufSize - 1) buf[n++] = (char)ch;
    }
    buf[n] = '\0';
    return n > 0;
}

// Reads the body. With a known Content-Length we read exactly that many bytes;
// otherwise we drain until the peer closes. Never relies on Stream timeouts.
static String f1ReadBody(WiFiClient& c, int contentLength, uint32_t deadline) {
    String out;
    // Reserve up front on BOTH paths. Arduino's String grows in 16-byte steps,
    // so an unreserved byte-at-a-time read reallocates repeatedly and
    // fragments the heap. With Content-Length known this is a single alloc.
    out.reserve(contentLength > 0 ? contentLength + 1 : 512);

    while ((int32_t)(millis() - deadline) < 0) {
        if (contentLength > 0 && (int)out.length() >= contentLength) break;
        if (!c.available()) {
            if (!c.connected()) break;      // closed AND drained
            delay(1);
            continue;
        }
        const int ch = c.read();
        if (ch >= 0) out += (char)ch;
    }
    return out;
}

// Reads status line + headers. Captures Set-Cookie and Content-Length when
// asked. Leaves the client positioned at the first body byte.
static int f1ReadHeaders(WiFiClient& c, bool captureCookies, int* contentLength = nullptr) {
    int  status = 0;
    bool first  = true;
    char line[256];
    const uint32_t deadline = millis() + 5000;

    if (contentLength) *contentLength = -1;

    while ((int32_t)(millis() - deadline) < 0) {
        if (!f1ReadLine(c, line, sizeof(line), deadline)) {
            if (!c.connected() && !c.available()) break;
            continue;
        }

        if (first) {
            const char* sp = strchr(line, ' ');
            if (sp) status = atoi(sp + 1);
            first = false;
            continue;
        }
        if (line[0] == '\0') break;          // blank line: end of headers

        if (contentLength && strncasecmp(line, "Content-Length:", 15) == 0) {
            *contentLength = atoi(line + 15);
        }

        if (captureCookies && strncasecmp(line, "Set-Cookie:", 11) == 0) {
            String v = String(line + 11);
            v.trim();
            const int semi = v.indexOf(';');
            if (semi > 0) v = v.substring(0, semi);   // keep only NAME=VALUE
            if (v.startsWith("AWSALB")) {
                if (f1Cookie[0] != '\0') strlcat(f1Cookie, "; ", sizeof(f1Cookie));
                strlcat(f1Cookie, v.c_str(), sizeof(f1Cookie));
            }
        }
    }
    return status;
}

// ---------------------------------------------------------------------------
// Step 1: is anything live at all?
// ---------------------------------------------------------------------------

static bool f1FetchStreamingStatus(bool& available) {
    WiFiClientSecure tls;
    WiFiClient       plain;
    WiFiClient* c = f1Connect(tls, plain);
    if (!c) {
        Serial.println("[F1] StreamingStatus: connect failed");
        strlcpy(f1LastError, "streamingstatus connect failed", sizeof(f1LastError));
        return false;
    }

    c->printf("GET /static/StreamingStatus.json HTTP/1.1\r\n"
              "Host: %s\r\n"
              "User-Agent: F1Lamp\r\n"
              "Connection: close\r\n\r\n", Config::f1_host);

    int len = -1;
    const int status = f1ReadHeaders(*c, false, &len);
    String body = f1ReadBody(*c, len, millis() + 4000);
    c->stop();

    if (status != 200) {
        Serial.printf("[F1] StreamingStatus: HTTP %d\n", status);
        return false;
    }

    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, f1SkipBom(body.c_str())) != DeserializationError::Ok) {
        Serial.printf("[F1] StreamingStatus: bad JSON (len=%d, got %d bytes): %.60s\n",
                      len, (int)body.length(), body.c_str());
        return false;
    }

    const char* s = doc["Status"] | "";
    available = (strcasecmp(s, "Available") == 0);
    return true;
}

// ---------------------------------------------------------------------------
// Step 2: negotiate (captures the sticky-session cookies)
// ---------------------------------------------------------------------------

static bool f1Negotiate() {
    WiFiClientSecure tls;
    WiFiClient       plain;
    WiFiClient* c = f1Connect(tls, plain);
    if (!c) return false;

    c->printf("POST /signalrcore/negotiate?negotiateVersion=1 HTTP/1.1\r\n"
              "Host: %s\r\n"
              "User-Agent: F1Lamp\r\n"
              "Content-Length: 0\r\n"
              "Connection: close\r\n\r\n", Config::f1_host);

    f1Cookie[0] = '\0';
    int len = -1;
    const int status = f1ReadHeaders(*c, true, &len);
    String body = f1ReadBody(*c, len, millis() + 4000);
    c->stop();

    if (status != 200) {
        Serial.printf("[F1] negotiate: HTTP %d\n", status);
        return false;
    }

    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, f1SkipBom(body.c_str())) != DeserializationError::Ok) {
        Serial.printf("[F1] negotiate: bad JSON (len=%d, got %d bytes): %.80s\n",
                      len, (int)body.length(), body.c_str());
        snprintf(f1LastError, sizeof(f1LastError), "negotiate bad JSON (%d/%d B)",
                 (int)body.length(), len);
        return false;
    }

    const char* tok = doc["connectionToken"] | "";
    if (tok[0] == '\0') {
        Serial.println("[F1] negotiate: no connectionToken");
        return false;
    }
    f1UrlEncode(tok, f1TokenEnc, sizeof(f1TokenEnc));

    if (f1Cookie[0] == '\0') {
        // Without the ALB cookie every later request lands on another backend
        Serial.println("[F1] negotiate: WARNING no ALB cookie received");
    }
    return true;
}

// ---------------------------------------------------------------------------
// Step 3: open the SSE receive stream (must happen before any POST)
// ---------------------------------------------------------------------------

static bool f1OpenStream() {
    f1Stream = f1Connect(f1StreamTls, f1StreamPlain);
    if (!f1Stream) return false;

    f1Stream->printf("GET /signalrcore?id=%s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: F1Lamp\r\n"
                     "Accept: text/event-stream\r\n"
                     "Cookie: %s\r\n"
                     "Connection: keep-alive\r\n\r\n",
                     f1TokenEnc, Config::f1_host, f1Cookie);

    const int status = f1ReadHeaders(*f1Stream, false);
    if (status != 200) {
        Serial.printf("[F1] stream: HTTP %d\n", status);
        f1Stream->stop();
        f1Stream = nullptr;
        return false;
    }
    f1BufLen  = 0;
    f1LastData = millis();
    return true;
}

// ---------------------------------------------------------------------------
// Step 4: send a SignalR record (handshake / Subscribe) on its own connection
// ---------------------------------------------------------------------------

static bool f1Send(const char* record) {
    WiFiClientSecure tls;
    WiFiClient       plain;
    WiFiClient* c = f1Connect(tls, plain);
    if (!c) return false;

    c->printf("POST /signalrcore?id=%s HTTP/1.1\r\n"
              "Host: %s\r\n"
              "User-Agent: F1Lamp\r\n"
              "Cookie: %s\r\n"
              "Content-Type: text/plain;charset=UTF-8\r\n"
              "Content-Length: %u\r\n"
              "Connection: close\r\n\r\n%s",
              f1TokenEnc, Config::f1_host, f1Cookie, (unsigned)strlen(record), record);

    int len = -1;
    const int status = f1ReadHeaders(*c, false, &len);
    f1ReadBody(*c, len, millis() + 3000);
    c->stop();
    if (status != 200) Serial.printf("[F1] send: HTTP %d\n", status);
    return status == 200;
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------

static void f1ApplyTrackStatus(const char* code) {
    F1Track t = TRK_UNKNOWN;
    if      (strcmp(code, "1") == 0) t = TRK_CLEAR;
    else if (strcmp(code, "2") == 0) t = TRK_YELLOW;
    else if (strcmp(code, "4") == 0) t = TRK_SC;
    else if (strcmp(code, "5") == 0) t = TRK_RED;
    else if (strcmp(code, "6") == 0) t = TRK_VSC;
    else if (strcmp(code, "7") == 0) t = TRK_VSC_ENDING;
    else {
        Serial.printf("[F1] unknown TrackStatus '%s' - holding last state\n", code);
        return;
    }

    if (t == f1Track) return;
    f1Track = t;
    Serial.printf("[F1] track status -> %s\n", f1TrackName(t));

    if (f1IsSessionLive()) {
        f1ApplyTrackToLamp();
        publishState();
    }
}

static void f1ApplySessionStatus(const char* s) {
    F1Session prev = f1Session;
    if      (strcasecmp(s, "Inactive")  == 0) f1Session = SES_INACTIVE;
    else if (strcasecmp(s, "Started")   == 0) f1Session = SES_STARTED;
    else if (strcasecmp(s, "Aborted")   == 0) f1Session = SES_ABORTED;
    else if (strcasecmp(s, "Finished")  == 0) f1Session = SES_FINISHED;
    else if (strcasecmp(s, "Ends")      == 0) f1Session = SES_FINISHED;
    else if (strcasecmp(s, "Finalised") == 0) f1Session = SES_FINALISED;
    else                                      f1Session = SES_UNKNOWN;

    if (f1Session != prev) Serial.printf("[F1] session status -> %s\n", f1SessionStateName(f1Session));
}

// Handles one topic payload, from either the Subscribe result or a live update
static void f1HandleTopic(const char* topic, JsonVariantConst data) {
    if (strcmp(topic, "TrackStatus") == 0) {
        const char* code = data["Status"] | "";
        if (code[0]) f1ApplyTrackStatus(code);
    } else if (strcmp(topic, "SessionStatus") == 0) {
        const char* st = data["Status"] | "";
        if (st[0]) f1ApplySessionStatus(st);
    }
}

static void f1HandleRecord(const char* json) {
    if (json[0] == '\0' || strcmp(json, "{}") == 0) return;
    f1RxRecords++;

    DynamicJsonDocument doc(768);
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;

    const int type = doc["type"] | 0;

    if (type == 6) return;                    // keep-alive ping, timestamp already updated

    if (type == 3) {                          // Subscribe completion: initial snapshot
        JsonObjectConst res = doc["result"];
        for (JsonPairConst kv : res) f1HandleTopic(kv.key().c_str(), kv.value());
        f1SetLive(f1IsSessionLive());
        return;
    }

    if (type == 1) {                          // live update: ["Topic", {...}, "ts"]
        JsonArrayConst args = doc["arguments"];
        if (args.size() >= 2) {
            const char* topic = args[0] | "";
            if (topic[0]) {
                f1HandleTopic(topic, args[1]);
                f1SetLive(f1IsSessionLive());
            }
        }
    }
}

// The static JSON files are served with a UTF-8 BOM, which ArduinoJson rejects.
// Skip anything before the opening brace.
const char* f1SkipBom(const char* s) {
    while (*s && *s != '{') s++;
    return s;
}

// A "data:" line may carry several 0x1E-separated records
static void f1HandleDataLine(char* line) {
    char* rec = line;
    while (rec && *rec) {
        char* sep = strchr(rec, F1_RS);
        if (sep) *sep = '\0';
        while (*rec == ' ') rec++;
        if (*rec) f1HandleRecord(rec);
        rec = sep ? sep + 1 : nullptr;
    }
}

// One complete SSE line. Non-static so the self-test can feed it directly.
void f1HandleSseLine(char* line) {
    if (strncmp(line, "data:", 5) == 0) f1HandleDataLine(line + 5);
}

// ---------------------------------------------------------------------------
// Connection / teardown
// ---------------------------------------------------------------------------

static void f1Disconnect() {
    // stop() UNCONDITIONALLY. The old guard only closed the socket while
    // connected() was still true, so every dropped stream leaked its file
    // descriptor. ESP32 has ~10 sockets total, so after a handful of reconnects
    // the web server could no longer accept - the device answered ping but not
    // HTTP, and the F1 stream starved because no socket was left to service.
    if (f1Stream) f1Stream->stop();
    f1Stream = nullptr;
    f1StreamPlain.stop();
    f1StreamTls.stop();
    f1BufLen = 0;
}

void f1Shutdown() {
    f1Disconnect();
    f1Feed      = FEED_DISABLED;
    f1Track     = TRK_UNKNOWN;
    f1Session   = SES_UNKNOWN;
    f1IsLive    = -1;
    f1SessionName[0] = '\0';
    if (f1Override) {
        f1Override = false;
        updateLEDs();
    }
}

static void f1Backoff() {
    f1Drops++;
    f1Disconnect();
    f1Feed      = FEED_IDLE;
    f1NextRetry = millis() + f1RetryDelay;
    const uint32_t cap = f1UsingRealService() ? (uint32_t)F1_RETRY_MAX_MS
                                              : (uint32_t)F1_DEV_RETRY_MAX;
    f1RetryDelay = min(f1RetryDelay * 2, cap);
    Serial.printf("[F1] retry in %lu s\n", (unsigned long)(f1RetryDelay / 1000));
    f1SetLive(false);
}

static void f1Connect() {
    f1Feed = FEED_CONNECTING;
    f1Disconnect();   // defensive: never overwrite a live f1Stream and leak its socket
    Serial.printf("[F1] connecting (heap %u)\n", (unsigned)ESP.getFreeHeap());

    if (!f1Negotiate())  { f1Backoff(); return; }
    if (!f1OpenStream()) { f1Backoff(); return; }

    // Handshake, then subscribe. Order matters: the SSE GET above must already
    // be established or the server rejects these with 404.
    static const char HS[]  = "{\"protocol\":\"json\",\"version\":1}\x1e";
    static const char SUB[] = "{\"arguments\":[[\"TrackStatus\",\"SessionStatus\"]],"
                              "\"invocationId\":\"0\",\"target\":\"Subscribe\",\"type\":1}\x1e";

    if (!f1Send(HS))  { f1Backoff(); return; }
    if (!f1Send(SUB)) { f1Backoff(); return; }

    f1Feed       = FEED_LIVE;
    f1LastData   = millis();
    f1RetryDelay = F1_RETRY_MIN_MS;
    f1Connects++;
    f1LastError[0] = '\0';
    Serial.printf("[F1] subscribed (heap %u)\n", (unsigned)ESP.getFreeHeap());
}

// ---------------------------------------------------------------------------
// Stream pump - non-blocking, called from loop()
// ---------------------------------------------------------------------------

static void f1PumpStream() {
    if (!f1Stream) return;
    while (f1Stream->available()) {
        const int ch = f1Stream->read();
        if (ch < 0) break;
        f1LastData = millis();
        f1RxBytes++;

        if (ch == '\r') continue;
        if (ch == '\n') {
            f1Buf[f1BufLen] = '\0';
            if (strncmp(f1Buf, "data:", 5) == 0) f1HandleDataLine(f1Buf + 5);
            f1BufLen = 0;
            continue;
        }
        if (f1BufLen < sizeof(f1Buf) - 1) {
            f1Buf[f1BufLen++] = (char)ch;
        } else {
            // Oversized line - drop it rather than corrupt state, but say so.
            // Silently losing a status update would leave the lamp showing
            // stale information with no clue why.
            Serial.printf("[F1] SSE line over %u bytes, dropped\n",
                          (unsigned)sizeof(f1Buf));
            f1BufLen = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// f1Poll - state machine, called every loop()
// ---------------------------------------------------------------------------

void f1Poll() {
    // Disabled -> make sure nothing is left running
    if (!Config::f1_enabled) {
        if (f1Feed != FEED_DISABLED) {
            Serial.println("[F1] tracking disabled");
            f1Shutdown();
        }
        return;
    }

    if (WiFi.status() != WL_CONNECTED) return;

    const uint32_t now = millis();

    // Just enabled: switch the lamp off until a session is found
    if (f1Feed == FEED_DISABLED) {
        Serial.println("[F1] tracking enabled");
        f1Feed       = FEED_IDLE;
        f1LastPoll   = 0;
        f1NextRetry  = 0;
        f1RetryDelay = F1_RETRY_MIN_MS;
        f1IsLive     = -1;
        f1SetLive(false);
        return;
    }

    if (f1Feed == FEED_IDLE) {
        if (now < f1NextRetry) return;
        const uint32_t pollMs = f1UsingRealService() ? (uint32_t)F1_IDLE_POLL_MS
                                                     : (uint32_t)F1_DEV_POLL_MS;
        if (f1LastPoll != 0 && now - f1LastPoll < pollMs) return;
        f1LastPoll = now;

        bool available = false;
        if (!f1FetchStreamingStatus(available)) { f1Backoff(); return; }

        if (available) {
            Serial.println("[F1] feed available");
            f1Connect();
        } else {
            Serial.printf("[F1] %s says Offline - nothing live, lamp stays off\n",
                          Config::f1_host);
            f1SetLive(false);
        }
        return;
    }

    if (f1Feed == FEED_LIVE || f1Feed == FEED_STALE) {
        if (!f1Stream || !f1Stream->connected()) {
            Serial.println("[F1] stream closed");
        strlcpy(f1LastError, "stream closed by peer", sizeof(f1LastError));
            f1Backoff();
            return;
        }

        f1PumpStream();



        // Fail safe: never keep claiming a safety car on stale data.
        //
        // Re-read the clock here. `now` was sampled at the top of f1Poll(),
        // BEFORE f1PumpStream() ran - and the pump sets f1LastData to a LATER
        // millis(). Using the stale `now` made (now - f1LastData) underflow to
        // ~4.29e9, so the staleness test fired every single time data arrived,
        // dropping a perfectly healthy stream and reconnecting forever.
        const uint32_t sinceData = millis() - f1LastData;
        if (sinceData >= F1_STALE_MS) {
            if (f1Feed != FEED_STALE) {
                Serial.println("[F1] stale - no data or ping, dropping connection");
                strlcpy(f1LastError, "stale: no data for 45s", sizeof(f1LastError));
                f1Feed = FEED_STALE;
                f1SetLive(false);
            }
            f1Backoff();
            return;
        }

        // NOTE: do NOT drop the stream when the session finalises.
        // The Subscribe snapshot reports the CURRENT session state, so if we
        // connect after the last session ended it says "finalised" straight
        // away. Disconnecting here caused a reconnect storm - connect, read
        // "finalised", drop, poll, reconnect - which starved loop() so badly
        // that the web server stopped answering while ping still worked.
        // Holding the stream costs ~6 bytes/second and means we see the next
        // session start immediately. f1SetLive() has already switched the lamp
        // off, and stale detection still tears down a dead link.
    }
}
