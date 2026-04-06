# Eneby BT Bridge — Raspberry Pi Zero 2 W

Streams internet radio (MP3) to an IKEA ENEBY20 Bluetooth speaker via A2DP.
Same HTTP API as the ESP32 version — your Home Assistant config works unchanged.

---

## What you need

- Raspberry Pi Zero 2 W
- Micro SD card (8 GB+)
- USB power supply (5V/1A)
- IKEA ENEBY20 speaker (or any BT A2DP speaker)
- A computer to flash the SD card and SSH in

---

## Step 1: Flash Raspberry Pi OS Lite

1. Download and install [Raspberry Pi Imager](https://www.raspberrypi.com/software/)
2. Insert the SD card into your computer
3. In the Imager:
   - **OS:** Raspberry Pi OS Lite (32-bit) — no desktop needed
   - **Storage:** your SD card
   - Click the **gear icon** (⚙) or **Edit Settings** before writing:
     - **Hostname:** `eneby` (or whatever you prefer)
     - **Enable SSH:** Yes, use password authentication
     - **Username:** `pi`, **Password:** choose one
     - **WiFi:** enter your SSID and password, choose your country
   - Click **Write**

4. Insert the SD card into the Pi Zero 2 W and power it on
5. Wait ~60 seconds for first boot

---

## Step 2: SSH into the Pi

Find the Pi's IP address from your router's DHCP table, or try:

```bash
# From your computer (Windows/Mac/Linux):
ping eneby.local
```

Then SSH in:

```bash
ssh pi@eneby.local
# or: ssh pi@<ip-address>
```

---

## Step 3: Update the system

```bash
sudo apt update && sudo apt upgrade -y
```

---

## Step 4: Install required packages

```bash
sudo apt install -y git mpv pulseaudio pulseaudio-module-bluetooth python3-pip
```

> **Note:** `pulseaudio` and `pulseaudio-module-bluetooth` may already be
> installed on recent Pi OS images. This command ensures they are.

---

## Step 5: Set up Git and clone the repo from GitHub

### Option A: HTTPS (simplest — no SSH key needed)

```bash
git clone https://github.com/Teknosofen/EnebyBridge.git
```

If the repo is private, you'll need a Personal Access Token:

1. Go to https://github.com/settings/tokens
2. Click **Generate new token (classic)**
3. Give it a name (e.g. "pi-eneby"), select **repo** scope
4. Copy the token

Then clone with the token:

```bash
git clone https://<YOUR_TOKEN>@github.com/Teknosofen/EnebyBridge.git
```

### Option B: SSH key (recommended for frequent git operations)

Generate an SSH key on the Pi:

```bash
ssh-keygen -t ed25519 -C "pi-eneby"
# Press Enter for all prompts (no passphrase needed)
```

Display the public key:

```bash
cat ~/.ssh/id_ed25519.pub
```

Copy the output, then:

1. Go to https://github.com/settings/keys
2. Click **New SSH key**
3. Paste the key, give it a title (e.g. "Pi Zero 2 W")
4. Click **Add SSH key**

Now clone via SSH:

```bash
git clone git@github.com:Teknosofen/EnebyBridge.git
```

### Pulling updates later

```bash
cd ~/EnebyBridge
git pull
```

---

## Step 6: Install Python dependencies

```bash
cd ~/EnebyBridge/pi
pip3 install -r requirements.txt
```

> On newer Pi OS you may need `pip3 install --break-system-packages -r requirements.txt`
> or use a venv: `python3 -m venv venv && source venv/bin/activate && pip install -r requirements.txt`

---

## Step 7: Enable and start PulseAudio

PulseAudio needs to run as your user (not root) for Bluetooth audio:

```bash
# Enable PulseAudio to start on login
systemctl --user enable pulseaudio
systemctl --user start pulseaudio

# Verify it's running
pactl info
```

To make PulseAudio start even without a login session (needed for autostart):

```bash
loginctl enable-linger pi
```

---

## Step 8: Pair with ENEBY20

1. **Power on the ENEBY20** and put it in pairing mode (hold the BT button
   until the LED flashes)

2. On the Pi:

```bash
bluetoothctl
```

3. In the bluetoothctl prompt:

```
power on
agent on
default-agent
scan on
```

4. Wait until you see `ENEBY20` appear (something like `[NEW] Device AA:BB:CC:DD:EE:FF ENEBY20`). Note the MAC address.

5. Pair and trust:

```
pair AA:BB:CC:DD:EE:FF
trust AA:BB:CC:DD:EE:FF
connect AA:BB:CC:DD:EE:FF
```

6. You should see `Connection successful`. Type `quit` to exit.

7. Verify PulseAudio sees the BT sink:

```bash
pactl list short sinks
```

You should see a line like:
```
bluez_sink.AA_BB_CC_DD_EE_FF.a2dp_sink    module-bluez5-device.c ...
```

---

## Step 9: Test manually

```bash
# Test that audio plays through the ENEBY20:
mpv --no-video --audio-device=pulse http://ice1.somafm.com/groovesalad-128-mp3
```

You should hear music from the speaker. Press `q` to stop.

Now test the bridge service:

```bash
cd ~/EnebyBridge/pi
sudo python3 eneby_bridge.py
```

From another terminal (or your computer):

```bash
# Play
curl "http://eneby.local/play?url=http://ice1.somafm.com/groovesalad-128-mp3"

# Status
curl "http://eneby.local/status"

# Volume
curl "http://eneby.local/volume?level=50"

# Stop
curl "http://eneby.local/stop"
```

Press `Ctrl+C` to stop the manual test.

---

## Step 10: Set up autostart with systemd

```bash
# Copy the service file
sudo cp ~/EnebyBridge/pi/eneby-bridge.service /etc/systemd/system/

# Reload systemd, enable and start
sudo systemctl daemon-reload
sudo systemctl enable eneby-bridge
sudo systemctl start eneby-bridge

# Check it's running
sudo systemctl status eneby-bridge
```

The service will now **start automatically on every boot**.

### Useful systemd commands

```bash
# View logs
journalctl -u eneby-bridge -f

# Restart after code changes
sudo systemctl restart eneby-bridge

# Stop
sudo systemctl stop eneby-bridge

# Disable autostart
sudo systemctl disable eneby-bridge
```

---

## Step 11: Update Home Assistant

In your `home_assistant.yaml` / `configuration.yaml`, change the ESP32 IP
to the Pi's IP (or hostname):

```yaml
rest_command:
  eneby_play:
    url: "http://eneby.local/play"
    # ... rest stays the same
```

---

## Updating the code

When you push changes from your development machine:

```bash
# On the Pi:
cd ~/EnebyBridge
git pull
sudo systemctl restart eneby-bridge
```

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| `pactl: command not found` | `sudo apt install pulseaudio` |
| No BT sink in `pactl list short sinks` | Re-pair in `bluetoothctl`; restart PulseAudio: `systemctl --user restart pulseaudio` |
| Permission denied on port 80 | The systemd unit grants `CAP_NET_BIND_SERVICE`. For manual testing use `sudo` or set `PORT=8080` |
| ENEBY20 doesn't reconnect after reboot | Ensure `trust` was set in `bluetoothctl`; add `connect AA:BB:CC:DD:EE:FF` to a boot script if needed |
| `pip3 install` fails with "externally managed" | Use `--break-system-packages` flag or create a venv (see Step 6) |
| mpv can't find PulseAudio | Ensure `loginctl enable-linger pi` was run (Step 7) |

---

## Changing WiFi network

When moving the bridge to a different location with a different WiFi network:

### Before moving: add the new network via SSH

```bash
sudo nmcli connection add type wifi con-name "vacation-house" \
  ssid "OtherNetworkSSID" wifi-sec.key-mgmt wpa-psk \
  wifi-sec.psk "OtherNetworkPassword"
```

The Pi will automatically connect to whichever known network is available
at boot. You can add as many networks as you want.

List configured networks:

```bash
nmcli connection show
```

Remove a network:

```bash
sudo nmcli connection delete "vacation-house"
```

> **Note:** Older Pi OS (before Bookworm) uses `wpa_supplicant` instead of
> NetworkManager. In that case, edit `/etc/wpa_supplicant/wpa_supplicant.conf`
> and add a second `network={}` block.

### Emergency access: USB gadget mode

If the Pi is already at a new location and can't connect to any known
network, you can access it over USB:

1. On the Pi's SD card, add to `/boot/config.txt`:
   ```
   dtoverlay=dwc2
   ```
   And add `modules-load=dwc2,g_ether` to `/boot/cmdline.txt` (after `rootwait`).

2. Plug the Pi into your laptop via the **USB data port** (not the power port).
   It appears as a USB Ethernet device.

3. SSH in via `ssh pi@raspberrypi.local` and configure the new WiFi with
   `nmcli` as shown above.

> **Tip:** Enable USB gadget mode ahead of time so it's ready if you ever
> need emergency access.

---

## API Reference

Same as the ESP32 version:

| Endpoint | Description |
|---|---|
| `GET /play?url=<mp3-url>` | Start streaming |
| `GET /stop` | Stop playback |
| `GET /volume?level=<0-100>` | Set volume |
| `GET /status` | JSON: playing, url, volume, bt_sink |

---

## Project structure

```
EnebyBridge/
├── pi/                         ← Raspberry Pi version (this)
│   ├── eneby_bridge.py         # Main service (~120 lines)
│   ├── eneby-bridge.service    # systemd unit for autostart
│   ├── requirements.txt        # Python dependencies
│   └── README.md               # This file
├── src/                        ← ESP32 version
│   └── main.cpp
├── include/
│   └── config.h
├── platformio.ini
├── home_assistant.yaml
└── README.md
```
