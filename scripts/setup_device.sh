#!/bin/sh
# setup_device.sh — Prepare Armbian Amlogic S905X Device Nodes and Environment for XDJ-AZ
set -eu

echo "[setup_device] Initializing hardware stubs, nodes, and permissions..."

# 1. Block /dev/mem to prevent raw hardware register probing
chmod 000 /dev/mem 2>/dev/null || true

# 2. Sysfs backlight brightness stub
mkdir -p /sys/class/backlight/backlight 2>/dev/null || true
touch /sys/class/backlight/backlight/brightness 2>/dev/null || true
chmod 0666 /sys/class/backlight/backlight/brightness 2>/dev/null || true

# 3. Create redirected /proc FIFOs
mkdir -p /tmp/xdj_az_proc
[ -p /tmp/xdj_az_proc/udev_usb1 ]    || mkfifo -m 0666 /tmp/xdj_az_proc/udev_usb1
[ -p /tmp/xdj_az_proc/udev_usb2 ]    || mkfifo -m 0666 /tmp/xdj_az_proc/udev_usb2
[ -p /tmp/xdj_az_proc/udev_usbg1 ]   || mkfifo -m 0666 /tmp/xdj_az_proc/udev_usbg1
[ -p /tmp/xdj_az_proc/udev_usbctn1 ] || mkfifo -m 0666 /tmp/xdj_az_proc/udev_usbctn1

# 4. Create dummy front-panel SPI device nodes
for spi in /dev/subucom_spi1.0 /dev/subucom_spi2.0 /dev/subucom_spi3.0 /dev/subucom_spi5.0 /dev/gpiodrv /dev/hidg0; do
    if [ ! -e "$spi" ]; then
        touch "$spi" 2>/dev/null || true
        chmod 0666 "$spi" 2>/dev/null || true
    fi
done

# 5. Touchscreen detection and symlink
mkdir -p /dev/input/by-id
TOUCH_FOUND=""
for ev in /dev/input/event*; do
    if [ -e "$ev" ]; then
        NAME=$(cat "/sys/class/input/$(basename "$ev")/device/name" 2>/dev/null || true)
        case "$NAME" in
            *[Tt]ouch*|*[Gg]oodix*|*[Ii]litek*|*[Ff]ocal*|*[Ww]aveshare*|*[Cc][Tt][Pp]*|*[Uu][Ss][Bb]2[Ii][Ii][Cc]*)
                TOUCH_FOUND="$ev"
                echo "[setup_device] Found physical touchscreen: $NAME ($ev)"
                break
                ;;
        esac
    fi
done

if [ -n "$TOUCH_FOUND" ]; then
    ln -sf "$TOUCH_FOUND" /dev/input/by-id/gt928
else
    touch /dev/input/by-id/gt928 2>/dev/null || true
    chmod 0666 /dev/input/by-id/gt928 2>/dev/null || true
fi

# Touch calibration defaults (1280x800)
mkdir -p /home/root/settings
cat << 'EOF' > /home/root/settings/TouchCalib_User.dat
0
0
320
200
1280
800
EOF
cp -f /home/root/settings/TouchCalib_User.dat /home/root/settings/TouchCalib_Factory.dat 2>/dev/null || true
chmod 0666 /home/root/settings/TouchCalib_*.dat 2>/dev/null || true

# 6. Sparse file for LocalFileCache0.bin (1.4GB virtual, 0 bytes on disk)
mkdir -p /mnt
if [ ! -f /mnt/LocalFileCache0.bin ]; then
    truncate -s 1400M /mnt/LocalFileCache0.bin 2>/dev/null || true
    chmod 0666 /mnt/LocalFileCache0.bin 2>/dev/null || true
fi

# 7. Network and testmode stubs
echo "off" > /tmp/testmode 2>/dev/null || true
chmod 0666 /tmp/testmode 2>/dev/null || true

echo "XX" > /tmp/ccode 2>/dev/null || true
chmod 0666 /tmp/ccode 2>/dev/null || true

echo "00:00:00:00:00:00" > /tmp/mlan0Addr 2>/dev/null || true
chmod 0666 /tmp/mlan0Addr 2>/dev/null || true

echo "down" > /var/net_mlan0_state.dat 2>/dev/null || true
echo "1" > /var/net_eth0_state.dat 2>/dev/null || true

# Dummy mlan0 interface
modprobe dummy 2>/dev/null || true
if ! ip link show mlan0 >/dev/null 2>&1; then
    ip link add mlan0 type dummy 2>/dev/null || true
    ip link set mlan0 down 2>/dev/null || true
fi

# 8. Restore offline cabinet keys and onelibrary flag
mkdir -p /home/root/settings/cabinet/encryption
if [ -d /root/.xdj_cabinet_backup/encryption ]; then
    cp -a /root/.xdj_cabinet_backup/encryption/* /home/root/settings/cabinet/encryption/ 2>/dev/null || true
fi
touch /home/root/settings/azmod-onelibrarykey.on
echo '{"start":"false"}' > /home/root/settings/wifi.json 2>/dev/null || true

# 9. Ensure /home/root symlinks
[ -e /home/root/pdj ] || ln -sf /opt/xdjaz/pdj /home/root/pdj
[ -e /home/root/scripts ] || ln -sf /opt/xdjaz/scripts /home/root/scripts

# 10. USB Media Fixture: clean FAT32 Rekordbox loopback partition
mkdir -p /opt/xdjaz/media /media/usb/sda1
if [ ! -f /opt/xdjaz/media/usb_fixture.img ]; then
    dd if=/dev/zero of=/opt/xdjaz/media/usb_fixture.img bs=1M count=64 2>/dev/null || true
    mkfs.vfat -F 32 -n "XDJ_AZ" /opt/xdjaz/media/usb_fixture.img >/dev/null 2>&1 || true
fi

if ! grep -qs '/media/usb/sda1' /proc/mounts; then
    mount -o loop,rw /opt/xdjaz/media/usb_fixture.img /media/usb/sda1 2>/dev/null || true
fi
mkdir -p /media/usb/sda1/PIONEER/rekordbox /media/usb/sda1/Contents/Music 2>/dev/null || true
chmod -R 0777 /media/usb/sda1 2>/dev/null || true

echo "[setup_device] Device environment prepared successfully."
