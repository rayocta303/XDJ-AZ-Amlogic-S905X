import gdb, base64, struct

gdb.execute("b *0x403058")
gdb.execute("r")

inferior = gdb.selected_inferior()
mem_ptr = inferior.read_memory(0x4222b0, 8)
ptr = struct.unpack("<Q", mem_ptr)[0]
print(f"Table ptr: 0x{ptr:x}")

for i in range(150):
    entry = ptr + i * 24
    mem = inferior.read_memory(entry, 24)
    b, e, c = struct.unpack("<QQQ", mem)
    if b == 0 or e < b or (e - b) > 5000:
        break
    s_bytes = bytes(inferior.read_memory(b, e - b))
    xored = bytes([x ^ 0xA5 for x in s_bytes])
    try:
        dec = base64.b64decode(xored).decode("latin1")
    except Exception:
        dec = f"[raw] {xored}"
    print(f"[{i:02d}] (offset 0x{i*24:03x}): {dec}")

