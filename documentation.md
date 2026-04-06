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
│  │WiFiClient │──▶│ minimp3      │──▶│ SPSC   │ │
│  │(raw TCP)  │    │ (BSS, 0 heap)│    │ ring   │ │
│  └───────────┘    └──────────────┘    └───┬────┘ │
│   core 1: loop()                          │      │
│                                           │      │
│  ┌─────────────┐   ┌───────────────────┐  │      │
│  │ WebServer   │   │ WiFi reconnect    │  │      │
│  │ (HTTP API)  │   │ + heap monitor    │  │      │
│  └─────────────┘   └───────────────────┘  │      │
│                     core 0: BT callback ──┘      │
└───────────────────────────────────────────┼──────┘
                                            │ BT A2DP (SBC)
                                            ▼
                                    ┌──────────────┐
                                    │  IKEA Eneby   │
                                    │  20 Speaker   │
                                    └──────────────┘
```

### Audio pipeline

The audio pipeline is a three-stage chain with all decoder state in BSS
(zero heap allocation):

1. **WiFiClient** — Opens a raw TCP connection to the MP3 stream URL and
   reads data in chunks. Non-blocking header parsing prevents lwIP deadlocks
   at low heap. Supports HTTP redirects (up to 3).
2. **minimp3** — Decodes MP3 frames into raw PCM (44100 Hz, 16-bit, stereo).
   The decoder context, input buffer (4 KB), and PCM output buffer all live
   in BSS, never competing with the WiFi/BT heap.
3. **BluetoothA2DPSource** — A lock-free SPSC ring buffer (5 KB, BSS) bridges
   the decoder (core 1) and the BT data callback (core 0). The callback
   always returns `len` bytes, padding with silence when the ring is empty.

A single-step decode loop in `loop()` reads TCP data, decodes one MP3
frame, and writes PCM to the ring buffer each iteration.

### WiFi / Bluetooth coexistence

The ESP32-WROOM has a **single 2.4 GHz radio** shared between WiFi and
Bluetooth via time-division multiplexing. This project addresses the coexistence
challenge with:

- **`ESP_COEX_PREFER_BT`** during pairing, then **`ESP_COEX_PREFER_BALANCE`**
  for normal operation. Changing coex during A2DP streaming kills BT callbacks.
- **20 MHz WiFi bandwidth** (`WIFI_BW_HT20`) — halves WiFi radio time per
  packet, leaving more air time for BT A2DP.
- **BLE memory release** (`esp_bt_controller_mem_release(ESP_BT_MODE_BLE)`) —
  frees ~10 KB of contiguous DRAM since only BT Classic is used.
- **Reduced A2DP buffers** (2 × 512 B = 1 KB) and **reduced lwIP TCP buffers**
  (2920 B window) to minimize heap pressure.
- **Proactive reconnect** — monitors `heap_caps_get_largest_free_block()` and
  disconnects TCP before fragmentation kills both radios.
- **Automatic WiFi reconnection** — polls every 5 seconds; nudges reconnect
  every 2nd failure.

> **Known limitation:** Despite these optimizations, lwIP packet buffer
> allocation fragments the heap over 10–30 seconds, dropping `max_blk`
> below the ~2 KB needed for WiFi DMA and BT SBC encoding. This causes
> intermittent audio (~60–65% real PCM). A platform with separate WiFi
> and BT radios (e.g. Raspberry Pi Zero 2 W) would avoid this entirely.

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
| **BT-priority pairing, balanced streaming** | Coex set to PREFER_BT for pairing, PREFER_BALANCE during playback |
| **BLE memory release** | Frees ~10 KB contiguous DRAM for BT Classic + WiFi |
| **Proactive reconnect** | Monitors heap fragmentation; reconnects TCP before radios die |
| **Low memory footprint** | All decoder state in BSS; runs on WROOM (520 KB RAM, no PSRAM) |

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

1. WiFi connects first (clean radio, high heap ~130 KB)
2. 20 MHz WiFi bandwidth set (`WIFI_BW_HT20`)
3. Coexistence set to `ESP_COEX_PREFER_BT` for pairing
4. BLE memory released (`esp_bt_controller_mem_release`)
5. BluetoothA2DPSource starts and connects to the Eneby (auto-reconnect)
6. Waits for BT audio state → Started (SBC encoder running)
7. Coexistence switched to `ESP_COEX_PREFER_BALANCE`
8. HTTP server starts on the configured port
9. Main loop: services HTTP requests, drives the audio pipeline, monitors WiFi/heap

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `hash_map_set` crash at boot | Ring buffer or BSS too large for heap | Keep `PCM_RING_SIZE` ≤ 5120 |
| WiFi drops during streaming | Heap fragmentation (blk < 2 KB) | Proactive reconnect handles this; stream auto-retries |
| Heap allocation failed | TCP buffers fragmenting heap | Normal at low blk — check serial diagnostics |
| Stuttering / intermittent audio | Single-radio coex limitation | Expected (~60-65% real PCM); see Known Limitation |
| BT won't connect | Eneby paired to another device | Forget previous pairings on the Eneby and re-pair |
| Stream won't start | URL not serving MP3 | Verify the URL serves raw MP3 (no HLS/AAC) |
| No sound but "playing" | BT callback not firing | Check serial for `audio=STREAM` and `bt_cb > 0` |
| Serial freezes after TCP connect | Header read deadlock | Non-blocking reader should prevent this; check heap |

---

## Memory budget

The ESP32-WROOM has 520 KB internal SRAM. After WiFi + BT Classic stacks
initialize, roughly 30–35 KB of heap remains. All decoder state is statically
allocated in BSS to avoid competing with the WiFi/BT heap.

| Component | Size | Location |
|---|---|---|
| PCM ring buffer | 5 KB | BSS |
| MP3 input buffer | 4 KB | BSS |
| minimp3 decoder context | ~7 KB | BSS |
| PCM output buffer | ~4.5 KB | BSS |
| A2DP buffers | 2 × 512 B = 1 KB | Heap (BT stack) |
| lwIP TCP buffers | ~3 KB window | Heap (lwIP) |
| WebServer | ~2 KB | Heap |

BLE memory release (`esp_bt_controller_mem_release`) recovers ~10 KB of
contiguous DRAM. Monitor free heap and largest contiguous block via `/status`
or serial output (logged every 2–5 seconds).

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
| [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) | v1.0.0 | Dependency of ESP32-A2DP (not directly used in pipeline) |
| [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) | v1.8.4 | BluetoothA2DPSource — direct BT A2DP callback driver |
| [minimp3](https://github.com/pschatzmann/minimp3) | latest | Lightweight zero-alloc MP3 decoder |
