#!/bin/sh
# run_az.sh — Master Turnkey Production Launcher for XDJ-AZ on Armbian Amlogic S905X
set -eu

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
    "$SHIMS_DIR/fractional_grid.so" \
    "$SHIMS_DIR/fractional_grid_span.so" \
    "$SHIMS_DIR/ximage_fast24.so"; do
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
