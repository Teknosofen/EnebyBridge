# Eneby BT Bridge — Documentation

## Overview

The Eneby BT Bridge turns a plain ESP32-WROOM into a WiFi-to-Bluetooth audio
bridge for the IKEA Eneby 20 speaker. It receives audio stream URLs over HTTP
(from Home Assistant or any HTTP client), pulls the MP3 stream over WiFi,
decodes it in real time, and transmits the PCM audio to the Eneby speaker over
Bluetooth A2DP.

No additional hardware is required — just an ESP32-WROOM-32 and a USB cable.

---

## Architecture

```
┌──────────────────┐
│  Home Assistant   │
│  (or any client)  │
└────────┬─────────┘
         │  HTTP GET  /play?url=...
         │            /stop
         │            /volume?level=...
         │            /status
         ▼
┌──────────────────────────────────────────────────┐
│                  ESP32-WROOM                      │
│                                                   │
│  ┌───────────┐    ┌──────────────┐    ┌────────┐ │
│  │ URLStream │──▶│ MP3 Decoder  │──▶│ A2DP   │ │
│  │ (HTTP/WiFi)│    │ (minimp3)    │    │ Stream │ │
│  └───────────┘    └──────────────┘    └───┬────┘ │
│                                           │      │
│  ┌─────────────┐   ┌───────────────────┐  │      │
│  │ WebServer   │   │ WiFi reconnect    │  │      │
│  │ (HTTP API)  │   │ + coex scheduler  │  │      │
│  └─────────────┘   └───────────────────┘  │      │
└───────────────────────────────────────────┼──────┘
                                            │ Bluetooth A2DP
                                            ▼
                                    ┌──────────────┐
                                    │  IKEA Eneby   │
                                    │  20 Speaker   │
                                    └──────────────┘
```

### Audio pipeline

The audio pipeline is a three-stage chain:

1. **URLStream** — Opens an HTTP connection to the given MP3 URL and pulls
   audio data in chunks over WiFi.
2. **EncodedAudioStream + MP3DecoderMini** — Decodes the MP3 frames into raw
   PCM (44100 Hz, 16-bit, stereo) using the lightweight minimp3 library.
3. **A2DPStream** — Transmits the decoded PCM samples to the Eneby speaker
   over Classic Bluetooth A2DP (Advanced Audio Distribution Profile).

A `StreamCopy` object drives the pipeline by pulling data from URLStream and
pushing it through the decoder into the A2DP output in each `loop()` iteration.

### WiFi / Bluetooth coexistence

The ESP32-WROOM has a **single 2.4 GHz radio** shared between WiFi and
Bluetooth via time-division multiplexing. This project addresses the coexistence
challenge with:

- **`esp_coex_preference_set(ESP_COEX_PREFER_BT)`** — Tells the radio scheduler
  to prioritize Bluetooth. A2DP is latency-sensitive; HTTP streaming tolerates
  jitter.
- **`WIFI_PS_MIN_MODEM`** — Keeps WiFi in minimum-power sleep so it yields
  radio time to BT more gracefully.
- **Enlarged A2DP buffers** (12 × 512 B = 6 KB) — Absorbs timing jitter when
  the radio briefly switches to WiFi.
- **Automatic WiFi reconnection** — Polls connection health every 10 seconds;
  forces a full reconnect cycle every 3rd failure.

---

## Features

| Feature | Description |
|---|---|
| **HTTP-controlled playback** | Start/stop MP3 streams via simple GET requests |
| **Volume control** | Set volume 0–100 over HTTP; mapped to A2DP 0–127 |
| **Status endpoint** | JSON status with playing state, URL, volume, IP, free heap |
| **Home Assistant integration** | REST commands, template media_player, REST sensor, automations |
| **BT auto-reconnect** | Automatically reconnects to the Eneby when powered on |
| **WiFi auto-reconnect** | Recovers from WiFi drops without rebooting |
| **BT-priority coexistence** | Radio scheduler prioritizes A2DP to prevent audio drops |
| **Low memory footprint** | Uses minimp3 decoder; runs on WROOM (520 KB RAM, no PSRAM) |

---

## Hardware requirements

- ESP32-WROOM-32 (any variant)
- USB cable for flashing
- IKEA Eneby 20 (or any Bluetooth A2DP speaker)

---

## Configuration

Copy `include/config.h.example` to `include/config.h` and edit:

```cpp
#define WIFI_SSID       "your_wifi_ssid"
#define WIFI_PASS       "your_wifi_password"
#define BT_DEVICE_NAME  "ENEBY20"          // your speaker's BT name
#define HTTP_PORT       80
#define HTTP_STREAM_BUFFER_SIZE  (16 * 1024)  // 16 KB — tune if needed
```

---

## Build and flash

```bash
# Using PlatformIO CLI:
pio run --target upload

# Monitor serial output:
pio device monitor
```

Or use the PlatformIO toolbar in VS Code (Upload button).

---

## HTTP API

All endpoints use **GET** requests. Replace `<ip>` with your ESP32's IP address.

### `GET /play?url=<stream-url>`

Start streaming an MP3 URL to the speaker.

```bash
curl "http://<ip>/play?url=http://ice1.somafm.com/groovesalad-128-mp3"
```

**Responses:**
- `200 OK` — Playback started
- `400 Missing 'url' parameter`
- `500 Failed to open stream`

### `GET /stop`

Stop the current stream.

```bash
curl "http://<ip>/stop"
```

### `GET /volume?level=<0-100>`

Set the playback volume (0 = mute, 100 = max).

```bash
curl "http://<ip>/volume?level=60"
```

### `GET /status`

Returns JSON with the current device state.

```bash
curl "http://<ip>/status"
```

**Example response:**

```json
{
  "playing": true,
  "url": "http://ice1.somafm.com/groovesalad-128-mp3",
  "volume": 60,
  "ip": "192.168.1.92",
  "heap": 142568
}
```

---

## Usage examples

### Quick test from the command line

```bash
# Play a free internet radio stream (SomaFM Groove Salad)
curl "http://192.168.1.92/play?url=http://ice1.somafm.com/groovesalad-128-mp3"

# Check status
curl "http://192.168.1.92/status"

# Set volume to 50%
curl "http://192.168.1.92/volume?level=50"

# Stop playback
curl "http://192.168.1.92/stop"
```

### From a web browser

Open in any browser:
```
http://192.168.1.92/play?url=http://ice1.somafm.com/groovesalad-128-mp3
```

### Some free MP3 streams for testing

| Stream | URL |
|---|---|
| SomaFM Groove Salad | `http://ice1.somafm.com/groovesalad-128-mp3` |
| SomaFM Drone Zone | `http://ice1.somafm.com/dronezone-128-mp3` |
| SomaFM DEF CON | `http://ice1.somafm.com/defcon-128-mp3` |
| Swiss Radio Jazz | `http://stream.srg-ssr.ch/m/rsj/mp3_128` |

---

## Home Assistant integration

Add the contents of `home_assistant.yaml` to your HA `configuration.yaml`.
Replace `192.168.1.92` with your ESP32's actual IP.

### REST commands

```yaml
rest_command:
  eneby_play:
    url: "http://192.168.1.92/play"
    method: GET
    params:
      url: "{{ url }}"

  eneby_stop:
    url: "http://192.168.1.92/stop"
    method: GET

  eneby_volume:
    url: "http://192.168.1.92/volume"
    method: GET
    params:
      level: "{{ level }}"
```

### Call from Developer Tools → Services

```yaml
service: rest_command.eneby_play
data:
  url: "http://ice1.somafm.com/groovesalad-128-mp3"
```

### Template media_player

The `home_assistant.yaml` file includes a template `media_player` entity that
gives you a proper player card in HA dashboards with play/stop and volume
controls.

### REST sensor

A REST sensor polls `/status` every 10 seconds, exposing the playing state,
volume, current URL, and free heap as attributes.

### Example automations

- **Play radio** — triggered by an `input_boolean` toggle
- **Stop on away** — stops playback when you leave home

### Script: play any URL

```yaml
service: script.play_on_eneby
data:
  url: "http://ice1.somafm.com/groovesalad-128-mp3"
```

---

## Boot sequence

1. WiFi connects (15 s timeout; retries in the main loop if it fails)
2. Power-save mode set to `WIFI_PS_MIN_MODEM`
3. Coexistence scheduler set to `ESP_COEX_PREFER_BT`
4. Bluetooth A2DP starts and connects to the Eneby (auto-reconnect enabled)
5. HTTP server starts on the configured port
6. Main loop: services HTTP requests, drives the audio pipeline, monitors WiFi

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| BT keeps dropping | Coex scheduler favoring WiFi | Verify `ESP_COEX_PREFER_BT` is set; check serial log |
| WiFi won't reconnect | Auto-reconnect not kicking in | Watch serial — full reconnect fires every 3rd check |
| Heap allocation failed | Buffers too large for available RAM | Reduce `HTTP_STREAM_BUFFER_SIZE` or `A2DP_BUFFER_COUNT` |
| Stuttering audio | WiFi interference / weak signal | Move ESP32 closer to router; check free heap via `/status` |
| BT won't connect | Eneby paired to another device | Forget previous pairings on the Eneby and re-pair |
| Stream won't start | URL not serving MP3 | Verify the URL serves `audio/mpeg` content |
| No sound but "playing" | Volume too low or Eneby muted | Call `/volume?level=80`; check Eneby physical volume |

---

## Memory budget

The ESP32-WROOM has 520 KB internal SRAM. After WiFi + BT stacks initialize,
roughly 140–200 KB of heap remains. Key allocations:

| Component | Size |
|---|---|
| A2DP buffers | 12 × 512 B = 6 KB |
| HTTP stream buffer | 16 KB |
| MP3 decoder (minimp3) | ~20 KB |
| WebServer | ~2 KB |

Monitor free heap via the `/status` endpoint or serial output.

---

## Project structure

```
EnebyBridge/
├── platformio.ini          # Build config, libraries, A2DP buffer tuning
├── home_assistant.yaml     # HA integration (REST commands, media_player, etc.)
├── documentation.md        # This file
├── README.md               # Quick-start guide
├── include/
│   ├── config.h.example    # Template — copy to config.h and edit
│   └── config.h            # Your local WiFi/BT settings (git-ignored)
└── src/
    └── main.cpp            # All firmware logic (setup, loop, HTTP API, audio)
```

---

## Dependencies

Managed by PlatformIO (`platformio.ini`):

| Library | Version | Purpose |
|---|---|---|
| [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) | v1.0.0 | Audio pipeline framework (URLStream, StreamCopy, EncodedAudioStream) |
| [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) | v1.8.4 | Bluetooth A2DP source driver |
| [minimp3](https://github.com/pschatzmann/minimp3) | latest | Lightweight MP3 decoder |
