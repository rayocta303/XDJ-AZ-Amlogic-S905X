#!/usr/bin/env python3
"""
send_panel.py — Master Pioneer XDJ-AZ Panel & Hardware Control Injector
Controls every aspect of AlphaTheta / Pioneer XDJ-AZ EP147 via Subucom SPI emulation:
- Views: source, browse, waveform, menu/utility, taglist, playlist, search, shortcut
- Library Browser: scroll up/down/delta, push/enter, back
- Decks 1..4 Loading: load1, load2, load3, load4
- Decks 1..4 Transport: play, cue, sync, master, slip, tempo/pitch, jog nudge/touch
- Decks 1..4 Performance Pads: pad 1..8, pad_mode (hotcue, beatloop, sliploop, beatjump)
- Decks 1..4 Loops & Navigation: loop_in, loop_out, reloop, loop4, call_prev, call_next, mem_cue, track_prev, track_next
- 4-Channel Mixer: faders 1..4, crossfader, trims 1..4, 3-band EQs 1..4, Sound Color FX (filter) 1..4
- Headphone & Master: cue 1..4, master_cue, master_vol, hp_mix, hp_level
"""

import os
import sys
import time
import socket
import json

SUBUCOM_PORT = 9999
SUBUCOM_HOST = '127.0.0.1'
STATE_FILE = '/tmp/xdj_panel_state.json'

# --- Authentic Captured Hardware Frames (from xdj_az_abi64.h) ---
IDLE_MIXER = bytearray([
    0x03, 0x01, 0x02, 0x01, 0x01, 0x01, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x7C, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
    0x7B, 0x80, 0x80, 0x80, 0x40, 0x7B, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80,
    0x80, 0x80, 0xC0, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x80, 0x80, 0x80,
    0x00, 0x80, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x80, 0x2A, 0x02, 0xFF,
    0x00, 0x80, 0x00, 0xC0, 0x80, 0x79, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x0A, 0x00, 0xB0, 0x04, 0xF4, 0x01, 0x00, 0x58, 0x00, 0x00,
    0x52, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
])

IDLE_ERP = bytearray([
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x39, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
])

IDLE_DECK1 = bytearray([
    0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x44, 0x00, 0x00, 0x00, 0x68, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x46, 0x02, 0x05, 0x02, 0xC2, 0x00, 0x69, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x8E, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
])

IDLE_DECK2 = bytearray([
    0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x75, 0x02, 0x06, 0x02, 0xCD, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x26, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
])

# --- State Persistence ---
class PanelState:
    def __init__(self):
        self.mixer = bytearray(IDLE_MIXER)
        self.erp = bytearray(IDLE_ERP)
        self.deck1 = bytearray(IDLE_DECK1)
        self.deck2 = bytearray(IDLE_DECK2)
        self.knob_ctr = 128
        self.jog_pos = [0, 0, 0, 0]
        self.load()

    def load(self):
        if os.path.exists(STATE_FILE):
            try:
                with open(STATE_FILE, 'r') as f:
                    d = json.load(f)
                self.mixer = bytearray.fromhex(d['mixer'])
                self.erp = bytearray.fromhex(d['erp'])
                self.deck1 = bytearray.fromhex(d['deck1'])
                self.deck2 = bytearray.fromhex(d['deck2'])
                self.knob_ctr = d.get('knob_ctr', 128)
                self.jog_pos = d.get('jog_pos', [0, 0, 0, 0])
            except Exception:
                pass

    def save(self):
        d = {
            'mixer': self.mixer.hex(),
            'erp': self.erp.hex(),
            'deck1': self.deck1.hex(),
            'deck2': self.deck2.hex(),
            'knob_ctr': self.knob_ctr,
            'jog_pos': self.jog_pos
        }
        try:
            with open(STATE_FILE, 'w') as f:
                json.dump(d, f)
        except Exception:
            pass

def send_frame(bus_id, frame):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(bytes([bus_id]) + bytes(frame), (SUBUCOM_HOST, SUBUCOM_PORT))
    s.close()

def clear_bus(bus_id):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(bytes([bus_id]), (SUBUCOM_HOST, SUBUCOM_PORT))
    s.close()

def set10(f, hi, lo, shift, v):
    v = max(0, min(1023, int(v)))
    f[hi] = (v >> 2) & 0xFF
    f[lo] = (f[lo] & ~(0x03 << shift)) | ((v & 0x03) << shift)

# --- Command Handlers ---
state = PanelState()

def cmd_view(tab_name):
    # Byte 32: 0x80=SOURCE, 0x40=BROWSE, 0x20=TAG_LIST, 0x10=PLAYLIST, 0x08=SEARCH, 0x04=MENU
    masks = {
        'source': 0x80,
        'browse': 0x40,
        'menu': 0x04,
        'utility': 0x04,
        'taglist': 0x20,
        'playlist': 0x10,
        'search': 0x08,
    }
    mask = masks.get(tab_name.lower())
    if not mask:
        print(f"Unknown view: {tab_name}. Available: source, browse, menu/utility, taglist, playlist, search, waveform")
        return

    hold = 0.5 if tab_name.lower() in ('menu', 'utility') else 0.2
    f = bytearray(state.mixer)
    f[6] = 0x00 # Latch pressed
    f[32] = mask
    send_frame(2, f)
    time.sleep(hold)
    f[6] = 0x08 # Latch idle
    f[32] = 0x00
    send_frame(2, f)
    state.mixer[32] = 0x00
    state.save()
    print(f"Switched view -> {tab_name}")

def cmd_waveform():
    # To return to Waveform view from Browse or Utility:
    # 1. If in Utility, press menu (hold 0.5)
    # 2. If in Browse, touch top-left Back (x=100, y=50) or send Back button
    f = bytearray(state.mixer)
    f[6] = 0x00
    f[33] = 0x80 # Corner Back button
    send_frame(2, f)
    time.sleep(0.15)
    f[6] = 0x08
    f[33] = 0x00
    send_frame(2, f)
    state.save()
    print("Navigated back to Waveform view")

def cmd_back():
    f = bytearray(state.mixer)
    f[6] = 0x00
    f[33] = 0x80 # Back button
    send_frame(2, f)
    time.sleep(0.15)
    f[6] = 0x08
    f[33] = 0x00
    send_frame(2, f)
    state.save()
    print("Pressed BACK button")

def cmd_enter():
    f = bytearray(state.mixer)
    f[6] = 0x00
    f[31] = 0x01 # Knob push
    send_frame(2, f)
    time.sleep(0.15)
    f[6] = 0x08
    f[31] = 0x00
    send_frame(2, f)
    state.save()
    print("Pressed BROWSE knob (Enter)")

def cmd_scroll(delta):
    state.knob_ctr = (state.knob_ctr + delta) & 0xFF
    f = bytearray(state.mixer)
    f[34] = state.knob_ctr
    send_frame(2, f)
    state.mixer[34] = state.knob_ctr
    state.save()
    print(f"Scrolled browse encoder: delta={delta:+d} (ctr={state.knob_ctr})")

def cmd_load(deck_num):
    # Byte 33: 0x08=LOAD1, 0x04=LOAD2, 0x02=LOAD3, 0x01=LOAD4
    masks = {1: 0x08, 2: 0x04, 3: 0x02, 4: 0x01}
    mask = masks.get(deck_num)
    if not mask:
        print(f"Invalid deck {deck_num}. Use 1..4.")
        return
    f = bytearray(state.mixer)
    f[6] = 0x00
    f[33] = mask
    send_frame(2, f)
    time.sleep(0.2)
    f[6] = 0x08
    f[33] = 0x00
    send_frame(2, f)
    state.save()
    print(f"Loaded selected track into Deck {deck_num}")

def cmd_transport(deck_num, action):
    # Bus 0 = ErP. Deck 1/3 at off 8..15, Deck 2/4 at off 16..23 (stride 8)
    side = 0 if deck_num in (1, 3) else 1
    d_off = side * 8
    f = bytearray(state.erp)

    # Deck select secondary (Deck 3 or Deck 4)
    if deck_num == 3:
        f[9] |= 0x01
        f[8] &= ~0x01
    elif deck_num == 1:
        f[8] |= 0x01
        f[9] &= ~0x01
    elif deck_num == 4:
        f[17] |= 0x01
        f[16] &= ~0x01
    elif deck_num == 2:
        f[16] |= 0x01
        f[17] &= ~0x01

    if action == 'play':
        f[10 + d_off] |= 0x02
        send_frame(0, f)
        time.sleep(0.15)
        f[10 + d_off] &= ~0x02
        send_frame(0, f)
        print(f"Toggled Play/Pause on Deck {deck_num}")
    elif action == 'cue':
        f[10 + d_off] |= 0x04
        send_frame(0, f)
        time.sleep(0.15)
        f[10 + d_off] &= ~0x04
        send_frame(0, f)
        print(f"Triggered Cue on Deck {deck_num}")
    elif action == 'sync':
        f[8 + d_off] |= 0x02
        send_frame(0, f)
        time.sleep(0.15)
        f[8 + d_off] &= ~0x02
        send_frame(0, f)
        print(f"Toggled Beat Sync on Deck {deck_num}")
    elif action == 'master':
        f[8 + d_off] |= 0x10
        send_frame(0, f)
        time.sleep(0.15)
        f[8 + d_off] &= ~0x10
        send_frame(0, f)
        print(f"Toggled Master on Deck {deck_num}")
    elif action == 'slip':
        f[8 + d_off] |= 0x04
        send_frame(0, f)
        time.sleep(0.15)
        f[8 + d_off] &= ~0x04
        send_frame(0, f)
        print(f"Toggled Slip mode on Deck {deck_num}")
    elif action == 'loop_in':
        f[8 + d_off] |= 0x40
        send_frame(0, f)
        time.sleep(0.15)
        f[8 + d_off] &= ~0x40
        send_frame(0, f)
        print(f"Loop IN pressed on Deck {deck_num}")
    elif action == 'loop_out':
        f[8 + d_off] |= 0x20
        send_frame(0, f)
        time.sleep(0.15)
        f[8 + d_off] &= ~0x20
        send_frame(0, f)
        print(f"Loop OUT pressed on Deck {deck_num}")
    elif action == 'reloop':
        f[9 + d_off] |= 0x08
        send_frame(0, f)
        time.sleep(0.15)
        f[9 + d_off] &= ~0x08
        send_frame(0, f)
        print(f"Reloop/Exit pressed on Deck {deck_num}")
    elif action == 'loop4':
        f[12 + d_off] |= 0x04
        send_frame(0, f)
        time.sleep(0.15)
        f[12 + d_off] &= ~0x04
        send_frame(0, f)
        print(f"Auto 4-Beat Loop pressed on Deck {deck_num}")
    elif action == 'call_prev':
        f[12 + d_off] |= 0x20
        send_frame(0, f)
        time.sleep(0.15)
        f[12 + d_off] &= ~0x20
        send_frame(0, f)
        print(f"Cue Call < pressed on Deck {deck_num}")
    elif action == 'call_next':
        f[12 + d_off] |= 0x10
        send_frame(0, f)
        time.sleep(0.15)
        f[12 + d_off] &= ~0x10
        send_frame(0, f)
        print(f"Cue Call > pressed on Deck {deck_num}")
    elif action == 'mem_cue':
        f[12 + d_off] |= 0x80
        send_frame(0, f)
        time.sleep(0.15)
        f[12 + d_off] &= ~0x80
        send_frame(0, f)
        print(f"Memory Cue pressed on Deck {deck_num}")
    elif action == 'track_prev':
        f[11 + d_off] |= 0x08
        send_frame(0, f)
        time.sleep(0.15)
        f[11 + d_off] &= ~0x08
        send_frame(0, f)
        print(f"Track Prev pressed on Deck {deck_num}")
    elif action == 'track_next':
        f[11 + d_off] |= 0x04
        send_frame(0, f)
        time.sleep(0.15)
        f[11 + d_off] &= ~0x04
        send_frame(0, f)
        print(f"Track Next pressed on Deck {deck_num}")

    state.erp = f
    state.save()

def cmd_pad(deck_num, pad_num):
    side = 0 if deck_num in (1, 3) else 1
    d_off = side * 8
    pad_idx = max(1, min(8, int(pad_num))) - 1
    f = bytearray(state.erp)
    f[14 + d_off] = (1 << pad_idx)
    send_frame(0, f)
    time.sleep(0.15)
    f[14 + d_off] = 0x00
    send_frame(0, f)
    state.erp = f
    state.save()
    print(f"Triggered Performance Pad {pad_num} on Deck {deck_num}")

def cmd_pad_mode(deck_num, mode_name):
    # 0=HotCue, 1=BeatLoop, 2=SlipLoop, 3=BeatJump
    side = 0 if deck_num in (1, 3) else 1
    d_off = side * 8
    modes = {'hotcue': 0x01, 'beatloop': 0x02, 'sliploop': 0x04, 'beatjump': 0x08}
    mask = modes.get(mode_name.lower(), 0x01)
    f = bytearray(state.erp)
    f[13 + d_off] = mask
    send_frame(0, f)
    state.erp = f
    state.save()
    print(f"Set Pad Mode on Deck {deck_num} -> {mode_name}")

def cmd_fader(ch, val_pct):
    # val_pct: 0..100 (%) or 0..1023
    val = float(val_pct)
    if val <= 100.0:
        val10 = int((val / 100.0) * 1023)
    else:
        val10 = int(val)
    fader_regs = {
        1: (0x0A, 0x0E, 6),
        2: (0x0B, 0x0E, 4),
        3: (0x0C, 0x0E, 2),
        4: (0x0D, 0x0E, 0),
    }
    reg = fader_regs.get(int(ch))
    if not reg:
        print(f"Invalid channel {ch}. Use 1..4.")
        return
    f = bytearray(state.mixer)
    set10(f, reg[0], reg[1], reg[2], val10)
    send_frame(2, f)
    state.mixer = f
    state.save()
    print(f"Channel {ch} Fader -> {val_pct}% (10-bit: {val10})")

def cmd_xfader(val):
    # val: 'A' (0), 'B' (1023), 'center' (512), or 0..100%
    if str(val).lower() == 'a':
        val10 = 0
    elif str(val).lower() == 'b':
        val10 = 1023
    elif str(val).lower() in ('center', 'c', 'mid'):
        val10 = 512
    else:
        v = float(val)
        if v <= 100.0:
            val10 = int((v / 100.0) * 1023)
        else:
            val10 = int(v)
    f = bytearray(state.mixer)
    set10(f, 0x0F, 0x10, 0, val10)
    send_frame(2, f)
    state.mixer = f
    state.save()
    print(f"Crossfader -> {val} (10-bit: {val10})")

def cmd_eq(ch, hi_pct, mid_pct, low_pct):
    ch = int(ch)
    regs = {
        1: {'hi': (0x25, 0x28, 4), 'mid': (0x26, 0x28, 2), 'low': (0x27, 0x28, 0)},
        2: {'hi': (0x2A, 0x2D, 4), 'mid': (0x2B, 0x2D, 2), 'low': (0x2C, 0x2D, 0)},
        3: {'hi': (0x2F, 0x32, 4), 'mid': (0x30, 0x32, 2), 'low': (0x31, 0x32, 0)},
        4: {'hi': (0x34, 0x37, 4), 'mid': (0x35, 0x37, 2), 'low': (0x36, 0x37, 0)},
    }
    r = regs.get(ch)
    if not r:
        print(f"Invalid channel {ch}. Use 1..4.")
        return
    f = bytearray(state.mixer)
    h10 = int((float(hi_pct) / 100.0) * 1023)
    m10 = int((float(mid_pct) / 100.0) * 1023)
    l10 = int((float(low_pct) / 100.0) * 1023)
    set10(f, r['hi'][0], r['hi'][1], r['hi'][2], h10)
    set10(f, r['mid'][0], r['mid'][1], r['mid'][2], m10)
    set10(f, r['low'][0], r['low'][1], r['low'][2], l10)
    send_frame(2, f)
    state.mixer = f
    state.save()
    print(f"Channel {ch} EQ -> HI={hi_pct}%, MID={mid_pct}%, LOW={low_pct}%")

def cmd_trim(ch, val_pct):
    ch = int(ch)
    regs = {1: (0x24, 0x28, 6), 2: (0x29, 0x2D, 6), 3: (0x2E, 0x32, 6), 4: (0x33, 0x37, 6)}
    r = regs.get(ch)
    if not r: return
    val10 = int((float(val_pct) / 100.0) * 1023)
    f = bytearray(state.mixer)
    set10(f, r[0], r[1], r[2], val10)
    send_frame(2, f)
    state.mixer = f
    state.save()
    print(f"Channel {ch} Trim -> {val_pct}% (10-bit: {val10})")

def cmd_color(ch, val_pct):
    ch = int(ch)
    regs = {1: (0x3D, 0x41, 6), 2: (0x3E, 0x41, 4), 3: (0x3F, 0x41, 2), 4: (0x40, 0x41, 0)}
    r = regs.get(ch)
    if not r: return
    val10 = int((float(val_pct) / 100.0) * 1023)
    f = bytearray(state.mixer)
    set10(f, r[0], r[1], r[2], val10)
    send_frame(2, f)
    state.mixer = f
    state.save()
    print(f"Channel {ch} Sound Color FX (Filter) -> {val_pct}% (10-bit: {val10})")

def cmd_tempo(deck_num, pct):
    # -100% to +100%, center = 0.0%. Centered at 517 (range ~11..1023)
    deck_num = int(deck_num)
    bus = 3 if deck_num in (1, 3) else 1
    raw = int(517 + (float(pct) / 100.0) * 506)
    raw = max(11, min(1023, raw))
    f = bytearray(state.deck1 if bus == 3 else state.deck2)
    f[16] = raw & 0xFF
    f[17] = (raw >> 8) & 0xFF
    send_frame(bus, f)
    if bus == 3: state.deck1 = f
    else: state.deck2 = f
    state.save()
    print(f"Deck {deck_num} Tempo Slider -> {pct:+.2f}% (raw register: {raw})")

def cmd_jog(deck_num, delta):
    deck_num = int(deck_num)
    bus = 3 if deck_num in (1, 3) else 1
    idx = deck_num - 1
    state.jog_pos[idx] = (state.jog_pos[idx] + int(delta)) & 0xFFFF
    f = bytearray(state.deck1 if bus == 3 else state.deck2)
    f[9] = 0x0F if int(delta) > 0 else 0x0B # Touch turning
    f[10] = state.jog_pos[idx] & 0xFF
    f[11] = (state.jog_pos[idx] >> 8) & 0xFF
    f[12] = 450 & 0xFF # Turning period
    f[13] = (450 >> 8) & 0xFF
    send_frame(bus, f)
    time.sleep(0.08)
    f[9] = 0x00 # Touch release
    f[12] = 0xFF
    f[13] = 0xFF # Stopped period
    send_frame(bus, f)
    if bus == 3: state.deck1 = f
    else: state.deck2 = f
    state.save()
    print(f"Deck {deck_num} Jog rotation delta: {int(delta):+d} (pos={state.jog_pos[idx]})")

def cmd_touch(deck_num):
    deck_num = int(deck_num)
    bus = 3 if deck_num in (1, 3) else 1
    f = bytearray(state.deck1 if bus == 3 else state.deck2)
    f[9] = 0x0F # Touch hold
    f[12] = 0xFF
    f[13] = 0xFF # Stopped period
    send_frame(bus, f)
    if bus == 3: state.deck1 = f
    else: state.deck2 = f
    state.save()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sendto(f"TOUCH {deck_num}".encode(), ('127.0.0.1', 9996))
        s.close()
    except Exception:
        pass
    print(f"Deck {deck_num} Platter Touch -> TOUCHED (Vinyl scratch freeze)")

def cmd_release(deck_num):
    deck_num = int(deck_num)
    bus = 3 if deck_num in (1, 3) else 1
    f = bytearray(state.deck1 if bus == 3 else state.deck2)
    f[9] = 0x00 # Touch release
    f[12] = 0xFF
    f[13] = 0xFF # Stopped period
    send_frame(bus, f)
    if bus == 3: state.deck1 = f
    else: state.deck2 = f
    state.save()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sendto(f"RELEASE {deck_num}".encode(), ('127.0.0.1', 9996))
        s.close()
    except Exception:
        pass
    print(f"Deck {deck_num} Platter Touch -> RELEASED (Playback resumes)")

def print_help():
    print("""
AlphaTheta XDJ-AZ Command Injector (send_panel.py)
Usage: python3 send_panel.py <command> [args...]

VIEWS & NAVIGATION:
  source                   Switch to SOURCE view
  browse                   Switch to BROWSE / Library view
  waveform                 Return to WAVEFORM view
  menu | utility           Toggle UTILITY / Settings view (500ms hold)
  taglist                  Switch to TAG LIST view
  playlist                 Switch to PLAYLIST view
  search                   Switch to SEARCH view
  shortcut                 Toggle SHORTCUT menu
  back                     Press BACK button
  enter | push             Press Browse Rotary Push (Open / Select)
  scroll <delta>           Rotate browse knob by delta (+1 down, -1 up, etc.)
  scroll_down              Rotate browse knob down (+1)
  scroll_up                Rotate browse knob up (-1)

DECK LOADING:
  load1 | load 1           Load selected track to Deck 1
  load2 | load 2           Load selected track to Deck 2
  load3 | load 3           Load selected track to Deck 3
  load4 | load 4           Load selected track to Deck 4

DECK TRANSPORT & CONTROL:
  play <deck>              Toggle Play/Pause on Deck 1..4
  cue <deck>               Press Cue on Deck 1..4
  sync <deck>              Toggle Beat Sync on Deck 1..4
  master <deck>            Toggle Master on Deck 1..4
  slip <deck>              Toggle Slip mode on Deck 1..4
  loop_in <deck>           Press Loop IN on Deck 1..4
  loop_out <deck>          Press Loop OUT on Deck 1..4
  reloop <deck>            Press Reloop/Exit on Deck 1..4
  loop4 <deck>             Press Auto 4-Beat Loop on Deck 1..4
  call_prev <deck>         Cue/Loop Call < on Deck 1..4
  call_next <deck>         Cue/Loop Call > on Deck 1..4
  mem_cue <deck>           Memory Cue on Deck 1..4
  track_prev <deck>        Track Search < on Deck 1..4
  track_next <deck>        Track Search > on Deck 1..4
  pad <deck> <1..8>        Trigger Performance Pad 1..8 on Deck 1..4
  pad_mode <deck> <mode>   Set Pad Mode: hotcue, beatloop, sliploop, beatjump
  tempo <deck> <pct>       Set Tempo Slider (-100% to +100%, 0=center)
  jog <deck> <delta>       Nudge / scratch Jog Wheel (+/- delta ticks)

4-CHANNEL MIXER:
  fader <ch> <0..100%>     Set Channel 1..4 Fader level
  xfader <val>             Set Crossfader (A, B, center, or 0..100%)
  eq <ch> <hi> <mid> <low> Set 3-Band EQ (0..100%, 50=center)
  trim <ch> <0..100%>      Set Channel 1..4 Trim/Gain
  color <ch> <0..100%>     Set Channel 1..4 Sound Color FX (Filter, 50=center)

SYSTEM:
  reset                    Reset all shims back to authentic factory idle
""")

def main():
    if len(sys.argv) < 2:
        print_help()
        return

    cmd = sys.argv[1].lower()

    if cmd in ('source', 'browse', 'menu', 'utility', 'taglist', 'playlist', 'search'):
        cmd_view(cmd)
    elif cmd == 'waveform':
        cmd_waveform()
    elif cmd == 'back':
        cmd_back()
    elif cmd in ('enter', 'push'):
        cmd_enter()
    elif cmd == 'scroll':
        delta = int(sys.argv[2]) if len(sys.argv) > 2 else 1
        cmd_scroll(delta)
    elif cmd == 'scroll_down':
        cmd_scroll(1)
    elif cmd == 'scroll_up':
        cmd_scroll(-1)
    elif cmd in ('load1', 'load2', 'load3', 'load4'):
        cmd_load(int(cmd[-1]))
    elif cmd == 'load' and len(sys.argv) > 2:
        cmd_load(int(sys.argv[2]))
    elif cmd in ('play', 'cue', 'sync', 'master', 'slip', 'loop_in', 'loop_out', 'reloop', 'loop4',
                 'call_prev', 'call_next', 'mem_cue', 'track_prev', 'track_next'):
        deck = int(sys.argv[2]) if len(sys.argv) > 2 else 1
        cmd_transport(deck, cmd)
    elif cmd in ('play1', 'play2', 'play3', 'play4'):
        cmd_transport(int(cmd[-1]), 'play')
    elif cmd in ('cue1', 'cue2', 'cue3', 'cue4'):
        cmd_transport(int(cmd[-1]), 'cue')
    elif cmd == 'pad' and len(sys.argv) > 3:
        cmd_pad(int(sys.argv[2]), int(sys.argv[3]))
    elif cmd == 'pad_mode' and len(sys.argv) > 3:
        cmd_pad_mode(int(sys.argv[2]), sys.argv[3])
    elif cmd == 'fader' and len(sys.argv) > 3:
        cmd_fader(int(sys.argv[2]), sys.argv[3])
    elif cmd == 'xfader' and len(sys.argv) > 2:
        cmd_xfader(sys.argv[2])
    elif cmd == 'eq' and len(sys.argv) > 5:
        cmd_eq(int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5])
    elif cmd == 'trim' and len(sys.argv) > 3:
        cmd_trim(int(sys.argv[2]), sys.argv[3])
    elif cmd == 'color' and len(sys.argv) > 3:
        cmd_color(int(sys.argv[2]), sys.argv[3])
    elif cmd == 'tempo' and len(sys.argv) > 3:
        cmd_tempo(int(sys.argv[2]), sys.argv[3])
    elif cmd == 'jog' and len(sys.argv) > 3:
        cmd_jog(int(sys.argv[2]), sys.argv[3])
    elif cmd == 'touch' and len(sys.argv) > 2:
        cmd_touch(int(sys.argv[2]))
    elif cmd == 'release' and len(sys.argv) > 2:
        cmd_release(int(sys.argv[2]))
    elif cmd == 'reset':
        clear_bus(0)
        clear_bus(1)
        clear_bus(2)
        clear_bus(3)
        if os.path.exists(STATE_FILE): os.remove(STATE_FILE)
        print("Reset all hardware buses to factory idle state.")
    else:
        print_help()

if __name__ == '__main__':
    main()
