# Eneby BT Bridge — ESP32 WROOM

WiFi → Bluetooth A2DP bridge for IKEA Eneby 20 using a plain ESP32-WROOM
(no PSRAM required). Controlled via a simple HTTP API from Home Assistant.

## Architecture

```
Home Assistant
    │
    │  HTTP REST (play URL / stop / volume)
    ▼
ESP32-WROOM
    ├── pulls MP3 stream from URL
    ├── decodes with Helix MP3 decoder
    └── sends PCM via Bluetooth A2DP
            │
            ▼
        IKEA Eneby 20
```

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
| Heap allocation failed | Buffer too large | Reduce `HTTP_STREAM_BUFFER_SIZE` in config.h |
| Stuttering audio | WiFi interference | Confirm `WIFI_PS_NONE` is set (it is by default) |
| BT won't connect | Eneby paired to another device | Forget the ESP32 on the Eneby and re-pair |
| Stream won't start | URL not MP3 | Check content-type; only MP3 supported currently |

## Memory notes

The WROOM has 520KB internal RAM. After WiFi+BT stack the usable heap is
roughly 200KB. The audio buffer is set to 16KB — if you see heap errors,
reduce `HTTP_STREAM_BUFFER_SIZE` in `config.h` to 8KB.

Free heap is reported in `/status` → useful for diagnosing memory issues.
