#!/usr/bin/env python3
"""Eneby BT Bridge — Raspberry Pi Zero 2 W version.

Streams internet radio (MP3) to an IKEA ENEBY20 Bluetooth speaker.
Exposes the same HTTP API as the ESP32 version so Home Assistant
configuration works unchanged.

Requires: Flask, mpv, PulseAudio, BlueZ (paired with ENEBY20).
"""

import os
import signal
import subprocess
from flask import Flask, request, jsonify

app = Flask(__name__)

# ── State ──────────────────────────────────────────────────────────────────
_player_proc = None   # mpv subprocess
_current_url = ""
_current_volume = 70  # 0–100

BT_SINK_NAME = os.environ.get("BT_SINK", "")  # auto-detected if empty


def _get_bt_sink():
    """Find the PulseAudio sink name for the paired BT speaker."""
    global BT_SINK_NAME
    if BT_SINK_NAME:
        return BT_SINK_NAME
    try:
        out = subprocess.check_output(
            ["pactl", "list", "short", "sinks"], text=True
        )
        for line in out.strip().splitlines():
            # bluez sinks look like: bluez_sink.XX_XX_XX_XX_XX_XX.a2dp_sink
            if "bluez" in line.lower():
                BT_SINK_NAME = line.split("\t")[1]
                return BT_SINK_NAME
    except (subprocess.CalledProcessError, IndexError):
        pass
    return None


def _set_volume(level):
    """Set PulseAudio sink volume (0–100)."""
    global _current_volume
    _current_volume = max(0, min(100, level))
    sink = _get_bt_sink()
    if sink:
        subprocess.run(
            ["pactl", "set-sink-volume", sink, f"{_current_volume}%"],
            check=False,
        )


def _stop_player():
    """Stop the current mpv process if running."""
    global _player_proc, _current_url
    if _player_proc and _player_proc.poll() is None:
        _player_proc.terminate()
        try:
            _player_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _player_proc.kill()
    _player_proc = None
    _current_url = ""


def _start_player(url):
    """Start mpv streaming the given URL to the BT speaker."""
    global _player_proc, _current_url
    _stop_player()

    sink = _get_bt_sink()
    env = os.environ.copy()
    if sink:
        env["PULSE_SINK"] = sink

    _player_proc = subprocess.Popen(
        [
            "mpv",
            "--no-video",
            "--no-terminal",
            "--audio-device=pulse",
            f"--volume={_current_volume}",
            url,
        ],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    _current_url = url


# ── HTTP API ───────────────────────────────────────────────────────────────

@app.route("/play")
def play():
    url = request.args.get("url", "").strip()
    if not url:
        return "Missing 'url' parameter", 400
    if not url.startswith(("http://", "https://")):
        return "Invalid URL scheme", 400
    _start_player(url)
    return f"Playing {url}\n"


@app.route("/stop")
def stop():
    _stop_player()
    return "Stopped\n"


@app.route("/volume")
def volume():
    level = request.args.get("level", "")
    if not level.isdigit():
        return "Missing or invalid 'level' parameter (0-100)", 400
    _set_volume(int(level))
    return f"Volume {_current_volume}\n"


@app.route("/status")
def status():
    playing = _player_proc is not None and _player_proc.poll() is None
    return jsonify(
        playing=playing,
        url=_current_url if playing else "",
        volume=_current_volume,
        bt_sink=_get_bt_sink() or "not found",
    )


# ── Graceful shutdown ──────────────────────────────────────────────────────

def _shutdown(signum, frame):
    _stop_player()
    raise SystemExit(0)

signal.signal(signal.SIGTERM, _shutdown)
signal.signal(signal.SIGINT, _shutdown)


if __name__ == "__main__":
    print(f"Eneby Bridge starting on port {os.environ.get('PORT', 80)}")
    port = int(os.environ.get("PORT", 80))
    app.run(host="0.0.0.0", port=port)
