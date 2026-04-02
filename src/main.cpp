#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

// arduino-audio-tools (v1.x path layout: src/AudioTools/...)
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Mini.h"
#include "AudioTools/AudioLibs/A2DPStream.h"

#include "config.h"

// ─── Audio pipeline ──────────────────────────────────────────────────────────
//
//  URLStream  →  EncodedAudioStream (MP3 decode)  →  A2DPStream (BT TX)
//
//  URLStream pulls HTTP audio in chunks.
//  EncodedAudioStream wraps the Helix MP3 decoder.
//  A2DPStream sends decoded PCM to the Eneby via Classic Bluetooth A2DP.
//

URLStream       urlStream;   // HTTP client — WiFi managed in setup()
A2DPStream      a2dpStream;                         // BT A2DP source
MP3DecoderMini  mp3Decoder;                         // MP3 → PCM (minimp3, low RAM)
EncodedAudioStream decodedStream(&a2dpStream, &mp3Decoder);
StreamCopy      copier(decodedStream, urlStream);   // drives the pipeline

WebServer server(HTTP_PORT);

// ─── State ───────────────────────────────────────────────────────────────────
bool      isPlaying    = false;
String    currentUrl   = "";
int       currentVolume = 70;  // 0–100

// ─── Helpers ─────────────────────────────────────────────────────────────────
void stopPlayback() {
    if (isPlaying) {
        urlStream.end();
        decodedStream.end();
        isPlaying  = false;
        currentUrl = "";
        Serial.println("[audio] Stopped");
    }
}

bool startPlayback(const String& url) {
    stopPlayback();

    Serial.printf("[audio] Starting stream: %s\n", url.c_str());

    // Prevent URLStream from calling esp_wifi_set_ps(WIFI_PS_NONE) —
    // that aborts when Bluetooth is also active.
    urlStream.setPowerSave(true);

    // Open HTTP stream — assumes MP3 content
    if (!urlStream.begin(url.c_str(), "audio/mpeg")) {
        Serial.println("[audio] Failed to open URL");
        return false;
    }

    // Start decoder → A2DP pipeline
    auto audioCfg = decodedStream.defaultConfig();
    audioCfg.sample_rate  = 44100;
    audioCfg.channels     = 2;
    audioCfg.bits_per_sample = 16;
    decodedStream.begin(audioCfg);

    isPlaying  = true;
    currentUrl = url;
    return true;
}

// ─── HTTP API handlers ────────────────────────────────────────────────────────

// GET /play?url=http://...
// Starts streaming the given URL to the Eneby.
// HA example:
//   curl "http://<esp32-ip>/play?url=http://stream.example.com/radio.mp3"
void handlePlay() {
    if (!server.hasArg("url")) {
        server.send(400, "text/plain", "Missing 'url' parameter");
        return;
    }
    String url = server.arg("url");
    if (startPlayback(url)) {
        server.send(200, "text/plain", "OK");
    } else {
        server.send(500, "text/plain", "Failed to open stream");
    }
}

// GET /stop
void handleStop() {
    stopPlayback();
    server.send(200, "text/plain", "OK");
}

// GET /volume?level=0..100
// A2DP volume is 0–127; we map 0-100 → 0-127
void handleVolume() {
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
    Serial.println("\n[boot] Eneby BT bridge starting...");

    // ── WiFi ──────────────────────────────────────────────────────────────────
    // Connect WiFi first so it gets the radio without BT competition.
    // Set MIN_MODEM sleep *before* BT starts so coex initialises correctly
    // (WIFI_PS_NONE aborts when BT is active; setting MIN_MODEM early avoids this).
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[wifi] Connecting");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    Serial.printf("\n[wifi] Connected. IP: %s\n",
                  WiFi.localIP().toString().c_str());

    // ── Bluetooth A2DP ────────────────────────────────────────────────────────
    // Start BT after WiFi is stable and already in MIN_MODEM sleep mode.
    Serial.printf("[bt] Starting A2DP to '%s'...\n", BT_DEVICE_NAME);
    auto a2dpCfg = a2dpStream.defaultConfig(TX_MODE);
    a2dpCfg.name           = BT_DEVICE_NAME;
    a2dpCfg.auto_reconnect = true;
    a2dpStream.begin(a2dpCfg);
    a2dpStream.setVolume(currentVolume / 100.0f);

    // ── HTTP API ──────────────────────────────────────────────────────────────
    server.on("/play",   HTTP_GET, handlePlay);
    server.on("/stop",   HTTP_GET, handleStop);
    server.on("/volume", HTTP_GET, handleVolume);
    server.on("/status", HTTP_GET, handleStatus);
    server.begin();

    Serial.printf("[http] API ready on port %d\n", HTTP_PORT);
    Serial.println("[boot] Ready. Waiting for commands.");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    server.handleClient();

    if (isPlaying) {
        // Drives the URLStream → decoder → A2DP pipeline.
        // copy() is non-blocking; it transfers whatever is available.
        if (copier.copy() == 0) {
            // Nothing copied — stream may have ended or stalled
            if (!urlStream.available()) {
                Serial.println("[audio] Stream ended");
                stopPlayback();
            }
        }
    }

    // Small yield to keep WiFi stack healthy
    delay(1);
}
