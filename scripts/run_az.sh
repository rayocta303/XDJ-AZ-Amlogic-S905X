#!/bin/sh
# run_az.sh — Master Turnkey Production Launcher for XDJ-AZ on Armbian Amlogic S905X
# set -e
set -u

BASE_DIR="/opt/xdjaz"
SHIMS_DIR="$BASE_DIR/out"
LOG_DIR="$BASE_DIR/log"
EXEC_LOG="$LOG_DIR/ep147.log"

mkdir -p "$LOG_DIR"

echo "=========================================================="
echo " AlphaTheta XDJ-AZ Runtime on Armbian Amlogic S905X"
echo "=========================================================="

# 1. Run device setup
sh "$BASE_DIR/scripts/setup_device.sh"

# 2. Stop conflicting audio services
systemctl stop pulseaudio rblive4 2>/dev/null || true

# 2b. Start Pioneer Extended Database Stream Daemon (edb_streamd)
if [ -x /usr/bin/edb_streamd ]; then
    killall -9 edb_streamd 2>/dev/null || true
    rm -f /tmp/edb* /var/run/edb_streamd.pid
    env -u LD_PRELOAD /usr/bin/edb_streamd > "$LOG_DIR/edb_streamd.log" 2>&1 &
    sleep 1
    echo "[run_az] Pioneer edb_streamd database daemon started"
fi

# 3. Setup Display Server (Direct Modesetting on DRM /dev/dri/card0)
export DISPLAY="${DISPLAY:-:0}"
if ! pgrep -x Xorg >/dev/null && ! pgrep -x X >/dev/null; then
    echo "[run_az] Starting Xorg server with DRM KMS modesetting on :0..."
    Xorg :0 vt1 -auth /dev/null >/var/log/Xorg.0.log 2>&1 &
    sleep 2
fi

# Configure HDMI output mode 1280x800
if command -v xrandr >/dev/null 2>&1; then
    xrandr --output Composite-1 --off 2>/dev/null || true
    xrandr --output HDMI-1 --primary --mode 1280x800 2>/dev/null || \
    xrandr --output HDMI-1 --primary --mode 1024x600 2>/dev/null || true
fi

# Touchscreen identity matrix
if command -v xinput >/dev/null 2>&1; then
    for dev_id in $(xinput list --id-only 2>/dev/null); do
        xinput set-prop "$dev_id" "Coordinate Transformation Matrix" 1 0 0 0 1 0 0 0 1 2>/dev/null || true
    done
fi

# 3b. Start High-Performance Remote VNC (Port 5900) & HTML5 Web Remote (Port 5800)
pkill -9 x11vnc websockify vnc_touch_bridge 2>/dev/null || true
x11vnc -display :0 -forever -shared -nopw -rfbport 5900 -bg -o "$LOG_DIR/x11vnc.log" 2>/dev/null || true
if command -v websockify >/dev/null 2>&1 && [ -d /usr/share/novnc ]; then
    websockify --web /usr/share/novnc 5800 localhost:5900 >/dev/null 2>&1 &
    echo "[run_az] Web Remote HTML5 active at: http://192.168.137.227:5800/vnc.html"
fi

# 3c. Start VNC Remote Touch Bridge (Translates mouse clicks from noVNC/VNC to EP147 touch)
if [ -x "$SHIMS_DIR/vnc_touch_bridge" ]; then
    "$SHIMS_DIR/vnc_touch_bridge" > "$LOG_DIR/vnc_touch_bridge.log" 2>&1 &
    echo "[run_az] VNC Touch Bridge running (PID: $!)"
fi

# 3d. Start Automatic USB Storage Mount Daemon
if [ -f "$BASE_DIR/scripts/automount_storage.sh" ]; then
    pkill -f "automount_storage.sh" 2>/dev/null || true
    sh "$BASE_DIR/scripts/automount_storage.sh" >/dev/null 2>&1 &
    echo "[run_az] USB Storage Automount Daemon running (PID: $!)"
fi

# 4. Start FLX6 High-Performance Native C Bridge
if [ -x "$SHIMS_DIR/flx6_controls" ]; then
    killall -9 flx6_controls 2>/dev/null || true
    "$SHIMS_DIR/flx6_controls" > "$LOG_DIR/flx6_bridge.log" 2>&1 &
    echo "[run_az] Pioneer DDJ-FLX6 Native C Bridge running (PID: $!)"
fi

# 5. Build Preload Shim Chain
PRELOAD_CHAIN=""
for shim in \
    "$SHIMS_DIR/libcrashcatch.so" \
    "$SHIMS_DIR/libsubucom_emu.so" \
    "$SHIMS_DIR/libproc_usb_shim.so" \
    "$SHIMS_DIR/libenv_emu_shim.so" \
    "$SHIMS_DIR/libnet_shim.so" \
    "$SHIMS_DIR/libtouch_shim.so" \
    "$SHIMS_DIR/libaudio_shim.so" \
    "$SHIMS_DIR/sem_owner.so" \
    "$SHIMS_DIR/fractional_grid_span.so" \
    "$SHIMS_DIR/ximage_fast24.so" \
    "$SHIMS_DIR/libaz_patch_mod.so" \
    "$SHIMS_DIR/azmod.so"; do
    if [ -f "$shim" ]; then
        if [ -z "$PRELOAD_CHAIN" ]; then
            PRELOAD_CHAIN="$shim"
        else
            PRELOAD_CHAIN="$PRELOAD_CHAIN:$shim"
        fi
    fi
done

export LD_PRELOAD="$PRELOAD_CHAIN"
export LD_LIBRARY_PATH="/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:$BASE_DIR/usr/lib:$BASE_DIR/lib:${LD_LIBRARY_PATH:-}"
export HOME="/home/root"

echo "[run_az] Preloaded Shims: $LD_PRELOAD"
echo "[run_az] Launching EP147 Player..."

cd /home/root/pdj
exec /home/root/pdj/EP147 >> "$EXEC_LOG" 2>&1
