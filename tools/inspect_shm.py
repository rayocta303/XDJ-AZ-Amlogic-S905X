import struct

with open('/dev/shm/xdj_az_shared', 'rb') as f:
    data = f.read()

print(f"SHM size = {len(data)}")
magic, ver, seq = struct.unpack_from("<III", data, 0)
print(f"magic = 0x{magic:x}, ver = {ver}, seq = {seq}")
vu_ch = struct.unpack_from("4B", data, 12)
print(f"vu_channel = {vu_ch}")
loaded = struct.unpack_from("4B", data, 20)
print(f"track_loaded = {loaded}")

# mosi_size: uint32_t[4] at offset 24
mosi_sz = struct.unpack_from("4I", data, 24)
print(f"mosi_sz = {mosi_sz}")
# mosi_buf: uint8_t[4][152] at offset 40
# mosi_count: uint32_t[4] at offset 40 + 4*152 = 648
mosi_cnt = struct.unpack_from("4I", data, 648)
print(f"mosi_count = {mosi_cnt}")

# Let's inspect playhead_pos at offset:
# channel_gain: float[4] (16 bytes) -> 664
# cross_assign: int8[4] (4 bytes) -> 680
# cross_pos: float (4) -> 684
# master_gain: float (4) -> 688
# hp_gain: float (4) -> 692
# cue_master_mix: float (4) -> 696
# cue_mask: uint8 (1) -> 700
# eq_high: float[4] (16) -> 701
# eq_mid: float[4] (16) -> 717
# eq_low: float[4] (16) -> 733
# eq_mode: uint8[4] (4) -> 749
# cfx_type: uint8[4] (4) -> 753
# cfx_color: float[4] (16) -> 757
# cfx_param: float[4] (16) -> 773
# dsp_seq: uint32 (4) -> 789
# playhead_pos: uint64[4] (32) -> 793
# is_playing: uint8[4] (4) -> 825
# is_sync: uint8[4] (4) -> 829
# is_master: uint8[4] (4) -> 833
# is_looping: uint8[4] (4) -> 837

# Let's check struct size:
# In C:
# pack(push, 1):
# magic (4), ver (4), seq (4) = 12
# vu_channel (4), vu_master_l (1), vu_master_r (1), vu_cue_l (1), vu_cue_r (1) = 8
# track_loaded (4) = 4
# mosi_size (16), mosi_buf (608), mosi_count (16) = 640
# channel_gain (16), cross_assign (4), cross_pos (4), master_gain (4), hp_gain (4), cue_mix (4), cue_mask (1) = 37
# eq_high (16), eq_mid (16), eq_low (16), eq_mode (4) = 52
# cfx_type (4), cfx_color (16), cfx_param (16), dsp_seq (4) = 40
# playhead_pos (32), is_playing (4), is_sync (4), is_master (4), is_looping (4) = 48
# Total = 12 + 8 + 4 + 640 + 37 + 52 + 40 + 48 = 841 bytes!

fmt = "<III 4B BBBB 4B 4I 608s 4I 4f 4b f f f f B 4f 4f 4f 4B 4B 4f 4f I 4Q 4B 4B 4B 4B"
unpacked = struct.unpack_from(fmt, data, 0)
print(f"dsp_seq = {unpacked[34]}")
print(f"playhead_pos = {unpacked[35:39]}")
print(f"is_playing = {unpacked[39:43]}")
print(f"is_sync = {unpacked[43:47]}")
print(f"is_master = {unpacked[47:51]}")
print(f"is_looping = {unpacked[51:55]}")

