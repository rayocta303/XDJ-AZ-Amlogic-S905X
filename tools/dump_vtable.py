import struct

with open('/opt/xdjaz/pdj/EP147', 'rb') as f:
    base = 0x400000
    f.seek(0x25eb390 - base)
    print("Vtable methods for dj_player::PlayerStateWithUpdate:")
    for idx in range(30):
        fn_ptr = struct.unpack('<Q', f.read(8))[0]
        if fn_ptr < base or fn_ptr > 0x3b00000:
            print(f"  [{idx}] 0x{fn_ptr:x} (end of vtable)")
            break
        print(f"  [{idx}] 0x{fn_ptr:x}")
