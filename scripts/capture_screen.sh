#!/bin/sh
# capture_screen.sh — Capture current display frame on Armbian S905X
set -eu

OUTPUT="${1:-/opt/xdjaz/screenshot.png}"
export DISPLAY="${DISPLAY:-:0}"

if command -v import >/dev/null 2>&1; then
    import -window root "$OUTPUT" 2>/dev/null && echo "[capture] Captured using ImageMagick: $OUTPUT" && exit 0
fi

if command -v scrot >/dev/null 2>&1; then
    scrot -o "$OUTPUT" 2>/dev/null && echo "[capture] Captured using scrot: $OUTPUT" && exit 0
fi

if command -v grim >/dev/null 2>&1; then
    grim "$OUTPUT" 2>/dev/null && echo "[capture] Captured using grim: $OUTPUT" && exit 0
fi

# Fallback: Framebuffer dump via ffmpeg
if [ -e /dev/fb0 ] && command -v ffmpeg >/dev/null 2>&1; then
    ffmpeg -y -f fbdev -framerate 1 -i /dev/fb0 -vframes 1 "$OUTPUT" 2>/dev/null && \
    echo "[capture] Captured using ffmpeg fbdev: $OUTPUT" && exit 0
fi

echo "[capture] Error: No supported capture tool found." >&2
exit 1
