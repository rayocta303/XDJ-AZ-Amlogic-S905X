import struct

with open('/opt/xdjaz/pdj/EP147', 'rb') as f:
    base = 0x400000 # load address for non-PIE ELF
    for vtbl_addr, name in [(0x25eb390, "Source"), (0x2da1c28, "LocalPlayer")]:
        offset = vtbl_addr - base
        f.seek(offset - 8)
        typeinfo_ptr = struct.unpack('<Q', f.read(8))[0]
        typeinfo_off = typeinfo_ptr - base
        f.seek(typeinfo_off + 8) # char* name in std::type_info
        name_ptr = struct.unpack('<Q', f.read(8))[0]
        name_off = name_ptr - base
        f.seek(name_off)
        raw_name = f.read(128).split(b'\0')[0].decode('utf-8', errors='ignore')
        print(f"Vtable 0x{vtbl_addr:x} ({name}): typeinfo=0x{typeinfo_ptr:x}, mangled={raw_name}")
        # Demangle using c++filt if possible
        import subprocess
        try:
            demangled = subprocess.check_output(['c++filt', '-t', raw_name]).decode().strip()
            print(f"  -> Demangled: {demangled}")
        except Exception as e:
            print(f"  -> Demangle error: {e}")
