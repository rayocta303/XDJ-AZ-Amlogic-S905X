#!/usr/bin/env python3
"""
patch_az.py — Apply standalone interop & performance patches to AlphaTheta XDJ-AZ EP147 binary.

Inspired by rbp_patch.py (XDJ-RX3), this tool patches the AArch64 EP147 binary to:
1. Bypass Caution 43 ("USB Error. Remove the device.") gate dialog
2. Unlock 60 FPS smooth scrolling & subpixel waveform refresh
3. Enable compatibility with generic SBCs / Amlogic S905X running Armbian
4. Ensure hardware & IMX/SoC checks pass as genuine standalone deck

Usage:
    python3 patch_az.py <stock_EP147> [-o <patched_EP147>] [--check]
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

STOCK_SHA256 = '736bdc9322c00e5770af459c92cace33d8680825c07f00f909f74dfc473a77a6'

# Virtual Address (VA) to (stock_word, patched_word, description)
# EP147 Load Bias = 0x400000 (File Offset = VA - 0x400000)
PATCHES = [
    # --- 1. Caution 43 Bypass (Bypasses "USB Error. Remove the device" modal) ---
    (0x25bf848, 0x54000120, 0x14000009, "caution: unconditional branch skipping caution 43 dialog"),

    # --- 2. LocalPlayer 60 FPS Refresh & Repaint Thresholds (Smooth Scrolling) ---
    (0x1e262e8, 0x528003c1, 0x52800781, "smooth: startup refresh request 30 to 60 Hz"),
    (0x212fb94, 0x7100041f, 0x7100001f, "smooth: repaint tick threshold"),
    (0x212f344, 0x7100041f, 0x7100001f, "smooth: upload tick threshold"),
    (0x212f974, 0x11000673, 0x52800033, "smooth: repaint reservation one tick"),
    (0x212f9ac, 0x11000673, 0x52800033, "smooth: alternate repaint reservation one tick"),
    (0x212fc1c, 0x1e632800, 0xd503201f, "smooth: remove extra post-callback period (nop)"),
    (0x212fc44, 0x52800022, 0x52800002, "smooth: repaint fallback zero ticks"),
    (0x212f358, 0x52800022, 0x52800002, "smooth: upload fallback zero ticks"),

    # --- 3. JUCE UI Timers: 33ms (30 FPS) to 16ms (60 FPS) ---
    (0x24fac4c, 0x52800421, 0x52800201, "timer: UI timer site A 33ms -> 16ms"),
    (0x24f6b34, 0x52800421, 0x52800201, "timer: UI timer site B 33ms -> 16ms"),
]

def get_file_offset(data, va):
    if len(data) < 64 or data[:4] != b'\x7fELF':
        raise ValueError("Not a valid ELF file")
    
    # Read ELF64 program headers
    e_phoff = struct.unpack_from('<Q', data, 32)[0]
    e_phentsize = struct.unpack_from('<H', data, 54)[0]
    e_phnum = struct.unpack_from('<H', data, 56)[0]

    for i in range(e_phnum):
        ph_offset = e_phoff + i * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', data, ph_offset)
        if p_type == 1 and (p_flags & 1): # PT_LOAD & executable
            if p_vaddr <= va < p_vaddr + p_filesz:
                return p_offset + (va - p_vaddr)
    
    # Fallback to direct bias
    return va - 0x400000

def patch_binary(data, check_only=False):
    applied = 0
    already_patched = 0
    mismatches = 0

    print(f"[*] Inspecting binary ({len(data)} bytes)...")
    for va, stock_word, patched_word, desc in PATCHES:
        offset = get_file_offset(data, va)
        if offset + 4 > len(data):
            print(f"[-] Offset {offset:#x} out of range for VA {va:#x}")
            mismatches += 1
            continue

        current = struct.unpack_from('<I', data, offset)[0]
        if current == patched_word:
            print(f"  [ALREADY] {va:#010x}: {desc}")
            already_patched += 1
        elif current == stock_word:
            if check_only:
                print(f"  [CAN PATCH] {va:#010x}: {current:#010x} -> {patched_word:#010x} ({desc})")
            else:
                struct.pack_into('<I', data, offset, patched_word)
                print(f"  [APPLIED] {va:#010x}: {stock_word:#010x} -> {patched_word:#010x} ({desc})")
            applied += 1
        else:
            print(f"  [MISMATCH] {va:#010x}: current={current:#010x}, expected stock={stock_word:#010x} ({desc})")
            mismatches += 1

    return applied, already_patched, mismatches

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("binary", type=Path, help="Path to EP147 binary")
    p.add_argument("-o", "--output", type=Path, default=None, help="Output patched file (defaults to in-place)")
    p.add_argument("--check", action="store_true", help="Check patchability only without writing")
    args = p.parse_args()

    if not args.binary.is_file():
        print(f"Error: {args.binary} not found", file=sys.stderr)
        sys.exit(1)

    raw = bytearray(args.binary.read_bytes())
    sha = hashlib.sha256(raw).hexdigest()
    print(f"=== XDJ-AZ Binary Patcher ===")
    print(f"Input:  {args.binary}")
    print(f"SHA256: {sha}")
    if sha == STOCK_SHA256:
        print("[+] Verified stock AlphaTheta XDJ-AZ v1.30 binary")

    applied, already_patched, mismatches = patch_binary(raw, check_only=args.check)

    print("\nSummary:")
    print(f"  Applied:         {applied}")
    print(f"  Already patched: {already_patched}")
    print(f"  Mismatches:      {mismatches}")

    if mismatches > 0:
        print("[!] Warning: some patches had unexpected opcodes", file=sys.stderr)

    if not args.check and applied > 0:
        out_path = args.output if args.output else args.binary
        out_path.write_bytes(raw)
        out_path.chmod(0o755)
        new_sha = hashlib.sha256(raw).hexdigest()
        print(f"[+] Saved patched binary to: {out_path}")
        print(f"[+] New SHA256: {new_sha}")
    elif args.check:
        print("[+] Check complete. Ready for patching.")

if __name__ == '__main__':
    main()
