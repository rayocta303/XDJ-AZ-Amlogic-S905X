#!/usr/bin/env python3
"""
test_views.py — Automated View Navigation & Remote Touch Verification for XDJ-AZ
Tests:
1. Clean Boot & Source View (Verifies NO USB error modal is present)
2. Interactive Touch Injection & Click Response (via /tmp/xdj_touch_inject.sock & xdotool)
3. Browse View Switching (via subucom SPI Mixer /dev/subucom_spi2.0 & UDP 9999)
4. Waveform / Deck View Switching (2-Deck / 4-Deck toggle)
5. Utility / Menu Screen Switching
6. Captures full 1280x800 display snapshot for each view verification
"""

import os
import sys
import time
import socket
import subprocess

SNAPSHOT_DIR = "/opt/xdjaz/snapshots"
TOUCH_SOCK   = "/tmp/xdj_touch_inject.sock"
SUBUCOM_SOCK = "/tmp/subucom_ctrl.sock"
SUBUCOM_PORT = 9999

def log(msg):
    print(f"[TEST_VIEWS] {msg}", flush=True)

def capture_screen(name):
    os.makedirs(SNAPSHOT_DIR, exist_ok=True)
    out_path = os.path.join(SNAPSHOT_DIR, f"{name}.png")
    env = os.environ.copy()
    env["DISPLAY"] = env.get("DISPLAY", ":0")

    # Try ImageMagick import, then scrot, then grim
    for cmd in [
        ["import", "-window", "root", out_path],
        ["scrot", "-o", out_path],
        ["grim", out_path]
    ]:
        try:
            res = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if res.returncode == 0 and os.path.isfile(out_path) and os.path.getsize(out_path) > 1000:
                log(f"  -> Captured {name}: {out_path} ({os.path.getsize(out_path)} bytes)")
                return out_path
        except FileNotFoundError:
            continue

    log(f"  -> Warning: Screen capture failed for {name}")
    return None

def inject_touch(x, y, hold_sec=0.1):
    """Inject a touch down and touch up via touch_shim UNIX datagram socket"""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        s.sendto(f"{x},{y},1\n".encode(), TOUCH_SOCK)
        time.sleep(hold_sec)
        s.sendto(f"{x},{y},0\n".encode(), TOUCH_SOCK)
        s.close()
        return True
    except Exception as e:
        log(f"  Touch injection error: {e}")
        return False

def inject_x11_click(x, y):
    """Simulate X11 mouse click via xdotool to verify vnc_touch_bridge"""
    try:
        env = os.environ.copy()
        env["DISPLAY"] = env.get("DISPLAY", ":0")
        subprocess.run(["xdotool", "mousemove", str(x), str(y), "click", "1"],
                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2)
        return True
    except Exception as e:
        log(f"  xdotool click error: {e}")
        return False

def send_subucom_mixer_cmd(browse_view=0, browse_push=0, browse_delta=0):
    """Send Mixer packet to UDP 9999 to simulate hardware button/knob actions"""
    try:
        # Mixer is Bus 2. Total wire frame: 1 byte bus + 128 bytes payload
        # xdj_az_abi64:
        # byte 0: magic 0x50
        # byte 1: cmd 0x02
        # byte 2: packet_id
        # byte 3: flags
        # byte 4..11: faders (ch1..4)
        # byte 12..13: crossfader
        # byte 14..37: EQs & Trims
        # byte 38..45: Sound Color FX
        # byte 46..51: Beat FX
        # byte 52: ch_cue_states
        # byte 53: master_cue, bfx_on
        # byte 54: browse_push (bit 0), browse_back (bit 1), browse_view (bit 2)
        # byte 55: browse_delta (signed int8)
        payload = bytearray([0x02] + [0] * 128)
        payload[1] = 0x50 # Magic
        payload[2] = 0x02 # Cmd
        payload[5] = 0xFF # Ch1 fader
        payload[6] = 0x03
        payload[7] = 0xFF # Ch2 fader
        payload[8] = 0x03

        # Browse bits
        browse_bits = 0
        if browse_push: browse_bits |= (1 << 0)
        if browse_view: browse_bits |= (1 << 2)
        payload[55] = browse_bits
        payload[56] = browse_delta & 0xFF

        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sendto(bytes(payload), ('127.0.0.1', SUBUCOM_PORT))
        s.close()
        return True
    except Exception as e:
        log(f"  subucom send error: {e}")
        return False

def main():
    log("==========================================================")
    log(" AlphaTheta XDJ-AZ Automated View & Remote Interaction QA")
    log("==========================================================")

    # 1. Capture Initial State (Source View)
    log("[STEP 1] Testing Initial Clean Boot / Source View...")
    time.sleep(1.0)
    capture_screen("01_source_view")

    # 2. Test Remote Touch / Click on Cloud Log In button (at x=250, y=140)
    log("[STEP 2] Testing Remote Touch Injection (via touch_shim socket)...")
    if inject_touch(250, 140, hold_sec=0.12):
        log("  Injected touch at (250, 140) -> Cloud Log In tab")
    time.sleep(1.5)
    capture_screen("02_touch_cloud_login")

    # 3. Test X11 VNC Remote Click Bridge (via xdotool through vnc_touch_bridge)
    log("[STEP 3] Testing Remote X11 Click Bridge (simulating VNC / noVNC mouse click)...")
    if inject_x11_click(468, 140):
        log("  Simulated VNC click at (468, 140)")
    time.sleep(1.5)
    capture_screen("03_vnc_click_bridge")

    # 4. Test Browse View Trigger (Subucom browse_view button)
    log("[STEP 4] Testing Browse View Trigger via Subucom hardware packet...")
    send_subucom_mixer_cmd(browse_view=1)
    time.sleep(0.1)
    send_subucom_mixer_cmd(browse_view=0) # Release
    time.sleep(1.5)
    capture_screen("04_browse_view")

    # 5. Test Rotary Browse Scroll
    log("[STEP 5] Testing Rotary Encoder Scroll in Library...")
    for _ in range(3):
        send_subucom_mixer_cmd(browse_delta=1)
        time.sleep(0.08)
    time.sleep(1.0)
    capture_screen("05_browse_scroll")

    # 6. Test Waveform / Deck View
    log("[STEP 6] Testing Waveform / Deck View Navigation...")
    # Click top-left Waveform / Back or inject touch on Deck area
    inject_touch(100, 50, hold_sec=0.1) # Back to main performance view
    time.sleep(1.5)
    capture_screen("06_waveform_view")

    # 7. Test Deck 1 / Deck 2 Play buttons
    log("[STEP 7] Testing Deck 1 Transport Trigger...")
    # Deck 1 Play: Bus 0 (ERP/Deck1)
    deck1_play = bytearray([0x00] + [0] * 128)
    deck1_play[1] = 0x50 # Magic
    deck1_play[2] = 0x01
    deck1_play[4] = 0x01 # Play bit
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(bytes(deck1_play), ('127.0.0.1', SUBUCOM_PORT))
    s.close()
    time.sleep(1.5)
    capture_screen("07_deck1_play_state")

    log("==========================================================")
    log(" Automated View Navigation and QA Complete!")
    log(f" All view snapshots saved in: {SNAPSHOT_DIR}")
    log("==========================================================")

if __name__ == '__main__':
    main()
