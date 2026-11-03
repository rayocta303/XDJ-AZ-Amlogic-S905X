#!/usr/bin/env python3
"""
test_flx6_midi_out.py — Pioneer DDJ-FLX6 Hardware LED & Platter Ring Diagnostic
Tests:
  - Play/Pause & Cue LEDs (Deck 1..4)
  - Beat Sync & KeyLock (Master Tempo) LEDs
  - Channel PFL Cue LEDs (Ch 1..4) & Master Cue LED
  - Performance Pad LEDs 1..8 across Deck 1..4
  - 72-LED Platter Jog Ring Animation (Ch 11, CC 0 & 1)
  - Channel VU Level Meters (CC 2 on Ch 0..3)
"""

import sys
import time
import glob

def find_midi_device():
    devs = glob.glob('/dev/snd/midi*')
    if not devs:
        print("[!] No /dev/snd/midi* device found!")
        sys.exit(1)
    return devs[0]

def main():
    dev = find_midi_device()
    print(f"[*] Opening rawmidi device {dev} for DDJ-FLX6 MIDI Out test...")
    
    try:
        f = open(dev, 'wb', buffering=0)
    except Exception as e:
        print(f"[!] Failed to open {dev}: {e}")
        sys.exit(1)

    print("[*] 1. Illuminating Play/Pause & Cue LEDs on Decks 1..4...")
    for d in range(4):
        # Note 11 = Play (Vel 127 = On)
        f.write(bytes([0x90 | d, 11, 127]))
        # Note 12 = Cue (Vel 127 = On)
        f.write(bytes([0x90 | d, 12, 127]))
        # Note 88 = Sync
        f.write(bytes([0x90 | d, 88, 127]))
        # Note 93 = Master Tempo / Key Lock
        f.write(bytes([0x90 | d, 93, 127]))
        # Note 84 = Channel Headphone Cue
        f.write(bytes([0x90 | d, 84, 127]))
        # Deck 3 / 4 indicators (Note 114)
        f.write(bytes([0x90 | d, 114, 127]))

    # Master Cue on Ch 6 (Note 99)
    f.write(bytes([0x96, 99, 127]))

    print("[*] 2. Illuminating Performance Pad 1..8 LEDs...")
    # Pad channels: Deck 1=7, Deck 2=9, Deck 3=11, Deck 4=13
    pad_channels = [7, 9, 11, 13]
    for pch in pad_channels:
        for p in range(8):
            f.write(bytes([0x90 | pch, p, 127]))

    print("[*] 3. Running 72-LED Jog Ring Spinning Animation & VU Meter Cascade (5 seconds)...")
    for loop in range(3):
        for step in range(1, 73, 2):
            # Left Platter Ring: Ch 11 (0x0B), CC 0, Val 1..72
            f.write(bytes([0xBB, 0, step]))
            # Right Platter Ring: Ch 11 (0x0B), CC 1, Val 1..72
            f.write(bytes([0xBB, 1, ((step + 36 - 1) % 72) + 1]))

            # Channel VU meters: CC 2 on Ch 0..3 (Banded 0x26..0x7F)
            vu_val = 0x26 + int((step / 72.0) * (0x7F - 0x26))
            for d in range(4):
                f.write(bytes([0xB0 | d, 2, vu_val]))

            time.sleep(0.025)

    print("[*] 4. Resetting LEDs to default operating state (Deck 1/2 Play/Cue ready)...")
    for d in range(4):
        # Reset Play to ON for active playing decks (Deck 1 & 2), Cue to OFF
        if d < 2:
            f.write(bytes([0x90 | d, 11, 127])) # Play ON
            f.write(bytes([0x90 | d, 12, 0]))   # Cue OFF
        else:
            f.write(bytes([0x90 | d, 11, 0]))
            f.write(bytes([0x90 | d, 12, 127])) # Cue ON
        f.write(bytes([0x90 | d, 114, 0])) # Deck 3/4 toggle off
        f.write(bytes([0xB0 | d, 2, 0]))   # Clear VU

    f.close()
    print("[+] DDJ-FLX6 MIDI Out Diagnostic finished successfully!")

if __name__ == '__main__':
    main()
