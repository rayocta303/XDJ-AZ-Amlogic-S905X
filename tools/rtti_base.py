import struct
import subprocess

with open('/opt/xdjaz/pdj/EP147', 'rb') as f:
    base = 0x400000
    ti_off = 0x25eb1d8 - base
    f.seek(ti_off)
    vptr = struct.unpack('<Q', f.read(8))[0]
    name_ptr = struct.unpack('<Q', f.read(8))[0]
    base_ti_ptr = struct.unpack('<Q', f.read(8))[0]
    
    f.seek(base_ti_ptr - base + 8)
    base_name_ptr = struct.unpack('<Q', f.read(8))[0]
    f.seek(base_name_ptr - base)
    base_name = f.read(128).split(b'\0')[0].decode('utf-8', errors='ignore')
    
    demangled = subprocess.check_output(['c++filt', '-t', base_name]).decode().strip()
    print(f"PlayerStateWithUpdate base class: {base_name} -> {demangled}")

