#!/bin/sh
# automount_storage.sh — Automatic USB Storage Hotplug & Mount Daemon for XDJ-AZ
# Watches for physical USB drives (sda1, sdb1, etc.) and auto-mounts them for EP147.

set -u

BASE_DIR="/opt/xdjaz"
MNT_USB1="/media/usb/sda1"
MNT_USB2="/media/usb/sdb1"
PROC_USB1_FIFO="/tmp/xdj_az_proc/udev_usb1"
PROC_USB2_FIFO="/tmp/xdj_az_proc/udev_usb2"
TRIGGER_USB1="/tmp/usb1_event"
TRIGGER_USB2="/tmp/usb2_event"
LOG_FILE="$BASE_DIR/log/automount.log"

mkdir -p "$BASE_DIR/log" "/media/usb"

log() {
    echo "$(date '+%F %T') [automount] $*" >> "$LOG_FILE" 2>/dev/null
}

find_usb_partition() {
    # Scan for physical sdX block devices on USB bus
    for blk in /sys/block/sd*; do
        [ -e "$blk" ] || continue
        tgt=$(readlink -f "$blk" 2>/dev/null) || continue
        case "$tgt" in *"/usb"[0-9]*) ;; *) continue ;; esac
        dev_name="${blk##*/}"
        
        # Look for partition 1 first
        if [ -b "/dev/${dev_name}1" ]; then
            echo "${dev_name}1"
            return 0
        elif [ -b "/dev/${dev_name}" ]; then
            # Unpartitioned disk
            echo "${dev_name}"
            return 0
        fi
    done
    return 1
}

mount_part() {
    dev="$1"
    mnt="$2"
    mkdir -p "$mnt"
    
    # Try VFAT first, then auto
    if mount -t vfat -o flush,rw,noatime,dmask=000,fmask=000,utf8 "/dev/$dev" "$mnt" 2>/dev/null; then
        return 0
    elif mount -o rw,noatime "/dev/$dev" "$mnt" 2>/dev/null; then
        return 0
    fi
    return 1
}

notify_ep147() {
    fifo="$1"
    trig="$2"
    msg="$3"
    
    # Write to trigger file so proc_usb_shim picks it up
    echo "$msg" > "$trig" 2>/dev/null || true
    
    # Also write directly to udev FIFO with timeout if FIFO exists
    if [ -p "$fifo" ]; then
        timeout 2 sh -c "echo '$msg' > '$fifo'" 2>/dev/null || true
    fi
}

current_usb1=""

log "USB Storage Automount Daemon started."

while true; do
    part=$(find_usb_partition || true)
    
    if [ -n "$part" ]; then
        if [ "$part" != "$current_usb1" ]; then
            log "Discovered USB drive: /dev/$part (Previous: '$current_usb1')"
            
            # Check if currently mounted with a dummy loop device
            if grep -qs "$MNT_USB1" /proc/mounts; then
                cur_dev=$(awk -v m="$MNT_USB1" '$2 == m { print $1; exit }' /proc/mounts)
                case "$cur_dev" in
                    /dev/loop*)
                        log "Unmounting dummy fixture $cur_dev from $MNT_USB1"
                        umount -l "$MNT_USB1" 2>/dev/null || true
                        ;;
                esac
            fi
            
            # Mount the real USB drive
            if ! grep -qs "$MNT_USB1" /proc/mounts; then
                if mount_part "$part" "$MNT_USB1"; then
                    log "Successfully mounted /dev/$part to $MNT_USB1"
                    current_usb1="$part"
                    notify_ep147 "$PROC_USB1_FIFO" "$TRIGGER_USB1" "mount $MNT_USB1 vfat protect:0"
                else
                    log "Failed to mount /dev/$part"
                fi
            else
                current_usb1="$part"
            fi
        fi
    else
        # USB drive disconnected
        if [ -n "$current_usb1" ]; then
            log "USB drive /dev/$current_usb1 was removed."
            notify_ep147 "$PROC_USB1_FIFO" "$TRIGGER_USB1" "umount $MNT_USB1"
            umount -l "$MNT_USB1" 2>/dev/null || true
            current_usb1=""
        fi
    fi
    
    sleep 2
done

