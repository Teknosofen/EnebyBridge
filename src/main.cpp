#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_coexist.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <lwip/sockets.h>

// BluetoothA2DPSource — direct access, no A2DPStream wrapper.
// A2DPStream's BufferRTOS blocks the BT callback with portMAX_DELAY,
// starving the SBC encoder and dropping BT after ~12 s.
#include "BluetoothA2DPSource.h"

// minimp3 directly — all state lives in BSS, zero heap allocation
#include "minimp3.h"

#include "config.h"

// ─── Audio pipeline ──────────────────────────────────────────────────────────
//
//  WiFiClient → minimp3 → ringBuffer (BSS) → BT callback → SBC → speaker
//
//  All decoder memory is statically allocated (BSS) so it never competes
//  with the BT/WiFi heap.  The BT callback reads decoded PCM from a
//  lock-free ring buffer and pads with silence when empty.
//

WiFiClient          streamClient;                   // raw TCP to stream server
BluetoothA2DPSource a2dp_source;                    // BT A2DP source (direct)

// ─── Lock-free SPSC ring buffer (BSS) for decoded PCM ────────────────────────
// Producer: loop() on core 1 writes decoded PCM
// Consumer: BT callback on core 0 reads PCM for SBC encoder
// One decoded frame = 4608 bytes (1152 samples * 2ch * 2B).
#define PCM_RING_SIZE 5120
static uint8_t  pcmRing[PCM_RING_SIZE];
static volatile uint32_t ringHead = 0;
static volatile uint32_t ringTail = 0;

static int ringWrite(const uint8_t* src, int len) {
    int written = 0;
    uint32_t h = ringHead;
    for (int i = 0; i < len; i++) {
        uint32_t next = (h + 1) % PCM_RING_SIZE;
        if (next == ringTail) break;   // full
        pcmRing[h] = src[i];
        h = next;
        written++;
    }
    ringHead = h;
    return written;
}

static int ringRead(uint8_t* dst, int len) {
    int rd = 0;
    uint32_t t = ringTail;
    uint32_t h = ringHead;
    while (rd < len && t != h) {
        dst[rd++] = pcmRing[t];
        t = (t + 1) % PCM_RING_SIZE;
    }
    ringTail = t;
    return rd;
}

static int ringAvailableForWrite() {
    uint32_t h = ringHead;
    uint32_t t = ringTail;
    uint32_t used = (h >= t) ? (h - t) : (PCM_RING_SIZE - t + h);
    return PCM_RING_SIZE - 1 - used;
}

// BT data callback — runs in BT task on core 0.
// Always returns `len` bytes (silence-padded) so the SBC encoder
// always has full, aligned frames and BT never starves.
static volatile uint32_t btCallbacks = 0;
static volatile uint32_t btRealBytes = 0;
static volatile uint32_t btSilenceBytes = 0;
static int32_t bt_data_callback(uint8_t* data, int32_t len) {
    int got = ringRead(data, len);
    if (got < len)
        memset(data + got, 0, len - got);
    btCallbacks++;
    btRealBytes += got;
    btSilenceBytes += (len - got);
    return len;
}

// BT connection state callback — diagnostics
static void bt_connection_state(esp_a2d_connection_state_t state, void*) {
    const char* names[] = {"Disconnected","Connecting","Connected","Disconnecting"};
    Serial.printf("%lu [bt] state -> %s\n", millis(),
                  (state <= 3) ? names[state] : "?");
}

// BT audio state callback — tracks when the speaker actually starts
// requesting audio data (AVDTP stream open → started).
static volatile bool btAudioStarted = false;
static void bt_audio_state(esp_a2d_audio_state_t state, void*) {
    const char* names[] = {"Suspended","Stopped","Started"};
    Serial.printf("%lu [bt] audio -> %s\n", millis(),
                  (state <= 2) ? names[state] : "?");
    btAudioStarted = (state == ESP_A2D_AUDIO_STATE_STARTED);
}

// ─── Static decoder state (BSS, not heap) ────────────────────────────────────
static mp3dec_t           mp3d;                      // minimp3 context
static mp3dec_frame_info_t mp3info;                  // last frame info
static mp3d_sample_t      pcmBuf[MINIMP3_MAX_SAMPLES_PER_FRAME]; // PCM out
static int                pcmWritePos = 0;   // next byte in pcmBuf to write to ring
static int                pcmWriteLen = 0;   // total bytes in pcmBuf from last decode
static uint8_t            mp3InBuf[4096];            // input accumulation buffer
static int                mp3InLen = 0;              // bytes in mp3InBuf

WebServer server(HTTP_PORT);

// ─── State ───────────────────────────────────────────────────────────────────
bool      isPlaying    = false;
String    currentUrl   = "";
int       currentVolume = 70;  // 0–100

// Pipeline diagnostics (file-scope so startPlayback() can reset them)
static uint32_t       framesDecoded    = 0;
static unsigned long  lastPipelineLog  = 0;
static bool           fmtLogged        = false;
static uint32_t       tcpBytesRead     = 0;
static uint32_t       pcmDiscards      = 0;

// Deferred playback — URL is set by the HTTP handler, stream is opened
// in the main loop so the HTTP response completes first.
String    pendingUrl   = "";
String    retryUrl     = "";  // saved for auto-retry after WiFi drop
static bool fragRetry  = false; // true = retry caused by heap fragmentation (fast retry)

// ─── WiFi reconnection ──────────────────────────────────────────────────────
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_MS = 5000;  // poll every 5 s
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
void stopPlayback(bool userStop = false) {
    if (isPlaying) {
        // If WiFi dropped mid-stream, save URL for auto-retry
        if (!userStop && currentUrl.length() > 0) {
            retryUrl = currentUrl;
            Serial.printf("%lu [audio] Saving URL for retry\n", millis());
        }
        streamClient.stop();
        mp3InLen = 0;  // flush decoder input
        pcmWritePos = pcmWriteLen = 0;  // flush PCM pipeline
        ringHead = ringTail = 0;        // flush ring buffer
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

        streamClient.setTimeout(2000);   // 2 s read timeout for headers
        streamClient.setNoDelay(true);     // disable Nagle

        Serial.printf("%lu [audio] Connecting...\n", millis());
        if (!streamClient.connect(host.c_str(), port, 5000)) {
            Serial.printf("%lu [audio] Connect failed (heap: %d)\n", millis(), ESP.getFreeHeap());
            return false;
        }
        Serial.printf("%lu [audio] TCP connected (heap: %d, blk: %d)\n",
                      millis(), ESP.getFreeHeap(),
                      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

        // Build HTTP request on stack to avoid heap allocation.
        {
            char req[300];
            int reqLen = snprintf(req, sizeof(req),
                "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ESP32/1.0\r\nAccept: */*\r\n\r\n",
                path.c_str(), host.c_str());
            int sent = streamClient.write((uint8_t*)req, reqLen);
            streamClient.flush();  // push to wire immediately
        }
        Serial.printf("%lu [audio] Request sent (heap: %d, blk: %d)\n",
                      millis(), ESP.getFreeHeap(),
                      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

        // Non-blocking line reader: readBytesUntil can deadlock at
        // low heap because lwIP recv blocks when pbufs can't be
        // allocated.  This helper yields with delay(1) between bytes
        // so BT callbacks keep firing and the watchdog is fed.
        auto readLine = [&](char* buf, int maxLen, unsigned long timeoutMs) -> int {
            unsigned long deadline = millis() + timeoutMs;
            int pos = 0;
            while (millis() < deadline && pos < maxLen - 1) {
                if (streamClient.available()) {
                    char c = streamClient.read();
                    if (c == '\n') break;
                    buf[pos++] = c;
                } else if (!streamClient.connected()) {
                    break;
                } else {
                    delay(1);
                }
            }
            buf[pos] = '\0';
            return pos;
        };

        char hdrBuf[256];
        int statusCode = 0;
        char locationBuf[200] = {0};

        // Status line (generous 5s timeout — server may be slow)
        int hLen = readLine(hdrBuf, sizeof(hdrBuf), 5000);
        { char* sp = strchr(hdrBuf, ' ');
          if (sp) statusCode = atoi(sp + 1); }
        Serial.printf("%lu [audio] HTTP %d (heap: %d)\n", millis(), statusCode, ESP.getFreeHeap());

        // Read headers, capture Location for redirects
        unsigned long hdrDeadline = millis() + 5000;
        while (streamClient.connected() && millis() < hdrDeadline) {
            hLen = readLine(hdrBuf, sizeof(hdrBuf), 2000);
            // trim \r
            if (hLen > 0 && hdrBuf[hLen - 1] == '\r') hdrBuf[--hLen] = '\0';
            if (hLen == 0) break;  // blank line = end of headers
            if (strncasecmp(hdrBuf, "Location:", 9) == 0) {
                const char* loc = hdrBuf + 9;
                while (*loc == ' ') loc++;
                strncpy(locationBuf, loc, sizeof(locationBuf) - 1);
            }
        }

        if (statusCode >= 300 && statusCode < 400 && locationBuf[0]) {
            Serial.printf("%lu [audio] Redirect -> %s\n", millis(), locationBuf);
            streamClient.stop();
            target = locationBuf;
            continue;
        }

        if (statusCode != 200) {
            Serial.printf("%lu [audio] HTTP error %d\n", millis(), statusCode);
            streamClient.stop();
            return false;
        }

        // Body starts here — raw MP3 data flows into the decoder.
        mp3InLen = 0;  // flush any stale input
        mp3dec_init(&mp3d);  // reset decoder for clean sync
        isPlaying  = true;
        currentUrl = url;
        // Reset pipeline diagnostics for this stream
        fmtLogged = false;
        framesDecoded = 0;
        tcpBytesRead = 0;
        pcmDiscards = 0;
        lastPipelineLog = millis();
        btCallbacks = btRealBytes = btSilenceBytes = 0;
        // Don't touch coex — PREFER_BALANCE (set once in setup) avoids
        // starving either radio.  Changing coex during A2DP kills BT callbacks.

        // Pre-buffer: fill the buffer before starting decode.
        // BT callback continues on core 0 (feeding silence).
        // This batch-receives TCP data so pbufs are allocated/freed
        // in one burst rather than continuously fragmenting the heap.
        Serial.printf("%lu [audio] Pre-buffering...\n", millis());
        unsigned long preStart = millis();
        while (mp3InLen < 1536 && streamClient.connected()
               && millis() - preStart < 3000) {
            int space = sizeof(mp3InBuf) - mp3InLen;
            if (space > 0) {
                int avail = streamClient.available();
                if (avail > 0) {
                    int n = streamClient.read(mp3InBuf + mp3InLen, min(avail, space));
                    if (n > 0) { mp3InLen += n; tcpBytesRead += n; }
                }
            }
            delay(1);
        }
        Serial.printf("%lu [audio] Buffered %d B (heap: %d, blk: %d)\n",
                      millis(), mp3InLen, ESP.getFreeHeap(),
                      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

        Serial.printf("%lu [audio] Streaming! (heap: %d)\n", millis(), ESP.getFreeHeap());
        return true;
    }

    Serial.printf("%lu [audio] Too many redirects\n", millis());
    return false;
}

// ─── HTTP API handlers ────────────────────────────────────────────────────────

// GET /play?url=http://...
void handlePlay() {
    Serial.printf("%lu [http] >> GET /play\n", millis());
    if (!server.hasArg("url")) {
        server.send(400, "text/plain", "Missing 'url' parameter");
        return;
    }
    pendingUrl = server.arg("url");
    retryUrl = "";  // new URL overrides any pending retry
    Serial.printf("%lu [http]    url=%s\n", millis(), pendingUrl.c_str());
    server.send(200, "text/plain", "OK");
}

// GET /stop
void handleStop() {
    Serial.printf("%lu [http] >> GET /stop\n", millis());
    retryUrl = "";  // user explicitly stopped — no auto-retry
    stopPlayback(true);
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
    a2dp_source.set_volume(map(level, 0, 100, 0, 127));
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

    // Suppress all ESP-IDF logging.  WiFi timer task calls wifi_log on
    // core 0; at low heap the UART mutex alloc fails → abort().  Our own
    // Serial.printf() calls bypass esp_log and are safe.
    esp_log_level_set("*", ESP_LOG_NONE);

    Serial.printf("%lu [boot] Eneby BT bridge starting...\n", millis());

    // ── minimp3 init (zero-alloc — just zeroes the struct in BSS) ────────
    mp3dec_init(&mp3d);

    // ════════════════════════════════════════════════════════════════════
    // Boot order: WiFi FIRST, then BT.
    //
    // WiFi needs a clean radio for the channel scan + WPA handshake.
    // Once WiFi is associated, BT can pair and the coex scheduler
    // arbitrates normally.
    // ════════════════════════════════════════════════════════════════════

    // ── 1. WiFi connect (clean radio, high heap ~124 KB) ─────────────
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("%lu [wifi] Connecting... (heap: %d)\n", millis(), ESP.getFreeHeap());

    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
        delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("%lu [wifi] Connected. IP: %s  heap: %d\n",
                      millis(), WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
        // Use 20 MHz channel width — halves WiFi radio time per packet,
        // leaving more air time for BT A2DP.
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    } else {
        Serial.printf("%lu [wifi] Not connected yet — will retry in loop\n", millis());
    }

    // ── 2. Coex: prioritize BT for pairing ──────────────────────
    esp_coex_preference_set(ESP_COEX_PREFER_BT);

    // ── 3. Bluetooth A2DP (direct BluetoothA2DPSource) ───────────────
    //
    // A2DPStream's BufferRTOS used portMAX_DELAY + trigger_level==512
    // which blocked the BT callback for ~1.6 s per cycle, starving the
    // SBC encoder.  Using BluetoothA2DPSource directly with a non-
    // blocking callback that always returns silence-padded data fixes
    // the keepalive.
    //
    Serial.printf("%lu [bt] Starting A2DP to '%s'...\n", millis(), BT_DEVICE_NAME);

    // Release BLE controller+host memory — we only use BT Classic.
    // Frees ~30 KB of contiguous DRAM that BLE would otherwise reserve.
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    a2dp_source.set_auto_reconnect(true);
    a2dp_source.set_volume(map(currentVolume, 0, 100, 0, 127));
    a2dp_source.set_on_connection_state_changed(bt_connection_state);
    a2dp_source.set_on_audio_state_changed(bt_audio_state);
    a2dp_source.start_raw(BT_DEVICE_NAME, bt_data_callback);

    // Wait for BT to connect — max 30 s
    Serial.print("[bt] Waiting for connection");
    unsigned long btStart = millis();
    while (!a2dp_source.is_connected() && millis() - btStart < 30000) {
        delay(500);
        Serial.print(".");
    }
    if (a2dp_source.is_connected()) {
        Serial.printf("\n%lu [bt] Connected! (heap: %d, blk: %d)\n", millis(),
                      ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        // Wait for the speaker to enter streaming state (audio -> Started)
        // which means the SBC encoder is running and the BT data callback
        // will be invoked.  Without this, bt_cb stays at 0.
        Serial.printf("%lu [bt] Waiting for audio state...\n", millis());
        unsigned long audioWait = millis();
        while (!btAudioStarted && millis() - audioWait < 10000) {
            delay(100);
        }
        if (btAudioStarted) {
            Serial.printf("%lu [bt] Audio streaming (heap: %d)\n", millis(), ESP.getFreeHeap());
        } else {
            Serial.printf("%lu [bt] Audio not started yet\n", millis());
        }
        Serial.printf("%lu [bt] Settled (heap: %d)\n", millis(), ESP.getFreeHeap());
    } else {
        Serial.printf("\n%lu [bt] Not connected yet — auto_reconnect active\n", millis());
    }

    // ── 4. Switch to balanced coex for normal operation ──────────────
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    Serial.printf("%lu [coex] -> PREFER_BALANCE\n", millis());

    // Check if WiFi survived BT startup
    Serial.printf("%lu [status] bt=%s wifi=%s heap=%d\n",
                  millis(),
                  a2dp_source.is_connected() ? "yes" : "NO",
                  WiFi.status() == WL_CONNECTED ? "yes" : "NO",
                  ESP.getFreeHeap());

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
                WiFi.begin(WIFI_SSID, WIFI_PASS);
            }
            if (isPlaying && !streamClient.connected()) {
                Serial.printf("%lu [wifi] Stopping playback — TCP dead\n", millis());
                stopPlayback();
            }
        } else if (wifiFailCount > 0) {
            Serial.printf("%lu [wifi] Reconnected after %d checks. IP: %s\n",
                          millis(), wifiFailCount, WiFi.localIP().toString().c_str());
            wifiFailCount = 0;
            // Auto-retry stream if it was interrupted by WiFi drop
            if (retryUrl.length() > 0 && pendingUrl.length() == 0) {
                Serial.printf("%lu [wifi] Auto-retrying: %s\n", millis(), retryUrl.c_str());
                pendingUrl = retryUrl;
                retryUrl = "";
            }
        }
    }

    server.handleClient();

    // ── WiFi-recovery retry (with cooldown) ─────────────────────────────
    // Wait 5 s after a stream failure before retrying so the WiFi/BT
    // stacks have time to stabilise and we can observe BT callback health.
    static unsigned long lastStreamStop = 0;
    unsigned long retryCooldown = fragRetry ? 1000 : 5000;
    if (!isPlaying && retryUrl.length() > 0 && pendingUrl.length() == 0
        && WiFi.status() == WL_CONNECTED
        && millis() - lastStreamStop >= retryCooldown) {
        fragRetry = false;
        Serial.printf("%lu [wifi] Auto-retrying: %s\n", millis(), retryUrl.c_str());
        pendingUrl = retryUrl;
        retryUrl = "";
    }

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
        // Read all available TCP data immediately.  This frees lwIP pbufs
        // ASAP, letting the heap defragment.  Throttling reads (e.g. 512 B)
        // keeps pbufs allocated longer and fragments the heap fatally.
        int space = sizeof(mp3InBuf) - mp3InLen;
        if (space > 0) {
            int avail = streamClient.available();
            if (avail > 0) {
                int toRead = min(avail, space);
                int n = streamClient.read(mp3InBuf + mp3InLen, toRead);
                if (n > 0) { mp3InLen += n; tcpBytesRead += n; }
            }
        }

        // ── Single-step decode pipeline ──────────────────────────────────
        // Write pending PCM to ring, then decode one frame.
        // If ring is full, discard remaining PCM to prevent permanent
        // deadlock (BT may have stalled due to heap fragmentation).
        if (pcmWritePos < pcmWriteLen) {
            int avail = ringAvailableForWrite();
            if (avail > 0) {
                int toWrite = min(avail, pcmWriteLen - pcmWritePos);
                ringWrite((uint8_t*)pcmBuf + pcmWritePos, toWrite);
                pcmWritePos += toWrite;
            } else {
                // Ring full — discard to keep pipeline flowing
                pcmWritePos = pcmWriteLen;
                pcmDiscards++;
            }
        }
        // Decode one MP3 frame when pcmBuf is empty.
        // Even if the ring is full, decoding keeps TCP flowing (freeing
        // pbufs).  Excess PCM is discarded — that's fine for live radio.
        else if (mp3InLen > 0) {
            int samples = mp3dec_decode_frame(&mp3d, mp3InBuf, mp3InLen,
                                             pcmBuf, &mp3info);
            if (mp3info.frame_bytes > 0) {
                int remaining = mp3InLen - mp3info.frame_bytes;
                if (remaining > 0)
                    memmove(mp3InBuf, mp3InBuf + mp3info.frame_bytes, remaining);
                mp3InLen = remaining;
            }
            if (samples > 0) {
                pcmWriteLen = samples * mp3info.channels * sizeof(mp3d_sample_t);
                pcmWritePos = 0;
                framesDecoded++;
                if (!fmtLogged) {
                    Serial.printf("%lu [audio] MP3: %d Hz, %d ch, %d samples, %d bytes\n",
                                  millis(), mp3info.hz, mp3info.channels, samples, pcmWriteLen);
                    fmtLogged = true;
                }
                int avail = ringAvailableForWrite();
                if (avail > 0) {
                    int toWrite = min(avail, pcmWriteLen);
                    ringWrite((uint8_t*)pcmBuf, toWrite);
                    pcmWritePos = toWrite;
                }
            } else if (mp3info.frame_bytes == 0 && mp3InLen >= (int)sizeof(mp3InBuf)) {
                memmove(mp3InBuf, mp3InBuf + 1, mp3InLen - 1);
                mp3InLen--;
            }
        }

        if (millis() - lastPipelineLog >= 2000) {
            uint32_t cb = btCallbacks; btCallbacks = 0;
            uint32_t rb = btRealBytes; btRealBytes = 0;
            uint32_t sb = btSilenceBytes; btSilenceBytes = 0;
            uint32_t total = rb + sb;
            int pct = total > 0 ? (int)(rb * 100 / total) : 0;
            int blk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            Serial.printf("%lu [audio] heap=%d blk=%d bt=%s dec=%u cb=%u real=%u%% tcp=%u in=%d conn=%d dsc=%u\n",
                          millis(), ESP.getFreeHeap(), blk,
                          a2dp_source.is_connected() ? "y" : "N",
                          framesDecoded, cb, pct, tcpBytesRead,
                          mp3InLen, streamClient.connected(), pcmDiscards);
            framesDecoded = 0;
            tcpBytesRead = 0;
            pcmDiscards = 0;
            lastPipelineLog = millis();

            // Proactive reconnect: only at truly critical fragmentation.
            // blk routinely dips to 2000-3000 during normal pbuf cycles
            // and recovers.  Only disconnect when blk is catastrophically
            // low for two consecutive checks (4 seconds).
            static int lowBlkCount = 0;
            if (blk < 1200) {
                lowBlkCount++;
                if (lowBlkCount >= 2 && currentUrl.length() > 0) {
                    Serial.printf("%lu [audio] blk=%d x%d — proactive reconnect\n", millis(), blk, lowBlkCount);
                    lowBlkCount = 0;
                    fragRetry = true;
                    String saveUrl = currentUrl;
                    stopPlayback();
                    retryUrl = saveUrl;
                    lastStreamStop = millis();
                }
            } else {
                lowBlkCount = 0;
            }
        }
        if (!streamClient.connected() && !streamClient.available()) {
            Serial.printf("%lu [audio] Stream ended (wifi=%s tcp_total=%u)\n",
                          millis(),
                          WiFi.status() == WL_CONNECTED ? "yes" : "NO",
                          tcpBytesRead);
            stopPlayback();
            lastStreamStop = millis();
        }
    }

    // No manual silence needed — bt_data_callback() pads with zeros
    // automatically whenever the ring buffer is empty.

    // ── BT status (idle) ─────────────────────────────────────────────────
    if (!isPlaying) {
        static unsigned long lastIdleLog = 0;
        if (millis() - lastIdleLog >= 5000) {
            uint32_t icb = btCallbacks; btCallbacks = 0;
            btRealBytes = 0; btSilenceBytes = 0;
            Serial.printf("%lu [idle] heap=%d max_blk=%d bt=%s audio=%s wifi=%s bt_cb=%u\n",
                          millis(), ESP.getFreeHeap(),
                          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                          a2dp_source.is_connected() ? "yes" : "NO",
                          btAudioStarted ? "STREAM" : "idle",
                          WiFi.status() == WL_CONNECTED ? "yes" : "NO",
                          icb);
            lastIdleLog = millis();
        }
    }

    // Yield to keep WiFi + BT stacks healthy.
    // When streaming with nothing to do, yield longer so the WiFi and BT
    // tasks on core 0 get uninterrupted radio time.
    if (isPlaying && mp3InLen == 0 && pcmWritePos >= pcmWriteLen) {
        delay(5);   // nothing pending — let radios breathe
    } else {
        delay(1);   // work to do — stay responsive
    }
}
