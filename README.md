# Eneby BT Bridge — ESP32 WROOM

WiFi → Bluetooth A2DP bridge for IKEA Eneby 20 using a plain ESP32-WROOM
(no PSRAM required). Controlled via a simple HTTP API from Home Assistant.

## Architecture

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
> (~60-65% real PCM). A Raspberry Pi Zero 2 W with separate radios
> would solve this reliably.

## Hardware

- ESP32-WROOM-32 (any variant)
- USB cable for flashing
- No additional hardware needed

## Setup

### 1. Configure WiFi and Bluetooth

Edit `include/config.h`:

```cpp
#define WIFI_SSID      "your_wifi_ssid"
#define WIFI_PASS      "your_wifi_password"
#define BT_DEVICE_NAME "ENEBY20"   // check your speaker's BT name
```

### 2. First-time BT pairing

Power on the Eneby and put it in pairing mode (hold BT button until
flashing). The ESP32 will find and pair with it automatically on first boot.
After that it reconnects automatically whenever the speaker is powered on.

### 3. Build and flash

```bash
# In VSCode with PlatformIO extension:
# Click the Upload button (→) in the PlatformIO toolbar

# Or from terminal:
pio run --target upload
pio device monitor   # watch serial output
```

### 4. Find the ESP32 IP

Check your router's DHCP table, or watch the serial monitor — it prints the
IP on boot. Consider giving it a static IP in your router.

### 5. Test the API

```bash
# Play a stream
curl "http://<esp32-ip>/play?url=http://ice1.somafm.com/groovesalad-128-mp3"

# Check status
curl "http://<esp32-ip>/status"

# Set volume (0–100)
curl "http://<esp32-ip>/volume?level=60"

# Stop
curl "http://<esp32-ip>/stop"
```

### 6. Home Assistant

See `home_assistant.yaml` for:
- `rest_command` entries (play/stop/volume)
- Template `media_player` entity
- REST sensor for polling status
- Example automations and scripts

Replace `192.168.1.XXX` with your ESP32's IP throughout.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `hash_map_set` crash at boot | Ring buffer or BSS too large | Keep `PCM_RING_SIZE` ≤ 5120 |
| WiFi drops during streaming | Heap fragmentation (blk < 2 KB) | Proactive reconnect handles this automatically |
| BT won't connect | Eneby paired to another device | Forget the ESP32 on the Eneby and re-pair |
| Stream won't start | URL not MP3 | Only raw MP3 streams supported (no HLS/AAC) |
| No sound but playing | BT callback not firing | Check serial for `audio=STREAM` and `bt_cb > 0` |
| Intermittent audio (~60%) | ESP32 single-radio limitation | Normal — see Known Limitation above |

## Memory notes

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
