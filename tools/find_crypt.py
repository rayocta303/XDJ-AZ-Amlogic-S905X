import struct

path = r"P:\XDJ-AZ-Amlogic-S905X\XDJAZv130\update_env"
with open(path, "rb") as f:
    d = f.read()

# Search for STR to [sp, #168] (0xa8, 168/8 = 21 = 0x15)
for i in range(0, len(d) - 4, 4):
    instr = struct.unpack("<I", d[i:i+4])[0]
    if (instr & 0xffffffe0) == (0xf9000000 | (0x15 << 10) | (31 << 5)):
        reg = instr & 0x1f
        print(f"STR X{reg}, [sp, #168] at VA 0x{0x400000+i:x}")



