#!/usr/bin/env python3
import struct
import sys

bin_path = sys.argv[1] if len(sys.argv) > 1 else "/home/root/pdj/EP147"
target = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x249efe0

with open(bin_path, "rb") as f:
    data = f.read()

bias = 0x400000
print(f"Scanning for BL callers to 0x{target:x} in {bin_path}...")
count = 0
for off in range(0, len(data) - 4, 4):
    inst = struct.unpack_from("<I", data, off)[0]
    if (inst & 0xfc000000) == 0x94000000:
        imm = inst & 0x03ffffff
        if imm & 0x02000000:
            imm -= 0x04000000
        dest = (off + bias) + (imm << 2)
        if dest == target:
            print(f"  BL at 0x{off + bias:x}")
            count += 1
print(f"Total callers: {count}")
