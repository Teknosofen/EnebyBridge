#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

// arduino-audio-tools (v1.x path layout: src/AudioTools/...)
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Mini.h"
#include "AudioTools/AudioLibs/A2DPStream.h"

#include "config.h"

// ─── Audio pipeline ──────────────────────────────────────────────────────────
//
//  WiFiClient → MP3DecoderMini → A2DPStream (BT TX)
//
//  WiFiClient pulls MP3 via raw HTTP/1.0 GET.
//  MP3DecoderMini decodes MP3 → PCM and writes directly to A2DPStream.
//  No EncodedAudioStream wrapper — avoids its 4.6 KB heap buffer.
//

WiFiClient      streamClient;                       // raw TCP to stream server
A2DPStream      a2dpStream;                         // BT A2DP source
MP3DecoderMini  mp3Decoder;                         // MP3 → PCM (minimp3, low RAM)
static uint8_t  readBuf[512];                       // static read buffer (in .bss, not heap)

WebServer server(HTTP_PORT);

// ─── State ───────────────────────────────────────────────────────────────────
bool      isPlaying    = false;
bool      decoderReady = false;  // true after first decodedStream.begin()
String    currentUrl   = "";
int       currentVolume = 70;  // 0–100

// Deferred playback — URL is set by the HTTP handler, stream is opened
// in the main loop so the HTTP response completes first.
String    pendingUrl   = "";

// ─── WiFi reconnection ──────────────────────────────────────────────────────
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_MS = 10000;  // poll every 10 s
int wifiFailCount = 0;

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

    // Suppress all ESP-IDF internal logging — the WiFi stack's log writes
    // allocate a mutex lock; if heap is tight this causes abort().
    esp_log_level_set("*", ESP_LOG_NONE);

    Serial.printf("%lu [boot] Eneby BT bridge starting...\n", millis());

    // ── Bluetooth A2DP ───────────────────────────────────────────────────
    // Start BT, but don't block waiting for connection.  auto_reconnect
    // handles pairing in the background.  Blocking here eats 10-25 s of
    // radio time, starving WiFi when it starts.
    Serial.printf("%lu [bt] Starting A2DP to '%s'...\n", millis(), BT_DEVICE_NAME);
    auto a2dpCfg = a2dpStream.defaultConfig(TX_MODE);
    a2dpCfg.name           = BT_DEVICE_NAME;
    a2dpCfg.auto_reconnect = true;
    a2dpStream.begin(a2dpCfg);
    a2dpStream.setVolume(currentVolume / 100.0f);

    // ── MP3 decoder ──────────────────────────────────────────────────────
    // Allocate the 4.6 KB decode buffer NOW, while heap is ~60–160 KB and
    // unfragmented.  After WiFi connects, max contiguous block is ~2.5 KB.
    mp3Decoder.setOutput(a2dpStream);
    mp3Decoder.begin();
    decoderReady = true;
    Serial.printf("%lu [audio] Decoder allocated (heap: %d)\n", millis(), ESP.getFreeHeap());

    // ── WiFi ─────────────────────────────────────────────────────────────
    // Start WiFi immediately — don't wait for BT to settle.  Both stacks
    // connect in parallel; BT auto_reconnect recovers from any radio sharing.
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[wifi] Connecting");
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n%lu [wifi] Connected. IP: %s  heap: %d\n",
                      millis(), WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
    } else {
        Serial.printf("\n%lu [wifi] Not connected yet — will retry in loop\n", millis());
    }

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
                WiFi.begin(WIFI_SSID, WIFI_PASS);
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
        int avail = streamClient.available();
        if (avail > 0) {
            int toRead = min(avail, (int)sizeof(readBuf));
            int n = streamClient.read(readBuf, toRead);
            if (n > 0) {
                mp3Decoder.write(readBuf, n);
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

    // Yield to keep WiFi + BT stacks healthy.
    // 10 ms gives both radio stacks breathing room.
    delay(10);
}
