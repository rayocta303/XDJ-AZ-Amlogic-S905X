#!/usr/bin/env python3
import struct

with open("/home/root/pdj/EP147", "rb") as f:
    data = f.read()

bias = 0x400000
print("Scanning for all Caution 43 slot dispatchers...")

# Pattern:
# PC:      tst ...
# PC+4/8:  str ...
# PC+C:    b.eq (0x54000120) or similar
# PC+10:   and w0, w0, #0x70 (0x121c0800 or 0x121c0821)
# PC+14:   mov w1, #0x2b (0x52800561) or mov w0, #0x2b (0x52800560)

for off in range(0, len(data) - 8, 4):
    inst = struct.unpack_from("<I", data, off)[0]
    # Check if mov w1, #0x2b or mov w0, #0x2b
    if inst == 0x52800561 or inst == 0x52800560:
        va = off + bias
        # Look back up to 8 instructions for b.eq
        for back in range(1, 8):
            b_inst = struct.unpack_from("<I", data, off - back * 4)[0]
            if (b_inst & 0xff00001f) == 0x54000000: # B.cond
                cond = b_inst & 0xf
                imm19 = (b_inst >> 5) & 0x7ffff
                target = (va - back * 4) + (imm19 << 2)
                print(f"Site at VA 0x{va:x}: B.cond at 0x{va - back*4:x} (inst 0x{b_inst:08x}) -> target 0x{target:x}")

