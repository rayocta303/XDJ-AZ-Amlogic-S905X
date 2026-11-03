#!/usr/bin/env python3
import struct

bin_path = "/home/root/pdj/EP147"
with open(bin_path, "rb") as f:
    data = f.read()

bias = 0x400000

for vname, vva in [
    ("gui::caution::KernelHandler", 0x29ac3d8),
    ("usecase::linux::KernelCautionUsecase", 0x269e1c8),
]:
    voff = vva - bias
    print(f"{vname} vtable at 0x{vva:x}:")
    for i in range(12):
        if voff + i * 8 + 8 > len(data): break
        fn = struct.unpack_from("<Q", data, voff + i * 8)[0]
        print(f"  slot {i:2d}: 0x{fn:x}")
