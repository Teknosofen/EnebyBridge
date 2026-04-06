# Eneby BT Bridge

WiFi → Bluetooth A2DP bridge for IKEA Eneby 20. Streams internet radio
(MP3) to the speaker, controlled via a simple HTTP API from Home Assistant.

## ⚡ Raspberry Pi version (recommended)

The **Pi Zero 2 W version** in [`pi/`](pi/) is the recommended platform.
It uses separate WiFi and Bluetooth radios, giving reliable, uninterrupted
audio. See [pi/README.md](pi/README.md) for full setup instructions.

## ESP32 version (paused)

The original ESP32-WROOM version in [`src/`](src/) works but is limited by
the ESP32's single 2.4 GHz radio shared between WiFi and Bluetooth. After
extensive optimization (BLE memory release, non-blocking I/O, proactive
reconnect, coex tuning), it achieves ~60–65% real audio with intermittent
gaps. The ESP32 code is preserved for reference but active development has
moved to the Pi version.

## HTTP API (both versions)

```bash
# Play a stream
curl "http://<ip>/play?url=http://ice1.somafm.com/groovesalad-128-mp3"

# Check status
curl "http://<ip>/status"

# Set volume (0–100)
curl "http://<ip>/volume?level=60"

# Stop
curl "http://<ip>/stop"
```

## Home Assistant

See `home_assistant.yaml` for REST commands, template media_player,
REST sensor, and example automations. Replace the IP with your device's IP.

---

## ESP32 reference notes

<details>
<summary>Click to expand ESP32 architecture, setup, and troubleshooting</summary>

### Architecture

```
Home Assistant
    │
    │  HTTP REST (play / stop / volume / status)
    ▼
ESP32-WROOM
    ├── WiFiClient pulls MP3 stream over TCP
    ├── minimp3 decodes to PCM (BSS, zero heap alloc)
    ├── lock-free SPSC ring buffer (5 KB, BSS)
    └── BluetoothA2DPSource callback → SBC → BT radio
            │
            ▼
        IKEA Eneby 20
```

> **Known limitation:** The ESP32 shares a single 2.4 GHz radio between
> WiFi and Bluetooth. Heap fragmentation from lwIP packet buffers
> eventually starves the BT SBC encoder, causing intermittent audio
> (~60-65% real PCM).

### Hardware

- ESP32-WROOM-32 (any variant)
- USB cable for flashing
- No additional hardware needed

### Setup

1. Copy `include/config.h.example` to `include/config.h` and edit WiFi/BT settings
2. Power on the Eneby in pairing mode (hold BT button until LED flashes)
3. Build and flash: `pio run --target upload`
4. Monitor serial: `pio device monitor`

### Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `hash_map_set` crash at boot | Ring buffer or BSS too large | Keep `PCM_RING_SIZE` ≤ 5120 |
| WiFi drops during streaming | Heap fragmentation (blk < 2 KB) | Proactive reconnect handles this automatically |
| BT won't connect | Eneby paired to another device | Forget the ESP32 on the Eneby and re-pair |
| Stream won't start | URL not MP3 | Only raw MP3 streams supported (no HLS/AAC) |
| No sound but playing | BT callback not firing | Check serial for `audio=STREAM` and `bt_cb > 0` |
| Intermittent audio (~60%) | ESP32 single-radio limitation | Normal — fundamental hardware constraint |

### Memory notes

The WROOM has 520 KB internal SRAM. After WiFi + BT Classic init, ~30 KB
of heap remains. All decoder state is in BSS (zero heap allocation).

Key static allocations (BSS):
- Ring buffer: 5 KB
- MP3 input buffer: 4 KB
- minimp3 decoder + PCM output: ~12 KB

`esp_bt_controller_mem_release(ESP_BT_MODE_BLE)` frees ~10 KB of contiguous
DRAM that BLE would otherwise reserve.

Free heap and largest contiguous block are reported in `/status` and on
serial every 2–5 seconds.

</details>
