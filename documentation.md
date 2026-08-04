# Eneby BT Bridge — Documentation

## Overview

The Eneby BT Bridge streams internet radio (MP3 over HTTP) to an IKEA ENEBY20
Bluetooth speaker via A2DP. It exposes a simple HTTP API for playback control,
making it easy to integrate with Home Assistant or any HTTP client.

The **recommended platform** is a **Raspberry Pi Zero 2 W**, which has separate
WiFi and Bluetooth radios for gap-free audio. An ESP32-WROOM version also exists
(see [ESP32 variant](#esp32-variant-experimental) at the end of this document)
but is limited to ~60–65% real audio due to single-radio coexistence constraints.

---

## Quick reference

| What | Value |
|---|---|
| SSH login | `ssh hasseberg@eneby.local` |
| SSH username | **`hasseberg`** (*not* `eneby`) |
| Hostname | `eneby.local` |
| ENEBY20 BT MAC | `FC:58:FA:31:65:77` |
| Pi BT controller | `B8:27:EB:...` (Raspberry Pi OUI prefix) |
| Service control | `sudo systemctl {status\|restart\|stop} eneby-bridge` |
| Service logs | `journalctl -u eneby-bridge -f` |
| Audio backend | PulseAudio → `bluez_sink.FC_58_FA_31_65_77.a2dp_sink` |
| API port | 80 (`CAP_NET_BIND_SERVICE` granted by the systemd unit) |

> **Username vs hostname.** `eneby` is the **hostname**, `hasseberg` is the
> **SSH username**. Logging in as `eneby@eneby.local` fails with "Permission
> denied" — always use `hasseberg@eneby.local`.

> **Password.** Deliberately not recorded in this repository, which is public.
> It is set once in Raspberry Pi Imager at flash time and stored nowhere on the
> running Pi — keep it in a password manager or private note. If it is lost,
> recover access by editing `userconf.txt` on the SD card's boot partition, or
> re-flash. Switching to SSH-key login avoids needing it at all.

---

## Architecture (Raspberry Pi)

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
│            Raspberry Pi Zero 2 W                  │
│                                                   │
│  ┌─────────────┐    ┌───────────┐    ┌─────────┐ │
│  │ Flask       │──▶│ mpv       │──▶│ Pulse-  │ │
│  │ (HTTP API)  │    │ (decoder) │    │ Audio   │ │
│  └─────────────┘    └───────────┘    └────┬────┘ │
│                                           │      │
│  ┌─────────────┐                          │      │
│  │ systemd     │   BlueZ auto-reconnect   │      │
│  │ (autostart) │                          │      │
│  └─────────────┘                          │      │
└───────────────────────────────────────────┼──────┘
                                            │ BT A2DP
                                            ▼
                                    ┌──────────────┐
                                    │  IKEA ENEBY   │
                                    │  20 Speaker   │
                                    └──────────────┘
```

### How it works

1. **Flask** receives an HTTP request with a stream URL.
2. It spawns **mpv** as a subprocess to decode and play the MP3 stream.
3. mpv outputs audio via **PulseAudio**, which routes it to the paired
   Bluetooth speaker over A2DP.
4. **BlueZ** manages the Bluetooth connection. Once the speaker is paired
   and trusted, it reconnects automatically on boot.
5. A **systemd** unit starts the bridge service on boot (port 80).

---

## Features

| Feature | Description |
|---|---|
| **HTTP-controlled playback** | Start/stop MP3 streams via simple GET requests |
| **Volume control** | Set volume 0–100 over HTTP via PulseAudio |
| **Status endpoint** | JSON status with playing state, URL, volume, BT sink |
| **Home Assistant integration** | REST commands, media_player, station selector, automations |
| **BT auto-reconnect** | BlueZ reconnects to trusted speaker on boot (not always reliable — see [Bluetooth pairing and reconnection](#bluetooth-pairing-and-reconnection)) |
| **mDNS discovery** | Accessible via `eneby.local` — works on any network |
| **Autostart on boot** | systemd service starts automatically, no login needed |
| **Gap-free audio** | Separate WiFi + BT radios = no coexistence issues |

---

## Hardware requirements

- Raspberry Pi Zero 2 W
- Micro SD card (8 GB+)
- USB power supply (5V / 1A)
- IKEA ENEBY20 (or any Bluetooth A2DP speaker)

---

## Setup

For step-by-step setup instructions (flashing the SD card, SSH, pairing,
installing the service), see **[pi/README.md](pi/README.md)**.

This document covers the reference material instead: API, integration,
[Bluetooth pairing and reconnection](#bluetooth-pairing-and-reconnection),
[network changes](#network-changes), and [troubleshooting](#troubleshooting).

---

## HTTP API

All endpoints use **GET** requests. Replace `eneby.local` with your Pi's IP
if mDNS isn't available.

### `GET /play?url=<stream-url>`

Start streaming an MP3 URL to the speaker.

```bash
curl "http://eneby.local/play?url=http://ice1.somafm.com/groovesalad-128-mp3"
```

**Responses:**
- `200 OK` — Playback started
- `400 Missing 'url' parameter` or `Invalid URL scheme`

### `GET /stop`

Stop the current stream.

```bash
curl "http://eneby.local/stop"
```

### `GET /volume?level=<0-100>`

Set the playback volume (0 = mute, 100 = max).

```bash
curl "http://eneby.local/volume?level=60"
```

### `GET /status`

Returns JSON with the current state.

```bash
curl "http://eneby.local/status"
```

**Example response:**

```json
{
  "playing": true,
  "url": "http://ice1.somafm.com/groovesalad-128-mp3",
  "volume": 60,
  "bt_sink": "bluez_sink.FC_58_FA_31_65_77.a2dp_sink"
}
```

---

## Usage examples

### Quick test from the command line

```bash
# Play Sveriges Radio P3
curl "http://eneby.local/play?url=https://sverigesradio.se/topsy/direkt/164-hi-mp3"

# Check status
curl "http://eneby.local/status"

# Set volume to 50%
curl "http://eneby.local/volume?level=50"

# Stop playback
curl "http://eneby.local/stop"
```

### From a web browser

Open in any browser:
```
http://eneby.local/play?url=https://sverigesradio.se/topsy/direkt/164-hi-mp3
```

### Swedish radio streams

| Station | URL |
|---|---|
| Sveriges Radio P1 | `https://sverigesradio.se/topsy/direkt/132-hi-mp3` |
| Sveriges Radio P2 | `https://sverigesradio.se/topsy/direkt/2562-hi-mp3` |
| Sveriges Radio P3 | `https://sverigesradio.se/topsy/direkt/164-hi-mp3` |
| P4 Stockholm | `https://sverigesradio.se/topsy/direkt/701-hi-mp3` |
| P4 Göteborg | `https://sverigesradio.se/topsy/direkt/212-hi-mp3` |
| RIX FM | `https://live-bauerse.sharp-stream.com/rixfm_mp3_128` |
| Lugna Favoriter | `https://live-bauerse.sharp-stream.com/lugnafavoriter_mp3_128` |

---

## Home Assistant integration

The file `home_assistant.yaml` contains everything needed to control the
bridge from Home Assistant. Copy its contents into your HA `configuration.yaml`
(or split into separate files using `!include`).

### What's included

The HA configuration provides:

1. **REST commands** — `eneby_play`, `eneby_stop`, `eneby_volume`
2. **Template media_player** — a proper player card with play/stop/volume
3. **REST sensor** — polls `/status` every 10 seconds
4. **Radio station selector** — dropdown with Swedish stations
5. **Automations** — auto-play on station change, stop on away
6. **Script** — play any URL from HA UI or automations

### Step-by-step: Adding to Home Assistant

1. **Copy the YAML** — Open `home_assistant.yaml` from this repo and copy
   its contents.

2. **Paste into HA** — Add to your `configuration.yaml`. If you already have
   sections like `rest_command:`, `automation:`, etc., merge the entries
   under the existing keys rather than duplicating the key.

3. **Check the config** — In HA, go to **Developer Tools → YAML** and click
   **Check Configuration**. Fix any errors.

4. **Restart HA** — Click **Restart** (or run from the command line:
   `ha core restart`).

5. **Verify the sensor** — Go to **Developer Tools → States** and search for
   `sensor.eneby_status`. It should show `idle` or `playing`.

### Using the station selector

After restarting HA, you'll have an `input_select.eneby_radio_station` entity.
Add it to a dashboard:

1. Go to your dashboard → **Edit** → **Add card**
2. Choose **Entities** card
3. Add `input_select.eneby_radio_station`
4. Optionally add `media_player.eneby_speaker` for volume control

Select a station from the dropdown to start playback. Select **"Av"** to stop.

Available stations:
- P1, P2, P3
- P4 Stockholm, P4 Göteborg
- RIX FM, Lugna Favoriter

### REST commands from Developer Tools

You can also call the REST commands directly:

```yaml
service: rest_command.eneby_play
data:
  url: "https://sverigesradio.se/topsy/direkt/164-hi-mp3"
```

### Script: play any URL

```yaml
service: script.play_on_eneby
data:
  url: "https://sverigesradio.se/topsy/direkt/164-hi-mp3"
```

### Automation: stop on away

The included automation stops playback and resets the station selector to "Av"
when a person entity leaves home. Edit the trigger entity to match your setup:

```yaml
trigger:
  - platform: state
    entity_id: person.your_name    # ← change this
    to: "not_home"
```

---

## Boot sequence

After powering on the Pi, the following happens automatically:

1. WiFi connects (configured in Pi OS)
2. PulseAudio starts (user service, enabled via `loginctl enable-linger`)
3. BlueZ reconnects to the trusted ENEBY20 speaker (~10–30 seconds)
4. systemd starts `eneby-bridge.service` (Flask on port 80)
5. The bridge is ready to accept HTTP requests
6. mDNS advertises `eneby.local` via avahi

> **Step 3 is the unreliable one.** `trust` normally makes BlueZ reconnect on
> boot, but in practice it sometimes doesn't. If the speaker fails to come back
> after a reboot, add a boot-time `bluetoothctl connect FC:58:FA:31:65:77` from
> a small systemd unit or startup script, delayed until after Bluetooth and
> PulseAudio are up.

---

## Bluetooth pairing and reconnection

The Bluetooth link is the single most common source of real-world failures.
Understanding two behaviours explains almost every symptom.

### The pairing key can be lost

The ENEBY20 stores a pairing key per device, and it **discards the Pi's key**
if the speaker is paired to something else in the meantime, or after long
periods unused (e.g. seasonal use). Once the key is gone, the Pi and the
speaker no longer trust each other even though both still list the pairing.

Symptoms of a lost key:

| Signal | What you see |
|---|---|
| `/status` | `"bt_sink": "not found"` |
| `pactl list short sinks` | only `auto_null` — no `bluez_sink.*` |
| ENEBY20 LED | **flashing fast** (back in pairing mode) |
| `bluetoothctl connect` | `Failed to connect: org.bluez.Error.Failed br-connection-key-missing` |
| Audible result | playback "succeeds" but audio goes nowhere — silence |

**A plain `connect` cannot fix this.** The stale pairing must be deleted with
`remove` and a fresh key negotiated with `pair`. The full command sequence is
in **[pi/README.md → Reconnecting the speaker](pi/README.md)**; in short:
`power on` → `remove <MAC>` → `agent on` → `default-agent` → `scan on` → put the
speaker in pairing mode → `scan off` → `pair` → `trust` → `connect`.

`trust` matters as much as `pair`: it is what authorises future automatic
reconnection. Pairing without trusting produces a link that works now and
fails after the next reboot.

### The speaker accepts only one device at a time

A2DP speakers are single-sink. A nearby phone that has the ENEBY20 paired can
**silently steal it** — including mid-re-pairing, which makes `pair` or
`connect` fail for no visible reason. Turn Bluetooth off on nearby phones when
re-pairing, and suspect a phone first whenever the speaker is unexpectedly
unavailable.

Two related timing traps:

- **`pair` reports `Device ... not available`** — the speaker hasn't been
  rediscovered yet. `scan on`, confirm the LED is flashing fast, and wait for
  the `[NEW] Device FC:58:FA:31:65:77 ENEBY20` line before pairing.
- **Pairing mode times out** — the ENEBY20 leaves pairing mode on its own.
  Power-cycle it and hold the BT button again.

### Verifying a good connection

```bash
pactl list short sinks    # must list bluez_sink.FC_58_FA_31_65_77.a2dp_sink
curl "http://eneby.local/status"                       # bt_sink must not be "not found"
curl "http://eneby.local/play?url=https://sverigesradio.se/topsy/direkt/164-hi-mp3"
```

If the sink is missing but Bluetooth reports connected, restart PulseAudio and
re-check: `systemctl --user restart pulseaudio`.

---

## Network changes

The bridge is designed to move between locations. Pi OS connects to whichever
known network is available at boot, so add networks before relocating:

```bash
sudo nmcli connection add type wifi con-name "vacation-house" \
  ssid "OtherNetworkSSID" wifi-sec.key-mgmt wpa-psk \
  wifi-sec.psk "OtherNetworkPassword"

nmcli connection show                              # list configured networks
sudo nmcli connection delete "vacation-house"      # remove one
```

Pre-Bookworm Pi OS uses `wpa_supplicant` instead of NetworkManager — add a
second `network={}` block to `/etc/wpa_supplicant/wpa_supplicant.conf`.

### Recovering a Pi on an unknown network

If the Pi is already somewhere it can't connect and you can't SSH in, two
approaches work:

1. **Impersonate a known network** (learned the hard way) — stand up a phone
   hotspot or spare router using the **exact same SSID and password** as a
   network the Pi already knows. It connects believing it's the known network,
   and you can then SSH in and add the real one with `nmcli`. The Pi Zero W is
   **2.4 GHz only**, so the temporary network must broadcast on 2.4 GHz.
2. **USB gadget mode** — `dtoverlay=dwc2` in `/boot/config.txt` plus
   `modules-load=dwc2,g_ether` in `/boot/cmdline.txt`, then connect the Pi's
   USB *data* port to a laptop and SSH over USB Ethernet. Worth enabling ahead
   of time so it's ready when needed.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `bt_sink: "not found"` in `/status`, only `auto_null` in `pactl list short sinks` | BT sink gone — usually a lost pairing key | Try `bluetoothctl connect FC:58:FA:31:65:77`; if it returns `br-connection-key-missing` you must `remove` and re-pair — see [Bluetooth pairing and reconnection](#bluetooth-pairing-and-reconnection) |
| `connect` fails with `br-connection-key-missing` | Pairing key discarded by the speaker | Plain `connect` cannot fix this — `remove <MAC>`, then `pair` / `trust` / `connect` afresh |
| `pair` fails with `Device ... not available` | Speaker not currently discoverable | `scan on`, put the speaker in pairing mode (LED flashing fast), wait for the `[NEW] Device` line, then `pair` |
| Speaker unavailable / pairing keeps failing | A nearby phone has claimed it — A2DP is single-device | Turn off Bluetooth on nearby phones, then retry |
| Service crash-loops (status=217) | Wrong paths in service file | `git pull`, re-copy service file, `systemctl daemon-reload` |
| No sound but "playing" | Speaker disconnected during idle | `bluetoothctl connect FC:58:FA:31:65:77` — if that fails with `br-connection-key-missing`, re-pair |
| Doesn't reconnect after reboot | `trust` missing, or BlueZ boot reconnect unreliable | Confirm `trust <MAC>` was set; otherwise add a boot-time `connect` (see [Boot sequence](#boot-sequence)) |
| BT won't connect | `rfkill` blocking Bluetooth | `sudo rfkill unblock bluetooth` (persist via `/etc/rc.local`) |
| `br-connection-busy` | Service repeatedly trying to connect | `sudo systemctl stop eneby-bridge`, disconnect, reconnect, restart service |
| SSH "Permission denied" | Wrong username | Use `hasseberg@eneby.local` — `eneby` is the hostname, not the username |
| PulseAudio connection refused | PulseAudio not running for user | `systemctl --user start pulseaudio` and ensure `loginctl enable-linger` is set |
| `eneby.local` not resolving | avahi not running or mDNS blocked | `sudo systemctl start avahi-daemon` — use IP address as fallback |
| Stream won't play | URL not serving MP3 | Verify with `mpv <url>` directly on the Pi |
| Port 80 permission denied | Missing capability in service file | Service needs `AmbientCapabilities=CAP_NET_BIND_SERVICE` |

---

## Project structure

```
EnebyBridge/
├── README.md               # Quick-start guide
├── documentation.md        # This file
├── home_assistant.yaml     # HA integration (REST commands, media_player, stations)
├── pi/
│   ├── README.md           # Step-by-step Pi setup guide
│   ├── eneby_bridge.py     # Flask HTTP API + mpv subprocess
│   ├── eneby-bridge.service # systemd unit file
│   └── requirements.txt    # Python dependencies (flask)
├── platformio.ini          # ESP32 build config (see ESP32 variant below)
├── include/
│   ├── config.h.example    # ESP32 config template
│   └── config.h            # ESP32 local settings (git-ignored)
└── src/
    └── main.cpp            # ESP32 firmware (see ESP32 variant below)
```

---

## Dependencies (Pi version)

| Component | Purpose |
|---|---|
| Python 3 + Flask | HTTP API server |
| mpv | Audio decoder and player |
| PulseAudio + pulseaudio-module-bluetooth | Audio routing to BT speaker |
| BlueZ (bluetoothctl) | Bluetooth pairing and auto-reconnect |
| avahi-daemon | mDNS (`eneby.local` hostname resolution) |

---

---

# ESP32 variant (experimental)

> **Note:** The ESP32 version works but is limited to ~60–65% real audio due
> to the single 2.4 GHz radio shared between WiFi and Bluetooth. The Pi
> version is recommended for production use.

The ESP32-WROOM version runs the entire pipeline on a single microcontroller:
HTTP server, MP3 decoding (minimp3), and Bluetooth A2DP source — all in
520 KB of RAM with no PSRAM.

## ESP32 Architecture

```
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
                                    │  IKEA ENEBY   │
                                    │  20 Speaker   │
                                    └──────────────┘
```

## ESP32 Audio pipeline

1. **WiFiClient** — Raw TCP to the MP3 stream. Non-blocking header parsing,
   supports HTTP redirects (up to 3).
2. **minimp3** — Decodes MP3 to PCM (44100 Hz, 16-bit, stereo). All state
   in BSS (zero heap allocation).
3. **BluetoothA2DPSource** — SPSC ring buffer (5 KB, BSS) bridges the decoder
   (core 1) and the BT data callback (core 0).

## ESP32 WiFi / Bluetooth coexistence

The single-radio coexistence challenge is addressed with:

- `ESP_COEX_PREFER_BT` during pairing, `ESP_COEX_PREFER_BALANCE` for streaming
- 20 MHz WiFi bandwidth (`WIFI_BW_HT20`)
- BLE memory release (~10 KB freed)
- Reduced A2DP buffers (2 × 512 B) and lwIP TCP buffers (2920 B window)
- Proactive reconnect based on heap fragmentation monitoring
- Automatic WiFi reconnection (5-second polling)

> **Known limitation:** lwIP packet buffer allocation fragments the heap
> over 10–30 seconds, causing intermittent audio (~60–65% real PCM).

## ESP32 Configuration

Copy `include/config.h.example` to `include/config.h` and edit:

```cpp
#define WIFI_SSID       "your_wifi_ssid"
#define WIFI_PASS       "your_wifi_password"
#define BT_DEVICE_NAME  "ENEBY20"
#define HTTP_PORT       80
```

## ESP32 Build and flash

```bash
pio run --target upload
pio device monitor
```

## ESP32 Dependencies

| Library | Version | Purpose |
|---|---|---|
| [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) | v1.0.0 | Dependency of ESP32-A2DP |
| [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) | v1.8.4 | BT A2DP source driver |
| [minimp3](https://github.com/pschatzmann/minimp3) | latest | Zero-alloc MP3 decoder |

## ESP32 Memory budget

| Component | Size | Location |
|---|---|---|
| PCM ring buffer | 5 KB | BSS |
| MP3 input buffer | 4 KB | BSS |
| minimp3 decoder context | ~7 KB | BSS |
| PCM output buffer | ~4.5 KB | BSS |
| A2DP buffers | 2 × 512 B = 1 KB | Heap (BT stack) |
| lwIP TCP buffers | ~3 KB window | Heap (lwIP) |
| WebServer | ~2 KB | Heap |

## ESP32 Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `hash_map_set` crash at boot | BSS too large for heap | Keep `PCM_RING_SIZE` ≤ 5120 |
| Stuttering / intermittent audio | Single-radio coex limitation | Expected (~60-65%); use Pi version instead |
| WiFi drops during streaming | Heap fragmentation (blk < 2 KB) | Proactive reconnect handles this |
| BT won't connect | Speaker paired to another device | Forget previous pairings and re-pair |
| No sound but "playing" | BT callback not firing | Check serial for `audio=STREAM` |
