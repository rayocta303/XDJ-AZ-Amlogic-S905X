#!/usr/bin/env python3
"""
capture_sample.py — Automated Snapshot, Framebuffer & Log Collector for XDJ-AZ

Captures live debugging samples directly from the target board into out/snapshot/:
1. Full display screen screenshot (1280x800)
2. Raw Framebuffer dump (/dev/fb0)
3. Smart UI crops:
   - Beat FX panel (x=1080..1280, y=50..460)
   - X-PAD ribbon (x=1090..1270, y=345..425)
   - Deck 1..4 status bar (x=0..1280, y=700..800)
4. Comprehensive debugging logs:
   - EP147 application stdout/stderr (/opt/xdjaz/log/ep147.log)
   - Systemd unit journal (xdjaz.service)
   - Pioneer EDB database stream log (/opt/xdjaz/log/edb_streamd.log)
   - Active subucom hardware frame & mixer state (/tmp/xdj_panel_state.json)
   - System display & hardware diagnostics (xrandr, fb0 mode, memory, dmesg)

Usage:
  python tools/capture_sample.py [tag]
  python tools/capture_sample.py --tag filter_test --ip 192.168.137.153
  python tools/capture_sample.py --open
"""

import os
import sys
import time
import argparse
import datetime
import subprocess
import shutil

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

DEFAULT_IP = os.environ.get("TARGET_IP", "192.168.137.153")
DEFAULT_USER = os.environ.get("TARGET_USER", "root")

def run_ssh(host, user, cmd, timeout=15):
    """Execute command on target via SSH."""
    full_cmd = [
        "ssh",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "LogLevel=ERROR",
        f"{user}@{host}",
        cmd
    ]
    try:
        res = subprocess.run(full_cmd, capture_output=True, text=True, timeout=timeout)
        return res.returncode, res.stdout, res.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "SSH command timed out"
    except Exception as e:
        return -1, "", str(e)

def scp_from_target(host, user, remote_path, local_path, timeout=20):
    """Copy file from target to local host via SCP."""
    full_cmd = [
        "scp",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "LogLevel=ERROR",
        f"{user}@{host}:{remote_path}",
        local_path
    ]
    try:
        res = subprocess.run(full_cmd, capture_output=True, timeout=timeout)
        return res.returncode == 0
    except Exception:
        return False

def main():
    parser = argparse.ArgumentParser(description="XDJ-AZ Automated Snapshot & Log Collector")
    parser.add_argument("tag", nargs="?", default="", help="Optional descriptive tag for the snapshot")
    parser.add_argument("--ip", default=DEFAULT_IP, help=f"Target IP address (default: {DEFAULT_IP})")
    parser.add_argument("--user", default=DEFAULT_USER, help=f"Target SSH username (default: {DEFAULT_USER})")
    parser.add_argument("--open", action="store_true", help="Open snapshot folder in Explorer upon completion")
    args = parser.parse_args()

    # Determine paths
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    snapshot_base = os.path.join(repo_root, "out", "snapshot")
    os.makedirs(snapshot_base, exist_ok=True)

    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    folder_name = f"snapshot_{timestamp}"
    if args.tag:
        clean_tag = "".join(c if c.isalnum() or c in ("-", "_") else "_" for c in args.tag)
        folder_name += f"_{clean_tag}"

    out_dir = os.path.join(snapshot_base, folder_name)
    os.makedirs(out_dir, exist_ok=True)

    print("==================================================================")
    print("  XDJ-AZ Automated Snapshot & Debugging Sample Collector")
    print("==================================================================")
    print(f"[*] Target IP    : {args.user}@{args.ip}")
    print(f"[*] Output Dir   : {out_dir}")
    print("[*] Collecting live screen, framebuffer & telemetry...")

    # Remote collection script
    remote_script = """
set -u
TMP_DIR="/tmp/az_snap"
mkdir -p "$TMP_DIR"
rm -f "$TMP_DIR"/*

# 1. Screen capture via capture_screen.sh
DISPLAY=:0 sh /opt/xdjaz/scripts/capture_screen.sh "$TMP_DIR/screen.png" >/dev/null 2>&1 || true

# 2. Raw Framebuffer dump
if [ -e /dev/fb0 ]; then
    cat /dev/fb0 > "$TMP_DIR/fb0.raw" 2>/dev/null || true
    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg -y -f fbdev -i /dev/fb0 -vframes 1 "$TMP_DIR/fb0.png" >/dev/null 2>&1 || true
    fi
fi

# 3. System and Display info
cat <<EOF > "$TMP_DIR/system_info.txt"
=== XDJ-AZ SNAPSHOT DIAGNOSTIC LOG ===
Date: $(date '+%Y-%m-%d %H:%M:%S %Z')
Uptime: $(uptime)

--- DISPLAY & MODESETTING ---
$(DISPLAY=:0 xrandr 2>/dev/null || echo "xrandr unavailable")

--- FRAMEBUFFER INFO ---
FB0 Virtual Size: $(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null || echo "N/A")
FB0 Mode: $(cat /sys/class/graphics/fb0/mode 2>/dev/null || echo "N/A")

--- MEMORY & STORAGE ---
$(free -m)
$(df -h /)

--- RUNNING PROCESSES ---
$(ps aux | grep -E 'EP147|Xorg|flx6|subucom|edb' | grep -v grep || true)

--- SYSTEM KERNEL MSGS (LAST 40) ---
$(dmesg | tail -n 40 || true)
EOF

# 4. Service and app logs
journalctl -u xdjaz.service -n 250 --no-pager > "$TMP_DIR/xdjaz_service.log" 2>/dev/null || true
tail -n 500 /opt/xdjaz/log/ep147.log > "$TMP_DIR/ep147.log" 2>/dev/null || true
tail -n 200 /opt/xdjaz/log/edb_streamd.log > "$TMP_DIR/edb_streamd.log" 2>/dev/null || true
cp /tmp/xdj_panel_state.json "$TMP_DIR/panel_state.json" 2>/dev/null || true

echo "SNAP_DONE"
"""
    ret, out, err = run_ssh(args.ip, args.user, remote_script, timeout=20)
    if ret != 0 or "SNAP_DONE" not in out:
        print(f"[-] Error collecting sample on target: {err}")
        return 1

    # Fetch collected files from target
    remote_files = [
        "screen.png",
        "fb0.raw",
        "fb0.png",
        "system_info.txt",
        "xdjaz_service.log",
        "ep147.log",
        "edb_streamd.log",
        "panel_state.json"
    ]

    fetched = []
    for f in remote_files:
        rpath = f"/tmp/az_snap/{f}"
        lpath = os.path.join(out_dir, f)
        if scp_from_target(args.ip, args.user, rpath, lpath, timeout=10):
            fetched.append(f)

    # Perform Smart UI Crops if screen.png is available
    screen_path = os.path.join(out_dir, "screen.png")
    if os.path.exists(screen_path) and HAS_PIL:
        try:
            im = Image.open(screen_path)
            w, h = im.size

            # Scale factor if displayed scaled
            sx = w / 1280.0
            sy = h / 800.0

            def scale_box(x0, y0, x1, y1):
                return (int(x0 * sx), int(y0 * sy), int(x1 * sx), int(y1 * sy))

            # 1. Beat FX Panel crop (x=1080..1280, y=50..470)
            bfx_box = scale_box(1080, 50, 1280, 470)
            bfx_im = im.crop(bfx_box)
            bfx_path = os.path.join(out_dir, "bfx_panel.png")
            bfx_im.save(bfx_path)
            fetched.append("bfx_panel.png")

            # 2. X-PAD Strip crop (x=1090..1270, y=345..430)
            xpad_box = scale_box(1090, 345, 1270, 430)
            xpad_im = im.crop(xpad_box)
            xpad_path = os.path.join(out_dir, "xpad_strip.png")
            xpad_im.save(xpad_path)
            fetched.append("xpad_strip.png")

            # 3. 4-Decks Status Bar crop (x=0..1280, y=700..800)
            decks_box = scale_box(0, 700, 1280, 800)
            decks_im = im.crop(decks_box)
            decks_path = os.path.join(out_dir, "decks_status.png")
            decks_im.save(decks_path)
            fetched.append("decks_status.png")

            # Also update "latest" pointers in out/snapshot/
            latest_screen = os.path.join(snapshot_base, "latest_screen.png")
            latest_bfx = os.path.join(snapshot_base, "latest_bfx.png")
            shutil.copy2(screen_path, latest_screen)
            shutil.copy2(bfx_path, latest_bfx)
        except Exception as e:
            print(f"[!] Warning: Image crop failed: {e}")

    print("\n[+] Snapshot successfully collected!")
    print(f"    Saved Files ({len(fetched)}):")
    for f in fetched:
        size = os.path.getsize(os.path.join(out_dir, f))
        print(f"      - {f:20s} ({size:,} bytes)")

    print(f"\n[+] Location: {out_dir}")

    if args.open:
        if sys.platform == "win32":
            os.startfile(out_dir)
        elif sys.platform == "darwin":
            subprocess.run(["open", out_dir])
        else:
            subprocess.run(["xdg-open", out_dir])

    return 0

if __name__ == "__main__":
    sys.exit(main())
