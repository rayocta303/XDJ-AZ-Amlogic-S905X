import re
import struct

path = r"P:\XDJ-AZ-Amlogic-S905X\XDJAZv130\initoptenv"
with open(path, "rb") as f:
    d = f.read()

import re
import struct

path = r"P:\XDJ-AZ-Amlogic-S905X\XDJAZv130\initoptenv"
with open(path, "rb") as f:
    d = f.read()

# 0x4222b0 = page 0x422000, offset 0x2b0
# In aarch64 STR Xn, [Xm, #0x2b0]
# 0x2b0 / 8 = 0x56 = 86 (imm9 in ldr/str)
# Let's search for ADRP to 0x422000
for i in range(0, len(d) - 4, 4):
    instr = struct.unpack("<I", d[i:i+4])[0]
    # Check if adrp (op=10000)
    if (instr & 0x9f000000) == 0x90000000:
        # ADRP
        immlo = (instr >> 29) & 3
        immhi = (instr >> 5) & 0x7ffff
        imm = (immhi << 2) | immlo
        if imm & 0x100000:
            imm -= 0x200000
        pc = 0x400000 + i
        target_page = (pc & ~0xfff) + (imm << 12)
        if target_page == 0x422000:
            print(f"ADRP to 0x422000 at VA 0x{pc:x}")



