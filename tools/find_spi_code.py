with open('/home/root/pdj/EP147', 'rb') as f:
    data = f.read()

import struct

def search_adrp(target_page, target_off):
    # ADRP: immhi:immlo
    # PC-relative page offset
    # Let's search for ADD instruction with target_off
    # ADD Xd, Xn, #target_off (12-bit unsigned immediate)
    # op: 0x91000000 | (imm12 << 10)
    imm12 = target_off & 0xFFF
    add_mask = 0xFFC003E0 # ADD Xd, Xn, #imm12
    # In ARM64 ADD 64-bit: 1 0 0 1 0 0 0 1 0 0 imm12(12) Rn(5) Rd(5)
    # 0x91000000 | (imm12 << 10)
    pattern = 0x91000000 | (imm12 << 10)
    for i in range(0, len(data) - 4, 4):
        val = struct.unpack('<I', data[i:i+4])[0]
        if (val & 0xFFC00000) == (pattern & 0xFFC00000):
            # Found ADD with imm12! Check preceding instruction for ADRP
            if i >= 4:
                prev = struct.unpack('<I', data[i-4:i])[0]
                if (prev & 0x9F000000) == 0x90000000: # ADRP
                    # Compute PC-relative target page
                    pc = i - 4
                    immlo = (prev >> 29) & 0x3
                    immhi = (prev >> 5) & 0x7FFFF
                    imm = (immhi << 2) | immlo
                    if imm & (1 << 20):
                        imm -= (1 << 21)
                    calc_page = (pc & ~0xFFF) + (imm << 12)
                    if calc_page == target_page:
                        print("Found reference at 0x%08x -> 0x%08x + 0x%x" % (pc, calc_page, imm12))

print("Searching subucom_spi3.0 (0x02a33b55):")
search_adrp(0x02a33000, 0xb55)

print("Searching subucom_spi1.0 (0x02a33aa5):")
search_adrp(0x02a33000, 0xaa5)
