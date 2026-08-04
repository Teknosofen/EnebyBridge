# Eneby BT Bridge

Streams internet radio (MP3) to an IKEA ENEBY20 Bluetooth speaker via A2DP,
controlled through a simple HTTP API from Home Assistant.

This project has two implementations:

- **Raspberry Pi Zero 2 W (recommended, actively maintained)** — separate WiFi
  and Bluetooth radios give reliable, uninterrupted audio. Documented below.
- **ESP32-WROOM (paused, reference only)** — works but is limited by the
  ESP32's single 2.4 GHz radio shared between WiFi and BT (~60–65% audio with
  gaps). Documented in [../README.md](../README.md) and
  [../documentation.md](../documentation.md).

Both expose the same HTTP API, so your Home Assistant config works with either.

**This file covers the Raspberry Pi version only.**

---

## Quick reference (once installed)

| What | Value |
|---|---|
| SSH login | `ssh hasseberg@eneby.local` (username **hasseberg**, *not* `eneby`) |
| SSH username | **`hasseberg`** |
| SSH password | *(not stored here — see your password manager / private notes; set in Raspberry Pi Imager at flash time)* |
| Hostname | `eneby.local` |
| ENEBY20 BT MAC | `FC:58:FA:31:65:77` |
| Pi BT controller | `B8:27:EB:...` (Raspberry Pi prefix) |
| Service | `sudo systemctl {status\|restart} eneby-bridge` |
| Audio backend | PulseAudio → `bluez_sink.FC_58_FA_31_65_77.a2dp_sink` |

> **Username vs hostname:** `eneby` is the **hostname**, `hasseberg` is the
> **SSH username**. Logging in as `eneby@eneby.local` fails with "Permission
> denied" — always use `hasseberg@eneby.local`.
>
> **Password:** deliberately not written in this file because the repository is
> public. Keep it in a password manager or a private note. If it's lost, reset
> access by editing the SD card (`userconf.txt` on the boot partition) or
> re-flash — or switch to SSH-key login (Step 5, Option B) to avoid needing a
> password at all.

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
     - **Username:** `hasseberg`, **Password:** choose one — **write it down!**
     - **WiFi:** enter your SSID and password, choose your country
       (tip: you can add a second network later — see "Changing WiFi network")
   - Click **Write**

4. Insert the SD card into the Pi Zero 2 W and power it on
5. Wait ~60 seconds for first boot

> **Save your credentials:** The username (`hasseberg`) and password are set
> here and nowhere else. Store them in a password manager or notes — they are
> not recoverable from the running Pi without physical SD-card access. (Losing
> them means re-flashing or editing the SD card to reset access.)

---

## Step 2: SSH into the Pi

Find the Pi's IP address from your router's DHCP table, or try:

```bash
# From your computer (Windows/Mac/Linux):
ping eneby.local
```

Then SSH in:

```bash
ssh hasseberg@eneby.local
# or: ssh hasseberg@<ip-address>
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
3. Give it a name (e.g. "hasseberg-eneby"), select **repo** scope
4. Copy the token

Then clone with the token:

```bash
git clone https://<YOUR_TOKEN>@github.com/Teknosofen/EnebyBridge.git
```

### Option B: SSH key (recommended for frequent git operations)

Generate an SSH key on the Pi:

```bash
ssh-keygen -t ed25519 -C "hasseberg-eneby"
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
loginctl enable-linger hasseberg
```

---

## Step 8: Pair with ENEBY20

1. **Power on the ENEBY20** and put it in pairing mode (hold the BT button
   until the LED flashes)

2. Unblock Bluetooth (needed on fresh Pi OS installs):

```bash
sudo rfkill unblock bluetooth
```

   To make this persist across reboots, create `/etc/rc.local`:

```bash
sudo tee /etc/rc.local << 'EOF'
#!/bin/sh
rfkill unblock bluetooth
exit 0
EOF
sudo chmod +x /etc/rc.local
```

3. On the Pi:

```bash
bluetoothctl
```

4. In the bluetoothctl prompt:

```
power on
agent on
default-agent
scan on
```

5. Wait until you see `ENEBY20` appear (something like `[NEW] Device FC:58:FA:31:65:77 ENEBY20`).
   You'll see many other devices scroll by — ignore them and wait for ENEBY20.

6. **Stop scanning and pair/trust/connect** (don't quit before this!):

```
scan off
pair FC:58:FA:31:65:77
trust FC:58:FA:31:65:77
connect FC:58:FA:31:65:77
```

7. You should see `Connection successful`. Type `quit` to exit.

8. Verify PulseAudio sees the BT sink:

```bash
pactl list short sinks
```

You should see a line like:
```
bluez_sink.FC_58_FA_31_65_77.a2dp_sink    module-bluez5-device.c ...
```

If you only see `auto_null`, restart PulseAudio and check again:

```bash
systemctl --user restart pulseaudio
pactl list short sinks
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
sudo /home/hasseberg/EnebyBridge/pi/venv/bin/python3 eneby_bridge.py
```

> **Note:** `sudo` is needed for port 80. Using the venv's Python ensures
> Flask is available. The systemd service handles this automatically.

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

## Step 11: Add to Home Assistant

The file `home_assistant.yaml` in this repo contains everything you need:
REST commands, a media player entity, a radio station selector (P1, P2, P3,
P4 Stockholm, P4 Göteborg, RIX FM, Lugna Favoriter), and automations.

### How to add it

1. **Open your HA config file.** Use the **File Editor** add-on
   (Settings → Add-ons → File Editor) or the **Studio Code Server** add-on.
   The file is at `/config/configuration.yaml`.

2. **Copy and paste** the contents of `home_assistant.yaml` from this repo
   at the bottom of your `configuration.yaml`.

   > **Important:** If you already have sections like `rest_command:`,
   > `automation:`, `sensor:`, or `input_select:`, **merge** the entries
   > under the existing key — don't add a duplicate key.

3. **Check the config:** In HA, go to **Settings → System** → top-right
   menu (⋮) → **Check Configuration**.

4. **Restart HA:** Settings → System → **Restart**.

5. **Verify:** Go to **Developer Tools → States** and search for
   `sensor.eneby_status`. It should show `idle` or `playing`.

### Add the station selector to a dashboard

1. Go to your dashboard → **Edit** → **Add card**
2. Choose **Entities** card
3. Add `input_select.eneby_radio_station`
4. Optionally add `media_player.eneby_speaker` for volume control

Select a station from the dropdown to start playback. Select **"Av"** to stop.

### Test from Developer Tools

```yaml
service: rest_command.eneby_play
data:
  url: "https://sverigesradio.se/topsy/direkt/164-hi-mp3"
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
| SSH "Permission denied" | Username is `hasseberg`, not `eneby` (that's the hostname). Then check the password. |
| `pactl: command not found` | `sudo apt install pulseaudio` |
| `/status` shows `bt_sink: "not found"` and `pactl list short sinks` shows only `auto_null` | The BT sink is gone. See "Reconnecting the speaker" below. |
| No BT sink in `pactl list short sinks` | Re-pair in `bluetoothctl`; restart PulseAudio: `systemctl --user restart pulseaudio` |
| `connect` fails with `br-connection-key-missing` | Pairing key lost — you must `remove` and re-pair. See "Reconnecting the speaker". |
| `pair` fails with `Device ... not available` | Device not currently visible — run `scan on` and wait for ENEBY20 first. |
| Permission denied on port 80 | The systemd unit grants `CAP_NET_BIND_SERVICE`. For manual testing use `sudo` or set `PORT=8080` |
| ENEBY20 doesn't reconnect after reboot | Ensure `trust` was set in `bluetoothctl`; add a boot-time `connect` (see below) |
| `pip3 install` fails with "externally managed" | Use `--break-system-packages` flag or create a venv (see Step 6) |
| mpv can't find PulseAudio | Ensure `loginctl enable-linger hasseberg` was run (Step 7) |

---

## Reconnecting the speaker (after a break, power loss, or `br-connection-key-missing`)

**The most common real-world issue.** After the bridge has been unused for a
while (e.g. seasonal use), or if the ENEBY20 was paired to another device in
the meantime, the Bluetooth pairing key is lost. Symptoms:

- `/status` returns `"bt_sink": "not found"` (audio plays into nothing → silence)
- `pactl list short sinks` shows only `auto_null`
- The ENEBY20 LED **flashes fast** (it's in pairing mode, having "forgotten" the Pi)
- `bluetoothctl connect` fails with
  **`Failed to connect: org.bluez.Error.Failed br-connection-key-missing`**

### Fix: remove the stale pairing and re-pair from scratch

A plain `connect` will NOT work when the key is missing — you must remove and
re-pair. On the Pi:

```bash
bluetoothctl
```

In the prompt:

```
power on
remove FC:58:FA:31:65:77      # delete the stale/dead pairing
agent on
default-agent
scan on
```

Now **put the ENEBY20 in pairing mode** (hold the BT button until the LED
flashes fast). Wait until you see:

```
[NEW] Device FC:58:FA:31:65:77 ENEBY20
```

Then:

```
scan off
pair FC:58:FA:31:65:77        # creates a fresh key — fixes the error
trust FC:58:FA:31:65:77       # enables automatic reconnection in future
connect FC:58:FA:31:65:77
```

You should see `Pairing successful` then `Connection successful`, and the
prompt changes to `[ENEBY20]>`. Type `quit` to exit.

### Verify and test

```bash
pactl list short sinks        # should now show bluez_sink.FC_58_FA_31_65_77.a2dp_sink
curl "http://eneby.local/play?url=https://sverigesradio.se/topsy/direkt/164-hi-mp3"
```

### Common gotchas during re-pairing

- **`pair` says "Device not available"** → it hasn't been rediscovered yet.
  Run `scan on`, make sure the speaker is in pairing mode (flashing fast), and
  wait for the `ENEBY20` line before `pair`.
- **A phone steals the speaker** → BT speakers connect to one device at a time.
  Turn off Bluetooth on nearby phones during re-pairing, or a phone may
  auto-connect and block the Pi.
- **Speaker dropped out of pairing mode** → it times out. Power-cycle it and
  hold the BT button again to re-enter pairing mode.

### Making reconnection survive reboots

`trust` should let the Pi auto-reconnect on boot, but this isn't always
reliable. If the speaker doesn't come back after a reboot, add a boot-time
connect — a small systemd service or a startup-script line that runs:

```bash
bluetoothctl connect FC:58:FA:31:65:77
```

a short delay after boot (after Bluetooth and PulseAudio are up).

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

> **Tip (learned the hard way):** If the Pi is already at a location with an
> unknown network and you can't SSH in, create a temporary WiFi network (a
> phone hotspot or spare router) using the **exact same SSID and password** as
> a network the Pi already knows. The Pi connects thinking it's the known
> network, and you can then SSH in and add the real local network with `nmcli`.
> (Pi Zero W is 2.4 GHz only — make sure the temporary network broadcasts on
> 2.4 GHz.)

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

3. SSH in via `ssh hasseberg@raspberrypi.local` and configure the new WiFi with
   `nmcli` as shown above.

> **Tip:** Enable USB gadget mode ahead of time so it's ready if you ever
> need emergency access.

---

## API Reference

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
├── src/                        ← ESP32 version (paused, reference only)
│   └── main.cpp
├── include/
│   └── config.h
├── platformio.ini
├── home_assistant.yaml
├── documentation.md            # Reference: API, BT reconnection, troubleshooting
└── README.md                   # Project overview + ESP32 notes
```

---

## ESP32 version

The ESP32-WROOM implementation is **paused and kept for reference only**. It is
documented outside this file:

- [../README.md](../README.md) — architecture, hardware, setup, troubleshooting,
  memory notes
- [../documentation.md](../documentation.md) — full reference: audio pipeline,
  WiFi/BT coexistence tuning, memory budget, dependency versions

Use the Pi version above unless you specifically need the ESP32 build.
