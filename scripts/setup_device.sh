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

# 2b. Prevent Linux console blanking and screen sleep
setterm -blank 0 -powerdown 0 </dev/tty1 2>/dev/null || true
printf '\033[9;0]' > /dev/tty1 2>/dev/null || true

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

# 6. Sparse files for LocalFileCache (virtual cache, 0 bytes physical on disk)
mkdir -p /mnt
for idx in 0 1 2 3; do
    if [ ! -f "/mnt/LocalFileCache${idx}.bin" ]; then
        truncate -s 2G "/mnt/LocalFileCache${idx}.bin" 2>/dev/null || true
        chmod 0666 "/mnt/LocalFileCache${idx}.bin" 2>/dev/null || true
    fi
done

# 7. Network and testmode stubs
echo "off" > /tmp/testmode 2>/dev/null || true
chmod 0666 /tmp/testmode 2>/dev/null || true

echo "XX" > /tmp/ccode 2>/dev/null || true
chmod 0666 /tmp/ccode 2>/dev/null || true

# Extract authentic hardware device MAC addresses
WLAN_MAC=$(cat /sys/class/net/wlan0/address 2>/dev/null || echo "d4:58:00:37:e9:3b")
ETH_MAC=$(cat /sys/class/net/eth0/address 2>/dev/null || echo "00:15:18:01:81:31")
echo "$WLAN_MAC" > /tmp/mlan0Addr 2>/dev/null || true
echo "$ETH_MAC"  > /tmp/eth0Addr 2>/dev/null || true
chmod 0666 /tmp/*Addr 2>/dev/null || true

echo "up" > /var/net_mlan0_state.dat 2>/dev/null || true
echo "0"  > /var/net_eth0_state.dat 2>/dev/null || true

# Setup WPA Supplicant control socket redirection for EP147 Wi-Fi manager
mkdir -p /tmp/WpaCtrl /run/wpa_supplicant /var/run/wpa_supplicant 2>/dev/null || true
chmod 777 /tmp/WpaCtrl /run/wpa_supplicant /var/run/wpa_supplicant 2>/dev/null || true
ln -sf /run/wpa_supplicant/wlan0 /run/wpa_supplicant/mlan0 2>/dev/null || true
ln -sf /run/wpa_supplicant/wlan0 /var/run/wpa_supplicant/mlan0 2>/dev/null || true
ln -sf /run/wpa_supplicant/wlan0 /tmp/WpaCtrl/mlan0 2>/dev/null || true
echo "CONNECTED" > /tmp/WpaCtrl/wpa_event 2>/dev/null || true
chmod 777 /run/wpa_supplicant/* /tmp/WpaCtrl/* 2>/dev/null || true

# Spawn background wpa_cli event listener to feed EP147 Wi-Fi manager
pkill -f "wpa_cli.*wpa_action" 2>/dev/null || true
wpa_cli -i wlan0 -B -a /tmp/WpaCtrl/wpa_action 2>/dev/null || true

# Dummy mlan0 interface matching authentic WLAN MAC
modprobe dummy 2>/dev/null || true
if ! ip link show mlan0 >/dev/null 2>&1; then
    ip link add mlan0 type dummy 2>/dev/null || true
fi
ip link set mlan0 address "$WLAN_MAC" 2>/dev/null || true
ip link set mlan0 up 2>/dev/null || true

# 8. Restore offline cabinet keys, Wi-Fi and Bluetooth configuration
mkdir -p /home/root/settings/cabinet/encryption
if [ -d /root/.xdj_cabinet_backup/encryption ]; then
    cp -a /root/.xdj_cabinet_backup/encryption/* /home/root/settings/cabinet/encryption/ 2>/dev/null || true
fi
touch /home/root/settings/azmod-onelibrarykey.on

cat << 'EOF' > /home/root/settings/wifi.json
{
    "enableBand": "2",
    "start": "true",
    "AP": "NOVA_7F08"
}
EOF

cat << 'EOF' > /home/root/settings/bluetooth_snk.json
{
    "started": "true",
    "remoteAddr": "00:00:00:00:00:00",
    "remoteDeviceName": ""
}
EOF

cat << EOF > /home/root/settings/bluetooth_src.json
{
    "ConnectInfo": {
        "status": "1",
        "remoteAddr": "00:00:00:00:00:00",
        "deviceName": "AlphaTheta XDJ-AZ",
        "btAddr": "$(echo "$WLAN_MAC" | tr '[:lower:]' '[:upper:]')"
    }
}
EOF
chmod 0666 /home/root/settings/*.json 2>/dev/null || true

# 9. Ensure /home/root symlinks
[ -e /home/root/pdj ] || ln -sf /opt/xdjaz/pdj /home/root/pdj
[ -e /home/root/scripts ] || ln -sf /opt/xdjaz/scripts /home/root/scripts

# 10. USB Media Storage: Prioritize physical USB partition, fallback to loop fixture
mkdir -p /media/usb/sda1
REAL_PART=""
for dev in /dev/sd[a-z][0-9]*; do
    if [ -b "$dev" ]; then
        REAL_PART="$dev"
        break
    fi
done

if [ -n "$REAL_PART" ]; then
    echo "[setup_device] Discovered physical USB partition: $REAL_PART -> mounting to /media/usb/sda1"
    umount -l /media/usb/sda1 2>/dev/null || true
    mount -t vfat -o flush,rw,noatime,dmask=000,fmask=000,utf8 "$REAL_PART" /media/usb/sda1 2>/dev/null || \
    mount -o rw,noatime "$REAL_PART" /media/usb/sda1 2>/dev/null || true
    chmod -R 0777 /media/usb/sda1 2>/dev/null || true
else
    # No USB media connected — leave unmounted to prevent Caution 43 dialog
    umount -l /media/usb/sda1 2>/dev/null || true
fi

echo "[setup_device] Device environment prepared successfully."
