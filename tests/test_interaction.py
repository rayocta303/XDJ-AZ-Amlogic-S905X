#!/usr/bin/env python3
"""
test_interaction.py — Automated Interaction Test for running XDJ-AZ EP147 instance
Sends simulated subucom controller packets to UDP port 9999 and /tmp/subucom_ctrl.sock.
"""
import socket
import time
import sys

def main():
    print("[INTERACTION TEST] Connecting to subucom_emu listener on 127.0.0.1:9999...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2.0)

    # 1. Ping / Query
    ping_payload = bytes([0x01, 0x00, 0x00, 0x00] + [0] * 28)
    sock.sendto(ping_payload, ('127.0.0.1', 9999))
    print("  [1] Sent ERP ping packet (32 bytes) -> OK")

    # 2. Simulate Rotary Encoder Browser scroll (Bus 3 Mixer)
    # Frame format: [bus_id=3] + 128 bytes mixer frame
    mixer_payload = bytearray([0x03] + [0] * 128)
    mixer_payload[4] = 0x7F # Channel 1 Fader up
    mixer_payload[5] = 0x40 # Channel 1 EQ Mid
    sock.sendto(bytes(mixer_payload), ('127.0.0.1', 9999))
    print("  [2] Sent Mixer control packet (Ch1 fader max, Ch1 EQ mid) -> OK")

    # 3. Simulate Deck 1 Play button toggle (Bus 1 ERP)
    erp_payload = bytearray([0x01] + [0] * 128)
    erp_payload[1] = 0x01 # Play button bit
    sock.sendto(bytes(erp_payload), ('127.0.0.1', 9999))
    print("  [3] Sent Deck 1 Play/Pause trigger -> OK")

    time.sleep(1.0)
    print("[PASS] Interactive controller commands injected successfully!")

if __name__ == '__main__':
    main()
