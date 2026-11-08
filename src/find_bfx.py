import struct

path = r'P:\az-runtime-lab-notworoking\xdjaz\rootfs\home\root\pdj\EP147'
with open(path, 'rb') as f:
    data = f.read()

# 0x2d63bd0 is vtable
# Find adrp/add that constructs 0x2d63bd0
# 0x2d63bd0 = (0x2d63000) + 0xbd0
# adrp Rd, 0x2d63000 -> immlo: 2 bits, immhi: 19 bits
# Let's search for add Rd, Rd, #0xbd0
for i in range(0, len(data), 4):
    ins = struct.unpack('<I', data[i:i+4])[0]
    # ADD Xd, Xn, #0xbd0 (imm12 = 0xbd0 = 3024)
    # ADD 64-bit: 0x91000000 | (shift << 22) | (imm12 << 10) | (Rn << 5) | Rd
    if (ins & 0xffc00000) == 0x91000000:
        imm12 = (ins >> 10) & 0xFFF
        if imm12 == 0xbd0:
            va = 0x400000 + i
            print(f'Found ADD #0xbd0 at VA: 0x{va:x}')
