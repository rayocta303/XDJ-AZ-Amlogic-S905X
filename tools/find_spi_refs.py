import re
import struct

with open('/home/root/pdj/EP147', 'rb') as f:
    data = f.read()

def find_adrl(target_addr):
    # Search for ADRP / ADD or literal pool pointing to target_addr
    # target_addr is around 0x02a33b55
    target_page = target_addr & ~0xFFF
    target_off = target_addr & 0xFFF
    results = []
    # Search 8-byte pointer in data sections
    packed = struct.pack('<Q', target_addr)
    idx = 0
    while True:
        pos = data.find(packed, idx)
        if pos == -1: break
        results.append(("ptr64", pos))
        idx = pos + 1
    
    packed32 = struct.pack('<I', target_addr)
    idx = 0
    while True:
        pos = data.find(packed32, idx)
        if pos == -1: break
        results.append(("ptr32", pos))
        idx = pos + 1
    return results

print("=== References to subucom_spi3.0 (0x02a33b55) ===")
for typ, pos in find_adrl(0x02a33b55):
    print(f"  {typ} at 0x{pos:08x}")

print("=== References to subucom_spi1.0 (0x02a33aa5) ===")
for typ, pos in find_adrl(0x02a33aa5):
    print(f"  {typ} at 0x{pos:08x}")
