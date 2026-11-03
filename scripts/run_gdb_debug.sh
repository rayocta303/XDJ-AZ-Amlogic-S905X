#!/bin/bash
set -eu
PID=$(pgrep -x EP147 | head -n 1)
echo "Target PID: $PID"
cat << 'EOF' > /tmp/gdb_script
set pagination off
thread 1
bt 35
quit
EOF
gdb -batch -x /tmp/gdb_script -p "$PID"
