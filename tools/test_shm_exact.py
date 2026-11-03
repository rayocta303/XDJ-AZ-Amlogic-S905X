import struct

with open('/dev/shm/xdj_az_shared', 'rb') as f:
    data = f.read()

magic, ver, seq = struct.unpack_from('<III', data, 0)
vu_ch = struct.unpack_from('4B', data, 12)
track_loaded = struct.unpack_from('4B', data, 20)
mosi_count = struct.unpack_from('4I', data, 648)
dsp_seq = struct.unpack_from('<I', data, 789)[0]
playhead_pos = struct.unpack_from('<4Q', data, 793)
is_playing = struct.unpack_from('4B', data, 825)
is_sync = struct.unpack_from('4B', data, 829)
is_master = struct.unpack_from('4B', data, 833)
is_looping = struct.unpack_from('4B', data, 837)

print(f"SHM Magic: 0x{magic:x}, Version: {ver}, Seq: {seq}")
print(f"Track Loaded: {track_loaded}")
print(f"Playhead Pos: {playhead_pos} (seconds: {[f'{p/44100.0:.2f}s' for p in playhead_pos]})")
print(f"Is Playing:   {is_playing}")
print(f"Is Sync:      {is_sync}")
print(f"Is Master:    {is_master}")
print(f"Is Looping:   {is_looping}")
print(f"MOSI Counts:  {mosi_count}")
