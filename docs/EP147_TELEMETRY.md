# AlphaTheta XDJ-AZ EP147 Internal Telemetry & State Extraction Architecture

## 1. Overview & Problem Analysis

Previously, deck playhead tracking and transport states relied on synthetic accumulators running outside the playback core:
- In `audio_shim.c`, an external counter (`g_deck_playhead_samples[c] += size`) accumulated ALSA buffer sizes whenever audio amplitudes were above threshold.
- This external logic suffered from severe limitations:
  1. **Looping Desynchronization**: When EP147 entered an active loop, the external counter continued incrementing linearly (e.g. 10s -> 20s -> 60s) rather than wrapping back to the Loop-In point.
  2. **Paused Jog Scrubbing Blindness**: When playback was paused or in vinyl touch halt, audio output was silent. The external accumulator completely froze, failing to track sample-accurate playhead scrubbing or cue-point searching.
  3. **Hot Cue & Seeker Desync**: Instant jumps via Hot Cue or touch strip did not update the synthetic counter.
  4. **Unloaded Deck False Telemetry**: An empty deck could report invalid playhead values.
  5. **Beat Sync & Master LED Decoding**: Master and Sync LEDs were evaluated using `>= 1`, causing inactive standby states to erroneously illuminate both indicators.

To achieve authentic Pioneer CDJ/XDJ behavior, playhead telemetry, loop states, Master, and Beat Sync must be obtained directly from EP147's memory and authentic SPI MOSI frames.

---

## 2. EP147 Memory Layout & Reverse Engineering

EP147 is a 64-bit ARM64 (AArch64) ELF executable built with JUCE and AlphaTheta DJ engine libraries (`dj_player`, `gui::player_info`). The main executable is loaded at a fixed virtual base (`0x400000`).

```
0x00400000 - 0x03b10000  r-xp  EP147 Code (.text)
0x03b1f000 - 0x03b25000  r--p  EP147 Read-Only Data (.rodata)
0x03b25000 - 0x03b41000  rw-p  EP147 Initialized Data (.data)
0x03b41000 - 0x03c4c000  rw-p  EP147 BSS Segment (.bss)
```

### 2.1 The JUCE Timer Singleton (`0x03bd7830`)

The global JUCE `TimerThread` singleton resides at a fixed virtual address in `.bss`:
```
0x03bd7830 : uintptr_t TimerThread_Instance
```
Dereferencing `*(uintptr_t*)0x03bd7830` yields the timer registry table:
- `registry + 0x1a0`: `uintptr_t lo` (Start of active timer pointers array)
- `registry + 0x1a8`: `uintptr_t hi` (End of active timer pointers array)
- Total registered timers: `(hi - lo) / sizeof(uintptr_t)`

### 2.2 Discovering `gui::player_info::LocalPlayer` (Vtable `0x2da1c28`)

Scanning the timer array reveals the four deck controllers instantiated by EP147. Each object exhibits:
- **Vtable**: `0x2da1c28` (`gui::player_info::LocalPlayer`)
- **Deck Index**: Located at `player + 0x38` (`uint32_t`, values `0, 1, 2, 3` corresponding to Decks 1, 2, 3, 4).
- **Source Engine Pointer**: Located at `player + 0xb0` (and mirrored at `player + 0x168`).

### 2.3 Audio Playback Engine: `dj_player::PlayerStateWithUpdate` (Vtable `0x25eb390`)

The pointer at `player + 0xb0` references the authentic playback state object:
- **RTTI Mangled**: `N9dj_player21PlayerStateWithUpdateE`
- **Inheritance**: Derived from `dj_player::PlayerState`
- **Vtable**: `0x25eb390`

```
gui::player_info::LocalPlayer (vtable: 0x2da1c28)
├── +0x038: uint32_t deck_index (0..3)
└── +0x0b0: dj_player::PlayerStateWithUpdate* (vtable: 0x25eb390)
            ├── +0x010: uint64_t deck_index
            ├── +0x018: uint64_t sample_rate (44100 Hz)
            └── +0x0e8: int64_t playhead_sample_position
```

---

## 3. Sample-Accurate Playhead & Looping Semantics

The field at `src + 0xe8` represents the native sample playhead position rendered by the audio engine:

### 3.1 Unloaded Deck Sentinel
When no track is loaded on a deck:
```
*(int64_t*)(src + 0xe8) == 0x7FFFFFFFFFFFFFFF  (INT64_MAX)
```
When this sentinel is detected, the bridge safely sets:
```c
g_shm->playhead_pos[d] = 0;
g_shm->track_loaded[d] = 0;
```

### 3.2 Active Track Playhead
When a track is loaded:
```
0 <= *(int64_t*)(src + 0xe8) < 0x7FFFFFFFFFFFFFFF
```
- Sample rate is 44,100 Hz.
- Playhead in seconds: `seconds = pos / 44100.0`.

### 3.3 Authentic Looping Boundary Wrapping
Unlike external counters, `src + 0xe8` is managed directly by EP147's DSP playback graph:
- When playback reaches the Loop-Out sample boundary, EP147 instantly updates `src + 0xe8` back to the Loop-In sample.
- The jog wheel ring display (`pos % 79380` corresponding to 1 revolution = 1.8s at 33 1/3 RPM) naturally wraps seamlessly during active looping without cumulative drift or external heuristics.

### 3.4 Paused & Jog Scratching Telemetry
When playback is stopped (`is_playing == 0`):
- Turning the jog wheel sends jog scratch / pitch bend ticks to EP147 via SPI MISO.
- EP147 updates `src + 0xe8` with sample precision on every jog tick.
- The controller bridge reads this movement in real time at 100 Hz, driving the FLX6 jog display LEDs with zero latency.

---

## 4. Hardware SPI Telemetry Decoding (ErP MOSI)

EP147 outputs 152-byte SPI MOSI frames to Bus 0 (`SUBUCOM_DEV_ERP` / `spi1.0`) at 100 Hz. These frames contain authentic 2-bit level indicators for hardware LEDs:

Each platter side occupies a stride of `0x38` bytes (Side 0 = Decks 1/3, Side 1 = Decks 2/4):

| Offset | Bitmask | Field | Value Decoding |
|---|---|---|---|
| `0x08 + stride` | `>> 4 & 0x03` | **Master LED** | `2` = Tempo Master (Solid ON), `1` = Standby / Inactive (OFF), `0` = OFF |
| `0x08 + stride` | `>> 2 & 0x03` | **Master Tempo** | `2` = Key Lock Active, `0` = Inactive |
| `0x09 + stride` | `>> 6 & 0x03` | **Beat Sync LED** | `2` = Beat Sync Engaged (Solid ON), `1` = Armed / Standby (Blinking), `0` = OFF |
| `0x09 + stride` | `>> 0 & 0x03` | **Reloop LED** | `2` = Reloop Available, `0` = None |
| `0x0A + stride` | `>> 6 & 0x03` | **Loop In LED** | `2` = Solid ON, `1` = Blinking / Adjusting, `0` = OFF |
| `0x0A + stride` | `>> 4 & 0x03` | **Loop Out LED** | `2` = Active Loop Engaged, `0` = Loop Exited |
| `0x0B + stride` | `>> 2 & 0x03` | **Cue LED** | `2` = Cue Point Set (Solid), `1` = Blinking, `0` = Dark |
| `0x0B + stride` | `>> 0 & 0x03` | **Play/Pause LED**| `2` = Playing (Solid), `1` = Paused (Blinking), `0` = Dark |

---

## 5. Controller Mapping Specifications (DDJ-FLX6)

| Physical Control | MIDI Channel / Note | Target Function |
|---|---|---|
| **SHIFT + DECK 3** (Left) | `CH_MIX` (Ch 6), Note 124 | Toggles **SLIP Mode** (`K_SLIP`) on `active_left_deck` (Deck 1 or 3) |
| **SHIFT + DECK 4** (Right) | `CH_MIX` (Ch 6), Note 125 | Toggles **SLIP Mode** (`K_SLIP`) on `active_right_deck` (Deck 2 or 4) |
| **SHIFT + JOG CUTTER** | Ch 0..3, Note 23 (`0x17`) | Toggles **Vinyl / CDJ Mode** (`K_VINYL`) on active deck |
| **DECK 3 / 4** (Unshifted) | Ch 0..3, Note 114 | Toggles active platter deck: Left (1 <-> 3), Right (2 <-> 4) |

---

## 6. Implementation Architecture

### 6.1 In-Process Memory Poller (`libsubucom_emu.so`)
Because `libsubucom_emu.so` is preloaded directly into EP147 via `LD_PRELOAD`:
- It shares the exact same virtual address space as EP147.
- Reading `*(volatile int64_t*)(src + 0xe8)` requires no IPC, syscalls, or `/proc/pid/mem` overhead.
- Cached lookups complete in under 50 nanoseconds per cycle.
- Polling runs concurrently inside `subucom_server_worker` at 100 Hz and on every SPI MOSI frame.

### 6.2 Shared Memory (`/dev/shm/xdj_az_shared`)
Telemetry values are published into `xdj_az_shared_t`:
```c
g_shm->playhead_pos[d] = (uint64_t)pos;
g_shm->track_loaded[d] = (pos != 0x7FFFFFFFFFFFFFFFULL && pos >= 0);
g_shm->is_master[d]    = (master_lvl == 2);
g_shm->is_sync[d]      = (sync_lvl == 2);
g_shm->is_looping[d]   = (out_lvl == 2);
```

All external synthetic playhead accumulators in `audio_shim.c` have been fully retired.

