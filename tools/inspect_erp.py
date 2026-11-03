import struct

with open('/dev/shm/xdj_az_shared', 'rb') as f:
    f.seek(40) # mosi_buf[0]
    erp = f.read(152)

print(f"ErP frame total length = {len(erp)}")
for side in (0, 1):
    stride = side * 0x38
    e = erp[stride:stride+0x38]
    print(f"--- Side {side} (Deck {'1/3' if side==0 else '2/4'}) ---")
    master_lvl = (e[0x08] >> 4) & 3
    mt_lvl = (e[0x08] >> 2) & 3
    sync_lvl = (e[0x09] >> 6) & 3
    reloop_lvl = (e[0x09] >> 0) & 3
    in_lvl = (e[0x0A] >> 6) & 3
    out_lvl = (e[0x0A] >> 4) & 3
    play_lvl = (e[0x0B] >> 0) & 3
    cue_lvl = (e[0x0B] >> 2) & 3
    print(f"  e[0x08]=0x{e[0x08]:02x} -> master_lvl={master_lvl}, mt_lvl={mt_lvl}")
    print(f"  e[0x09]=0x{e[0x09]:02x} -> sync_lvl={sync_lvl}, reloop_lvl={reloop_lvl}")
    print(f"  e[0x0A]=0x{e[0x0A]:02x} -> in_lvl={in_lvl}, out_lvl={out_lvl}")
    print(f"  e[0x0B]=0x{e[0x0B]:02x} -> play_lvl={play_lvl}, cue_lvl={cue_lvl}")

