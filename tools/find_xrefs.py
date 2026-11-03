#!/usr/bin/env python3
import struct

with open("/opt/xdjaz/pdj/EP147", "rb") as f:
    data = f.read()

# File offset 0x25c2198 -> VA = 0x25c2198 + 0x400000 = 0x29c2198
str_offset = 0x25c2198
str_va = str_offset + 0x400000

print(f"String 'USB Error. Remove the device.' at file offset {str_offset:#x}, VA {str_va:#x}")

# 1. 64-bit pointer table xrefs
packed_64 = struct.pack("<Q", str_va)
pos = 0
found_ptr = []
while True:
    pos = data.find(packed_64, pos)
    if pos == -1: break
    found_ptr.append(hex(pos + 0x400000))
    pos += 1
print("64-bit Pointer xrefs:", found_ptr)

# 2. 32-bit pointer table xrefs
packed_32 = struct.pack("<I", str_va)
pos = 0
found_ptr32 = []
while True:
    pos = data.find(packed_32, pos)
    if pos == -1: break
    found_ptr32.append(hex(pos + 0x400000))
    pos += 1
print("32-bit Pointer xrefs:", found_ptr32)

# 3. ADRP + ADD xrefs in AArch64 code
# ADRP computes page base: (str_va >> 12) << 12
# offset in page: str_va & 0xfff
page_target = (str_va >> 12) << 12
page_offset = str_va & 0xfff

table_va = 0x29b00b8
table_offset = table_va - 0x400000

# Find beginning of table:
base_idx = 0
for idx in range(0, -200, -1):
    entry_offset = table_offset + idx * 8
    ptr = struct.unpack_from("<Q", data, entry_offset)[0]
    if not (0x29c0000 <= ptr <= 0x29cffff):
        base_idx = idx + 1
        break

table_base_va = table_va + base_idx * 8
caution_idx = -base_idx
print(f"Table begins at VA {table_base_va:#x} (index {base_idx}). 'USB Error' is Caution #{caution_idx} (0x{caution_idx:x})")

# Search for code referencing table_base_va
page_base = (table_base_va >> 12) << 12
page_off = table_base_va & 0xfff
print(f"Search for ADRP page {page_base:#x}, offset {page_off:#x}")

# Scan .text section for ADRP to page_base
# ADRP encoding:
# bits 31: 1
# bits 30-29: immlo (imm & 3)
# bits 28-24: 10000
# bits 23-5: immhi (imm >> 2)
# bits 4-0: Rd
# imm is PC-relative page difference: (page_base - (pc & ~0xfff)) >> 12
text_start = 0x400000
text_end = len(data) + 0x400000

for pc in range(text_start, text_end, 4):
    inst = struct.unpack_from("<I", data, pc - 0x400000)[0]
    # Check if ADRP: (inst & 0x9f000000) == 0x90000000
    if (inst & 0x9f000000) == 0x90000000:
        immlo = (inst >> 29) & 3
        immhi = (inst >> 5) & 0x7ffff
        imm = (immhi << 2) | immlo
        if imm & 0x100000: # negative
            imm -= 0x200000
        target = ((pc >> 12) + imm) << 12
        if target == page_base:
            rd = inst & 0x1f
            # Check next instruction for ADD or LDR with page_off
            next_inst = struct.unpack_from("<I", data, pc - 0x400000 + 4)[0]
            # ADD 64-bit immediate: (next_inst & 0xff800000) == 0x91000000
            if (next_inst & 0xff800000) == 0x91000000:
                add_imm = (next_inst >> 10) & 0xfff
                rn = (next_inst >> 5) & 0x1f
                if rn == rd and add_imm == page_off:
                    add_rd = next_inst & 0x1f
                    print(f"Found Caution Table access at PC {pc:#x}: adrp x{rd}, {page_base:#x}; add x{add_rd}, x{rn}, #{page_off:#x}")
