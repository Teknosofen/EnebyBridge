#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_coexist.h>
#include <esp_wifi.h>

// A2DPStream from audio-tools for Bluetooth A2DP output
#include "AudioTools.h"
#include "AudioTools/AudioLibs/A2DPStream.h"

// minimp3 directly — all state lives in BSS, zero heap allocation
#include "minimp3.h"

#include "config.h"

// ─── Audio pipeline ──────────────────────────────────────────────────────────
//
//  WiFiClient → minimp3 → A2DPStream (BT TX)
//
//  All decoder memory is statically allocated (BSS) so it never competes
//  with the BT/WiFi heap.  mp3dec_t lives here as a global, the input
//  accumulation buffer and PCM output are static arrays.
//

WiFiClient      streamClient;                       // raw TCP to stream server
A2DPStream      a2dpStream;                         // BT A2DP source

// ─── Static decoder state (BSS, not heap) ────────────────────────────────────
static mp3dec_t           mp3d;                      // minimp3 context
static mp3dec_frame_info_t mp3info;                  // last frame info
static mp3d_sample_t      pcmBuf[MINIMP3_MAX_SAMPLES_PER_FRAME]; // PCM out
static uint8_t            mp3InBuf[2048];            // input accumulation (1 MP3 frame max ~1440 B)
static int                mp3InLen = 0;              // bytes in mp3InBuf

WebServer server(HTTP_PORT);

// ─── State ───────────────────────────────────────────────────────────────────
bool      isPlaying    = false;
String    currentUrl   = "";
int       currentVolume = 70;  // 0–100

// Deferred playback — URL is set by the HTTP handler, stream is opened
// in the main loop so the HTTP response completes first.
String    pendingUrl   = "";

// ─── WiFi reconnection ──────────────────────────────────────────────────────
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_MS = 10000;  // poll every 10 s
int wifiFailCount = 0;
int32_t  cachedChannel = 0;                 // from pre-scan, reused in reconnect
uint8_t  cachedBSSID[6] = {0};

// ─── URL parser ──────────────────────────────────────────────────────────────
bool parseUrl(const String& url, String& host, uint16_t& port, String& path) {
    int idx = url.indexOf("://");
    if (idx < 0) return false;
    String rest = url.substring(idx + 3);
    int pathIdx = rest.indexOf('/');
    if (pathIdx < 0) { host = rest; path = "/"; }
    else { host = rest.substring(0, pathIdx); path = rest.substring(pathIdx); }
    int colonIdx = host.indexOf(':');
    if (colonIdx >= 0) { port = host.substring(colonIdx + 1).toInt(); host = host.substring(0, colonIdx); }
    else { port = 80; }
    return host.length() > 0;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────
void stopPlayback() {
    if (isPlaying) {
        streamClient.stop();
        mp3InLen = 0;  // flush decoder input
        isPlaying  = false;
        currentUrl = "";
        Serial.printf("%lu [audio] Stopped\n", millis());
    }
}

bool startPlayback(const String& url) {
    stopPlayback();
    Serial.printf("%lu [audio] Starting: %s (heap: %d / max_block: %d)\n",
                  millis(), url.c_str(), ESP.getFreeHeap(),
                  heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    String target = url;
    for (int redir = 0; redir < 3; redir++) {
        String host, path;
        uint16_t port;
        if (!parseUrl(target, host, port, path)) {
            Serial.printf("%lu [audio] Bad URL\n", millis());
            return false;
        }

        Serial.printf("%lu [audio] -> %s:%d%s (heap: %d)\n",
                      millis(), host.c_str(), port, path.c_str(), ESP.getFreeHeap());

        streamClient.setTimeout(10000);  // 10 s read timeout (ms)
        if (!streamClient.connect(host.c_str(), port, 10000)) {
            Serial.printf("%lu [audio] Connect failed (heap: %d)\n", millis(), ESP.getFreeHeap());
            return false;
        }

        // HTTP/1.0 avoids chunked encoding — we get raw MP3 bytes
        streamClient.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ESP32/1.0\r\nAccept: */*\r\n\r\n",
                            path.c_str(), host.c_str());

        // Read status line (works with both HTTP/1.x and ICY responses)
        String statusLine = streamClient.readStringUntil('\n');
        int statusCode = 0;
        int sp = statusLine.indexOf(' ');
        if (sp > 0) statusCode = statusLine.substring(sp + 1).toInt();
        Serial.printf("%lu [audio] HTTP %d (heap: %d)\n", millis(), statusCode, ESP.getFreeHeap());

        // Skip headers, capture Location for redirects
        String location;
        while (streamClient.connected()) {
            String hdr = streamClient.readStringUntil('\n');
            hdr.trim();
            if (hdr.length() == 0) break;  // blank line = end of headers
            if (hdr.startsWith("Location:") || hdr.startsWith("location:")) {
                location = hdr.substring(9);
                location.trim();
            }
        }

        if (statusCode >= 300 && statusCode < 400 && location.length() > 0) {
            Serial.printf("%lu [audio] Redirect -> %s\n", millis(), location.c_str());
            streamClient.stop();
            target = location;
            continue;
        }

        if (statusCode != 200) {
            Serial.printf("%lu [audio] HTTP error %d\n", millis(), statusCode);
            streamClient.stop();
            return false;
        }

        // Body starts here — raw MP3 data flows into the decoder.
        mp3InLen = 0;  // flush any stale input
        isPlaying  = true;
        currentUrl = url;
        Serial.printf("%lu [audio] Streaming! (heap: %d)\n", millis(), ESP.getFreeHeap());
        return true;
    }

    Serial.printf("%lu [audio] Too many redirects\n", millis());
    return false;
}

// ─── HTTP API handlers ────────────────────────────────────────────────────────

// GET /play?url=http://...
// Starts streaming the given URL to the Eneby.
// HA example:
//   curl "http://<esp32-ip>/play?url=http://stream.example.com/radio.mp3"
void handlePlay() {
    Serial.printf("%lu [http] >> GET /play\n", millis());
    if (!server.hasArg("url")) {
        server.send(400, "text/plain", "Missing 'url' parameter");
        return;
    }
    pendingUrl = server.arg("url");
    Serial.printf("%lu [http]    url=%s\n", millis(), pendingUrl.c_str());
    server.send(200, "text/plain", "OK");
}

// GET /stop
void handleStop() {
    Serial.printf("%lu [http] >> GET /stop\n", millis());
    stopPlayback();
    server.send(200, "text/plain", "OK");
}

// GET /volume?level=0..100
void handleVolume() {
    Serial.printf("%lu [http] >> GET /volume\n", millis());
    if (!server.hasArg("level")) {
        server.send(400, "text/plain", "Missing 'level' parameter");
        return;
    }
    int level = server.arg("level").toInt();
    level = constrain(level, 0, 100);
    currentVolume = level;
    uint8_t a2dpVol = map(level, 0, 100, 0, 127);
    a2dpStream.setVolume(a2dpVol / 127.0f);
    server.send(200, "text/plain", "OK");
}

// GET /status  → JSON
void handleStatus() {
    Serial.printf("%lu [http] >> GET /status\n", millis());
    String json = "{";
    json += "\"playing\":"  + String(isPlaying ? "true" : "false") + ",";
    json += "\"url\":\""    + currentUrl + "\",";
    json += "\"volume\":"   + String(currentVolume) + ",";
    json += "\"ip\":\""     + WiFi.localIP().toString() + "\",";
    json += "\"heap\":"     + String(ESP.getFreeHeap());
    json += "}";
    server.send(200, "application/json", json);
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // Only suppress verbose/debug/info logs — keep ERROR and WARN visible
    // so we can see silent failures (SBC init, heap alloc, etc.)
    esp_log_level_set("*", ESP_LOG_WARN);

    Serial.printf("%lu [boot] Eneby BT bridge starting...\n", millis());

    // ── minimp3 init (zero-alloc — just zeroes the struct in BSS) ────────
    mp3dec_init(&mp3d);

    // ── WiFi stack + pre-scan ────────────────────────────────────────────
    // 1. Initialize WiFi at high heap (~170 KB) and set PS_NONE now.
    // 2. Quick-scan to discover our AP's channel + BSSID.
    // 3. After BT connects, use WiFi.begin(ssid,pass,channel,bssid) which
    //    SKIPS the channel scan.  This takes <500 ms instead of 2-4 s,
    //    preventing BT supervision timeout.
    WiFi.mode(WIFI_STA);
    // Don't set PS_NONE — it gets applied lazily at connect time, when heap
    // is low, and pm_set_sleep_type crashes or disrupts BT.
    // Default MIN_MODEM will be used.
    WiFi.setAutoReconnect(true);
    Serial.printf("%lu [wifi] Stack init, heap: %d\n", millis(), ESP.getFreeHeap());

    int n = WiFi.scanNetworks(false, false, false, 300);  // active scan, 300ms/ch
    for (int i = 0; i < n; i++) {
        if (String(WIFI_SSID) == WiFi.SSID(i)) {
            cachedChannel = WiFi.channel(i);
            memcpy(cachedBSSID, WiFi.BSSID(i), 6);
            Serial.printf("%lu [wifi] Found '%s' on ch %d (RSSI %d)\n",
                          millis(), WIFI_SSID, cachedChannel, WiFi.RSSI(i));
            break;
        }
    }
    WiFi.scanDelete();  // free scan results
    Serial.printf("%lu [wifi] Pre-scan done, heap: %d\n", millis(), ESP.getFreeHeap());

    // ── Coexistence: balanced — silence frames keep BT alive ──────────
    // PREFER_BT starves WiFi so badly it can't complete WPA auth.
    // PREFER_BALANCE lets both stacks share radio time fairly;
    // the continuous silence writes ensure BT keeps requesting slots.
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);

    // ── Bluetooth A2DP ───────────────────────────────────────────────────
    Serial.printf("%lu [bt] Starting A2DP to '%s'...\n", millis(), BT_DEVICE_NAME);
    auto a2dpCfg = a2dpStream.defaultConfig(TX_MODE);
    a2dpCfg.name           = BT_DEVICE_NAME;
    a2dpCfg.auto_reconnect = true;
    a2dpStream.begin(a2dpCfg);
    a2dpStream.setVolume(currentVolume / 100.0f);

    // Wait for BT to connect — max 30 s
    Serial.print("[bt] Waiting for connection");
    unsigned long btStart = millis();
    while (!a2dpStream.isConnected() && millis() - btStart < 30000) {
        delay(500);
        Serial.print(".");
    }
    if (a2dpStream.isConnected()) {
        Serial.printf("\n%lu [bt] Connected! (heap: %d)\n", millis(), ESP.getFreeHeap());
        // Let AVDTP signaling (codec negotiation, stream open) finish
        // before touching WiFi.  isConnected() fires on A2DP profile
        // connect, but the audio stream may still be setting up.
        Serial.printf("%lu [bt] Waiting 3 s for AVDTP stream setup...\n", millis());
        delay(3000);
        Serial.printf("%lu [bt] Settled (heap: %d, bt: %s)\n",
                      millis(), ESP.getFreeHeap(),
                      a2dpStream.isConnected() ? "still up" : "DROPPED");
    } else {
        Serial.printf("\n%lu [bt] Not connected yet — auto_reconnect active\n", millis());
    }

    // ── WiFi connect (scan-free) ─────────────────────────────────────────
    // Use pre-scanned channel + BSSID to skip the 2-4 s channel scan that
    // kills the BT link.  Direct association takes <500 ms.
    if (cachedChannel > 0) {
        WiFi.begin(WIFI_SSID, WIFI_PASS, cachedChannel, cachedBSSID);
        Serial.printf("%lu [wifi] Connecting (ch %d, no scan)...\n", millis(), cachedChannel);
    } else {
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        Serial.printf("%lu [wifi] Connecting (full scan)...\n", millis());
    }
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n%lu [wifi] Connected. IP: %s  heap: %d\n",
                      millis(), WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
    } else {
        Serial.printf("\n%lu [wifi] Not connected yet — will retry in loop\n", millis());
    }

    Serial.printf("%lu [audio] Ready (heap: %d)\n", millis(), ESP.getFreeHeap());

    // ── HTTP API ──────────────────────────────────────────────────────────
    server.on("/play",   HTTP_GET, handlePlay);
    server.on("/stop",   HTTP_GET, handleStop);
    server.on("/volume", HTTP_GET, handleVolume);
    server.on("/status", HTTP_GET, handleStatus);
    server.begin();

    Serial.printf("%lu [http] API ready on port %d\n", millis(), HTTP_PORT);
    Serial.printf("%lu [boot] Ready. Waiting for commands.\n", millis());
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    // ── WiFi health check ─────────────────────────────────────────────────
    unsigned long now = millis();
    if (now - lastWifiCheck >= WIFI_CHECK_MS) {
        lastWifiCheck = now;
        if (WiFi.status() != WL_CONNECTED) {
            wifiFailCount++;
            Serial.printf("%lu [wifi] Not connected (check #%d)\n", millis(), wifiFailCount);
            // Nudge reconnect every 2nd check (20 s)
            if (wifiFailCount % 2 == 0) {
                Serial.printf("%lu [wifi] Nudging reconnect\n", millis());
                WiFi.disconnect(false);  // keep radio on!
                delay(500);
                if (cachedChannel > 0) {
                    WiFi.begin(WIFI_SSID, WIFI_PASS, cachedChannel, cachedBSSID);
                } else {
                    WiFi.begin(WIFI_SSID, WIFI_PASS);
                }
            }
            if (isPlaying) {
                Serial.printf("%lu [wifi] Stopping playback — no network\n", millis());
                stopPlayback();
            }
        } else if (wifiFailCount > 0) {
            Serial.printf("%lu [wifi] Reconnected after %d checks. IP: %s\n",
                          millis(), wifiFailCount, WiFi.localIP().toString().c_str());
            wifiFailCount = 0;
        }
    }

    server.handleClient();

    // ── Deferred stream start ───────────────────────────────────────────
    if (pendingUrl.length() > 0) {
        String url = pendingUrl;
        pendingUrl = "";

        // Wait for WiFi to be ready and the radio to settle after
        // the HTTP response was sent.
        delay(100);

        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("%lu [audio] WiFi not ready — waiting...\n", millis());
            unsigned long waitStart = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - waitStart < 10000) {
                delay(500);
            }
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("%lu [audio] WiFi OK (heap: %d). Starting stream...\n", millis(), ESP.getFreeHeap());
            if (!startPlayback(url)) {
                Serial.printf("%lu [audio] Failed to start stream\n", millis());
            }
        } else {
            Serial.printf("%lu [audio] Aborted — WiFi not connected\n", millis());
        }
    }

    if (isPlaying) {
        // Read from TCP into accumulation buffer
        int space = sizeof(mp3InBuf) - mp3InLen;
        if (space > 0) {
            int avail = streamClient.available();
            if (avail > 0) {
                int toRead = min(avail, space);
                int n = streamClient.read(mp3InBuf + mp3InLen, toRead);
                if (n > 0) mp3InLen += n;
            }
        }

        // Decode as many frames as possible from the accumulated buffer
        while (mp3InLen > 0) {
            int samples = mp3dec_decode_frame(&mp3d, mp3InBuf, mp3InLen,
                                             pcmBuf, &mp3info);
            if (mp3info.frame_bytes > 0) {
                int remaining = mp3InLen - mp3info.frame_bytes;
                if (remaining > 0)
                    memmove(mp3InBuf, mp3InBuf + mp3info.frame_bytes, remaining);
                mp3InLen = remaining;
            } else {
                break;  // not enough data for a frame
            }
            if (samples > 0) {
                int pcmBytes = samples * mp3info.channels * sizeof(mp3d_sample_t);
                a2dpStream.write((uint8_t*)pcmBuf, pcmBytes);
            }
        }

        static unsigned long lastPipelineLog = 0;
        if (millis() - lastPipelineLog >= 5000) {
            Serial.printf("%lu [audio] heap=%d bt=%s\n",
                          millis(), ESP.getFreeHeap(),
                          a2dpStream.isConnected() ? "yes" : "NO");
            lastPipelineLog = millis();
        }
        if (!streamClient.connected() && !streamClient.available()) {
            Serial.printf("%lu [audio] Stream ended\n", millis());
            stopPlayback();
        }
    }

    // ── BT keepalive: feed silence when idle ─────────────────────────────
    // When not streaming, the A2DP source has no data to send.  Without
    // outgoing audio the BT stack stops requesting radio time-slots from
    // the coex scheduler, so WiFi takes over and BT supervision times out.
    // Writing a small silence buffer keeps the A2DP source active and
    // forces the coex scheduler to keep granting BT radio time.
    // IMPORTANT: Throttle writes — a2dpStream.write() can block when the
    // internal ring buffer is full, which starves server.handleClient().
    if (!isPlaying && a2dpStream.isConnected()) {
        static unsigned long lastSilence = 0;
        if (now - lastSilence >= 20) {            // ~50 Hz, enough for BT keepalive
            lastSilence = now;
            static const uint8_t silence[128] = {0};
            a2dpStream.write(silence, sizeof(silence));
        }
    }

    // ── BT status (idle) ─────────────────────────────────────────────────
    if (!isPlaying) {
        static unsigned long lastIdleLog = 0;
        if (millis() - lastIdleLog >= 5000) {
            Serial.printf("%lu [idle] heap=%d bt=%s wifi=%s\n",
                          millis(), ESP.getFreeHeap(),
                          a2dpStream.isConnected() ? "yes" : "NO",
                          WiFi.status() == WL_CONNECTED ? "yes" : "NO");
            lastIdleLog = millis();
        }
    }

    // Yield to keep WiFi + BT stacks healthy.
    // 10 ms gives both radio stacks breathing room.
    delay(10);
}
