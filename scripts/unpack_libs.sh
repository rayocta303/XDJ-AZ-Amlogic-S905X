#!/bin/sh
set -eu
mkdir -p /opt/xdjaz/usr
if [ -f /tmp/az_usr_lib.tar.gz ]; then
    tar -xzf /tmp/az_usr_lib.tar.gz -C /opt/xdjaz/usr
    rm -f /tmp/az_usr_lib.tar.gz
fi

cd /opt/xdjaz/usr/lib
for sym in $(find . -name "*.symlink"); do
    target=$(cat "$sym")
    dest="${sym%.symlink}"
    rm -f "$dest"
    ln -sf "$target" "$dest"
    rm -f "$sym"
done

echo "[unpack_libs] Done. Total files in /opt/xdjaz/usr/lib:"
ls -1 /opt/xdjaz/usr/lib | wc -l
