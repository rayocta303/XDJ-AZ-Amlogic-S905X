import struct
import time
import subprocess

def main():
    out = subprocess.check_output(['pgrep', '-x', 'EP147']).decode().strip().split('\n')
    pid = int(out[0])
    mem_path = f"/proc/{pid}/mem"
    with open(mem_path, "rb", 0) as mem:
        mem.seek(0x03bd7830)
        registry = struct.unpack("<Q", mem.read(8))[0]
        mem.seek(registry + 0x1a0)
        lo, hi = struct.unpack("<QQ", mem.read(16))
        count = (hi - lo) // 8

        players = [None] * 4
        for i in range(count):
            mem.seek(lo + i * 8)
            entry = struct.unpack("<Q", mem.read(8))[0]
            try:
                mem.seek(entry)
                vptr = struct.unpack("<Q", mem.read(8))[0]
                if vptr == 0x2da1c28:
                    mem.seek(entry + 0x38)
                    deck_idx = struct.unpack("<I", mem.read(4))[0]
                    if 0 <= deck_idx < 4:
                        players[deck_idx] = entry
            except Exception:
                continue

        print(f"Players found: {[hex(p) if p else None for p in players]}")
        for step in range(10):
            line = f"[{step}] "
            for d in range(4):
                p = players[d]
                if p:
                    mem.seek(p + 0xb0)
                    src = struct.unpack("<Q", mem.read(8))[0]
                    if src:
                        mem.seek(src + 0xe8)
                        pos = struct.unpack("<q", mem.read(8))[0]
                        if pos == 0x7fffffffffffffff:
                            line += f"D{d+1}:[EMPTY] "
                        else:
                            line += f"D{d+1}:{pos}({pos/44100.0:.2f}s) "
                    else:
                        line += f"D{d+1}:[NOSRC] "
            print(line)
            time.sleep(0.5)

if __name__ == '__main__':
    main()
