import sys, re, base64

def find_obfuscated_strings(file_path):
    with open(file_path, "rb") as f:
        d = f.read()

    # In these binaries, strings were base64 encoded, then XOR'd with 0xA5.
    # So if we XOR every byte of the file with 0xA5, valid base64 strings will appear!
    xored = bytes([b ^ 0xA5 for b in d])

    # Find sequences of ASCII characters that look like base64
    matches = re.findall(rb'[A-Za-z0-9+/=]{4,}', xored)
    results = set()
    for m in matches:
        if len(m) >= 4 and len(m) % 4 == 0:
            try:
                dec = base64.b64decode(m)
                # Check if dec is printable ASCII
                if all(32 <= b < 127 or b in [10, 13, 9] for b in dec) and len(dec) >= 3:
                    results.add((dec.decode('latin1', 'replace'), m.decode('latin1', 'replace')))
            except Exception:
                pass
    return sorted(results)

if __name__ == "__main__":
    path = sys.argv[1]
    res = find_obfuscated_strings(path)
    print(f"=== {path} ({len(res)} strings) ===")
    for dec, b64 in res:
        print(f"  {repr(dec)}  (from {b64})")

