#!/bin/sh
# fix_display.sh — Automatic Display Resolution, Scaling & Touch Alignment for XDJ-AZ
# Configures 1280x800 viewport on any physical HDMI display (1024x600, 1280x720, 1920x1080)

set -e
export DISPLAY="${DISPLAY:-:0}"

echo "=========================================================="
echo " XDJ-AZ Display & Screen Alignment Engine"
echo "=========================================================="

# 1. Ensure Xorg configuration directory exists
mkdir -p /etc/X11/xorg.conf.d

# Write KMS DRM modesetting config for Amlogic S905X
cat << 'EOF' > /etc/X11/xorg.conf.d/01-modesetting.conf
Section "Device"
    Identifier "Card0"
    Driver "modesetting"
    Option "kmsdev" "/dev/dri/card0"
    Option "AccelMethod" "glamor"
EndSection

Section "Screen"
    Identifier "Screen0"
    Device "Card0"
    SubSection "Display"
        Modes "1280x800" "1024x600" "1280x720" "1920x1080"
        Virtual 1280 800
    EndSubSection
EndSection
EOF

# Write ServerFlags to disable screensaver and DPMS blanking
cat << 'EOF' > /etc/X11/xorg.conf.d/10-serverflags.conf
Section "ServerFlags"
    Option "BlankTime" "0"
    Option "StandbyTime" "0"
    Option "SuspendTime" "0"
    Option "OffTime" "0"
    Option "DPMS" "false"
EndSection
EOF

# 2. Check Xorg server connection
if ! xhost >/dev/null 2>&1; then
    echo "[fix_display] X server not currently responsive on $DISPLAY. Config files written."
    exit 0
fi

# 3. Disable screensaver and DPMS dynamically
if command -v xset >/dev/null 2>&1; then
    xset s off 2>/dev/null || true
    xset s noblank 2>/dev/null || true
    xset -dpms 2>/dev/null || true
    xset dpms 0 0 0 2>/dev/null || true
    xset dpms force on 2>/dev/null || true
fi

# 4. Turn off Composite / CVBS output
if command -v xrandr >/dev/null 2>&1; then
    xrandr --output Composite-1 --off 2>/dev/null || true

    # Find active connected HDMI output
    HDMI_OUT=$(xrandr | grep " connected" | grep -v "Composite" | awk '{print $1}' | head -n1)
    if [ -z "$HDMI_OUT" ]; then
        HDMI_OUT="HDMI-1"
    fi

    echo "[fix_display] Configuring output: $HDMI_OUT"

    # Add custom 1280x800 modeline if not present
    if ! xrandr | grep -q "1280x800"; then
        xrandr --newmode "1280x800_60.00" 83.50 1280 1352 1480 1680 800 803 809 831 -hsync +vsync 2>/dev/null || true
        xrandr --addmode "$HDMI_OUT" "1280x800_60.00" 2>/dev/null || true
    fi

    # Query EDID preferred / native mode
    NATIVE_MODE=$(xrandr | grep -A1 "$HDMI_OUT connected" | tail -n1 | awk '{print $1}')
    echo "[fix_display] Detected native panel mode: $NATIVE_MODE"

    if [ "$NATIVE_MODE" = "1280x800" ] || [ "$NATIVE_MODE" = "1280x800_60.00" ]; then
        echo "[fix_display] Applying native 1280x800 mode..."
        xrandr --output "$HDMI_OUT" --mode "$NATIVE_MODE" 2>/dev/null || true
    else
        # For 1024x600, 1280x720, 1920x1080 panels:
        # Scale 1280x800 virtual viewport to fit physical panel
        echo "[fix_display] Scaling 1280x800 virtual viewport to physical panel ($NATIVE_MODE)..."
        xrandr --output "$HDMI_OUT" --mode "$NATIVE_MODE" --scale-from 1280x800 2>/dev/null || \
        xrandr --output "$HDMI_OUT" --mode "$NATIVE_MODE" 2>/dev/null || true
    fi
fi

# 5. Reset touch input transformation matrix
if command -v xinput >/dev/null 2>&1; then
    for dev_id in $(xinput list --id-only 2>/dev/null); do
        xinput set-prop "$dev_id" "Coordinate Transformation Matrix" 1 0 0 0 1 0 0 0 1 2>/dev/null || true
    done
fi

echo "[fix_display] Display configured successfully:"
xrandr | grep "Screen 0:"
xrandr | grep -E "connected primary|connected"

