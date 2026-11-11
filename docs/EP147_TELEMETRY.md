# AlphaTheta XDJ-AZ EP147 Internal Telemetry & State Extraction Architecture

## 1. Overview & Problem Analysis

Previously, deck playhead tracking, transport states, and mixer interactions relied on synthetic accumulators and decoupled heuristics running outside Pioneer's playback core:
- In `audio_shim.c`, an external counter (`g_deck_playhead_samples[c] += size`) accumulated ALSA buffer sizes whenever audio amplitudes were above threshold.
- This external logic suffered from severe limitations:
  1. **Looping Desynchronization**: When EP147 entered an active loop, external counters continued incrementing linearly (e.g. 10s -> 20s -> 60s) rather than wrapping back to the Loop-In point.
  2. **Paused Jog Scrubbing Blindness**: When playback was paused or in vinyl touch halt, audio output was silent. External accumulators completely froze, failing to track sample-accurate playhead scrubbing or cue-point searching.
  3. **Hot Cue & Seeker Desync**: Instant jumps via Hot Cue or touch strip did not update synthetic counters.
  4. **Unloaded Deck False Telemetry**: An empty deck could report invalid playhead values.
  5. **Beat Sync & Master LED Decoding**: Master and Sync LEDs were evaluated using `>= 1`, causing inactive standby states to erroneously illuminate both indicators.
  6. **Beat FX & X-PAD Disconnect**: Effects and touch ribbon modulation operated blindly without syncing to EP147's internal effect slots, snapping rungs, or screen views.

To achieve authentic Pioneer CDJ/XDJ behavior, playhead telemetry, loop states, Master/Sync LEDs, Beat FX, and X-PAD touch states are extracted directly from:
- **In-process EP147 memory structures** (JUCE Timer registry, `LocalPlayer`, `PlayerStateWithUpdate`).
- **Authentic SPI MOSI & MISO hardware frames** across four physical sub-CPU buses (ErP, Deck 1, Deck 2, Mixer MC).
- **In-process X11 framebuffer inspection** for dynamic screen-view detection and touch hit-testing.

---

## 2. EP147 Memory Layout & Direct In-Process State Extraction

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
            ├── +0x040: uint32_t track_loaded_flag (nonzero = loaded)
            └── +0x0e8: int64_t playhead_sample_position
```

### 2.4 Sample-Accurate Playhead & Looping Semantics

The field at `src + 0xe8` represents the native sample playhead position rendered by the audio engine:

#### Unloaded Deck Sentinel
When no track is loaded on a deck:
```
*(int64_t*)(src + 0xe8) == 0x7FFFFFFFFFFFFFFF  (INT64_MAX)
```
When this sentinel is detected or `*(uint32_t*)(src + 0x40) == 0`, the bridge safely publishes:
```c
g_shm->playhead_pos[d] = 0;
g_shm->track_loaded[d] = 0;
```

#### Active Track Playhead & Negative Lead-in
When a track is loaded:
```
*(int64_t*)(src + 0xe8) != 0x7FFFFFFFFFFFFFFF
```
- Sample rate is 44,100 Hz (`seconds = pos / 44100.0`).
- **Signed Negative Positions (Pre-gap / Lead-in)**: When rotating the jog wheel backward before 0:00 (the initial beat grid marker), `pos` becomes negative (e.g. `-1000` samples).
- Telemetry preserves `track_loaded = 1` and passes signed `int64_t pos` into shared memory.
- Jog display ring logic wraps backwards seamlessly:
  ```c
  int64_t p = pos % 79380;
  if (p < 0) p += 79380;
  ```
  This prevents shutting off the ring LED or falsely triggering a track-load animation upon crossing beat 0.

#### Authentic Looping Boundary Wrapping
Unlike external counters, `src + 0xe8` is managed directly by EP147's DSP playback graph:
- When playback reaches the Loop-Out sample boundary, EP147 instantly wraps `src + 0xe8` back to the Loop-In sample.
- The jog wheel ring display (`pos % 79380` corresponding to 1 revolution = 1.8s at 33 1/3 RPM) naturally wraps seamlessly during active looping without cumulative drift or external heuristics.

#### Paused & Jog Scratching Telemetry
When playback is stopped (`is_playing == 0`):
- Turning the jog wheel sends jog scratch / pitch bend ticks to EP147 via SPI MISO.
- EP147 updates `src + 0xe8` with sample precision on every jog tick.
- The controller bridge reads this movement in real time at 100 Hz, driving external jog display LEDs with zero latency.

---

## 3. Subucom Front-Panel Hardware SPI Telemetry Architecture

AlphaTheta XDJ-AZ front-panel communications are divided across four physical sub-CPU SPI buses:

| Bus ID | Linux Device Node | Sub-CPU Target | Payload Function |
|---|---|---|---|
| **Bus 0** | `/dev/subucom_spi1.0` | **ErP** (MPNL) | Decks 1-4 transport buttons, pads, loop controls, master/sync/cue/play LEDs |
| **Bus 1** | `/dev/subucom_spi2.0` | **Deck 2** | Right platter jog touch, 16-bit position, rotation period, tempo slider |
| **Bus 2** | `/dev/subucom_spi3.0` | **Mixer MC** | Mixer faders, trims, EQs, color FX, browse rotary, panel buttons, Beat FX, X-PAD |
| **Bus 3** | `/dev/subucom_spi5.0` | **Deck 1** | Left platter jog touch, 16-bit position, rotation period, tempo slider |
| **Control** | `/dev/subucom_ctrl` | **Virtual** | Inter-process frame injection, UDP socket bridge (9999), and LED extraction |

### 3.1 Frame Sizes, Timing & CRC-16/X-25 Protocol

- **Buffer Size**: EP147 reads and writes 152 bytes per transfer (128 active payload bytes + 24 zero padding).
- **Polling Rate**: ~850 Hz (1,176 $\mu\text{s}$ interval via `IOCTL_INTERVAL_WRITE`).
- **CRC Algorithm**: CRC-16/X-25 with polynomial `0x8408` (reflected `0x1021`), Initial Seed `0xFFFF`, Final XOR `0xFFFF`.

```c
uint16_t subucom_crc16_x25(const uint8_t *data, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0x8408) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFF;
}
```

Per-bus CRC protected length and storage offsets:
- **Bus 0 (ErP)**: Protects bytes `0..27` (28 bytes). CRC stored LE at bytes `28..29`.
- **Bus 1 (Deck 2)**: Protects bytes `0..37` (38 bytes). CRC stored LE at bytes `38..39`.
- **Bus 2 (Mixer)**: Protects bytes `0..95` (96 bytes). CRC stored LE at bytes `96..97`.
- **Bus 3 (Deck 1)**: Protects bytes `0..37` (38 bytes). CRC stored LE at bytes `38..39`.

### 3.2 Native Driver IOCTL ABI (`'p'` / `0x70`)

```c
#define SUBUCOM_IOCTL_MAGIC       'p'
#define IOCTL_MOSI_TRANSFER       0x40107000u  /* _IOW('p', 0, struct mosi_cmd) */
#define IOCTL_BITS_PER_WORD_WRITE 0x40017001u  /* _IOW('p', 1, uint8_t)         */
#define IOCTL_INTERVAL_WRITE      0x40047002u  /* _IOW('p', 2, uint32_t)        */
#define IOCTL_TIMER_STATUS_READ   0x80027003u  /* _IOR('p', 3, uint16_t)        */
#define IOCTL_RX_BYTES_WRITE      0x40047004u  /* _IOW('p', 4, uint32_t)        */

#pragma pack(push, 1)
struct mosi_cmd {
    uint32_t magic;    /* 0x01000000 */
    uint32_t size;     /* byte count of data at data_ptr (<= 152) */
    uint64_t data_ptr; /* userspace virtual address of LED/display payload */
};
#pragma pack(pop)
```

---

## 4. Hardware SPI Telemetry Decoding (ErP MOSI & Inputs)

EP147 outputs 152-byte SPI MOSI frames to Bus 0 (`SUBUCOM_DEV_ERP` / `spi1.0`) at 100 Hz. These frames contain authentic 2-bit level indicators for hardware LEDs:

Each platter side occupies a stride of `0x38` bytes:
- **Side 0 (Decks 1 & 3)**: Base offset `0x00`
- **Side 1 (Decks 2 & 4)**: Base offset `0x38`

### 4.1 LED Level Decoding Table

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

### 4.2 ErP Front-Panel Input Register Map (MISO)

Input stride between Deck 1 and Deck 2 controls is `8` bytes (`ERP_DECK2_STRIDE = 8`):

| Control Function | Byte Offset | Bitmask | Notes |
|---|---|---|---|
| **Play** | `10 + (8*d)` | `0x02` | `ERP_MASK_PLAY` |
| **Cue** | `10 + (8*d)` | `0x04` | `ERP_MASK_CUE` |
| **Hot Cue Mode** | `13 + (8*d)` | `0x01` | `ERP_MASK_HOTCUE` |
| **Beat Loop Mode** | `13 + (8*d)` | `0x02` | `ERP_MASK_BEATLOOP` |
| **Slip Loop Mode** | `13 + (8*d)` | `0x04` | `ERP_MASK_SLIPLOOP` |
| **Beat Jump Mode** | `13 + (8*d)` | `0x08` | `ERP_MASK_BEATJUMP` |
| **Performance Pads 1..8** | `14 + (8*d)` | `0x01..0x80` | `ERP_MASK_PAD1..PAD8` |
| **Loop In / 4-Beat** | `8 + (8*d)` | `0x40` | `ERP_MASK_LOOP_IN` |
| **Loop Out** | `8 + (8*d)` | `0x20` | `ERP_MASK_LOOP_OUT` |
| **Reloop / Exit** | `9 + (8*d)` | `0x08` | `ERP_MASK_RELOOP` |
| **Auto Loop 4** | `12 + (8*d)` | `0x04` | `ERP_MASK_LOOP4` |
| **Auto Loop 8** | `12 + (8*d)` | `0x08` | `ERP_MASK_LOOP8` |
| **Slip Mode** | `8 + (8*d)` | `0x04` | `ERP_MASK_SLIP` |
| **Slip Reverse** | `8 + (8*d)` | `0x80` | `ERP_MASK_SLIP_REV` |
| **Vinyl / CDJ Mode** | `12 + (8*d)` | `0x02` | `ERP_MASK_JOG_MODE` |
| **Beat Sync** | `8 + (8*d)` | `0x02` | `ERP_MASK_BEAT_SYNC` |
| **Key Sync** | `9 + (8*d)` | `0x02` | `ERP_MASK_KEY_SYNC` |
| **Master Deck** | `8 + (8*d)` | `0x10` | `ERP_MASK_MASTER` |
| **Deck Select Left (1 / 3)** | `8` / `9` | `0x01` | D1: byte 8 bit 0; D3: byte 9 bit 0 |
| **Deck Select Right (2 / 4)**| `16` / `17` | `0x01` | D2: byte 16 bit 0; D4: byte 17 bit 0 |
| **Global Quantize** | `10` | `0x01` | `ERP_MASK_GLOBAL_QUANTIZE` |
| **USB Stop 1 / 2** | `24` | `0x02` / `0x04`| Safe unmount buttons |

---

## 5. Mixer SPI Protocol & Beat FX Telemetry (Bus 2 / `spi3.0`)

The XDJ-AZ mixer front-panel sub-CPU controls analog faders, trims, EQs, rotary browse knobs, and the dedicated Beat FX processing engine.

### 5.1 Authentic EP147 14-Effect Table (`0x2554df0`)

Analysis of EP147's `.rodata` string table at offset `0x2554df0` confirms the authentic 14-effect configuration:

| Index | Effect Name | Category | Modulation Mode | Default Parameter |
|---|---|---|---|---|
| **0** | `DELAY` | Time / Echo | Discrete Snapping (BEAT) | 500 ms (1 Beat) |
| **1** | `ECHO` | Time / Echo | Discrete Snapping (BEAT) | 500 ms (1 Beat) |
| **2** | `PING PONG` | Time / Echo | Discrete Snapping (BEAT) | 500 ms (1 Beat) |
| **3** | `SPIRAL` | Pitch / Reverb | Continuous Modulation | 500 ms |
| **4** | `HELIX` | Pitch / Reverb | Continuous Modulation | 500 ms |
| **5** | `REVERB` | Space / Reverb | Continuous Modulation | **50% LENGTH** (Horizontal Gauge) |
| **6** | `FLANGER` | Modulation | Continuous Modulation | 500 ms |
| **7** | `PHASER` | Modulation | Continuous Modulation | 500 ms |
| **8** | `FILTER` | Filter (LPF/HPF) | Continuous Modulation | 1000 ms (Center 64 = Neutral) |
| **9** | `TRIPLET FILTER` | Filter (LPF/HPF) | Continuous Modulation | 1000 ms (Center 64 = Neutral) |
| **10** | `TRANS` | Gating | Discrete Snapping (BEAT) | 500 ms (1 Beat) |
| **11** | `ROLL` | Looping | Discrete Snapping (BEAT) | 500 ms (1 Beat) |
| **12** | `TRIPLET ROLL` | Triplet Looping | Discrete Snapping (TRIPLET)| 500 ms (1 Beat) |
| **13** | `MOBIUS` | Frequency Sweep | Continuous Modulation | 500 ms |

> [!IMPORTANT]
> Previous firmware shims used an RX3 effect table where index 9 was `TRANS`. On XDJ-AZ, index 9 is `TRIPLET FILTER` and index 10 is `TRANS`. Calibrating to the authentic 14-effect table ensures that tapping TRANS correctly triggers TRANS rather than TRIPLET FILTER.

### 5.2 Beat FX Hardware Control Registers

| Offset | Bitmask | Register | Description |
|---|---|---|---|
| `0x16` | `0xF0` | `MIXER_OFF_FX_SEL` | Upper nibble: FX select index (0..13) |
| `0x16` | `0x01` | `MIXER_MASK_FX_ON` | Beat FX Power ON (1) / OFF (0) toggle |
| `0x16` | `0x02` | `MIXER_MASK_FX_FREQ_LOW` | Low band FX frequency isolator |
| `0x16` | `0x04` | `MIXER_MASK_FX_FREQ_MID` | Mid band FX frequency isolator |
| `0x16` | `0x08` | `MIXER_MASK_FX_FREQ_HI` | High band FX frequency isolator |
| `0x17` | `0x80` | `MIXER_MASK_FX_CH1` | Route FX to Channel 1 |
| `0x17` | `0x40` | `MIXER_MASK_FX_CH2` | Route FX to Channel 2 |
| `0x17` | `0x20` | `MIXER_MASK_FX_CH3` | Route FX to Channel 3 |
| `0x17` | `0x10` | `MIXER_MASK_FX_CH4` | Route FX to Channel 4 |
| `0x17` | `0x02` | `MIXER_MASK_FX_CH_MST` | Route FX to Master Output |
| `0x17` | `0x01` | `MIXER_MASK_FX_CH_MIC` | Route FX to Microphone Input |
| `0x14` | `0x10` | `MIXER_MASK_FX_BEAT_FWD` | Step Beat Division Forward (x2) |
| `0x14` | `0x20` | `MIXER_MASK_FX_BEAT_REV` | Step Beat Division Reverse (/2) |
| `0x14` | `0x80` | `MIXER_MASK_FX_TAP` | Tap BPM pulse |
| `0x18` | `0x04` | `MIXER_MASK_FX_QUANTIZE`| Beat FX Quantize toggle |
| `0x06` | `0x08` / `0x00` | `MIXER_OFF_LATCH` | Press latch: `0x00` pressed, `0x08` idle |

### 5.3 Frame Status & Parameter Display Offsets

In the 152-byte SPI3 frame sent to EP147:
- **Byte 84**: Quantize status bitmask (`frame[84] & 0x40`).
- **Byte 85**: 
  - High nibble (`>> 4 & 0x0F`): Echoes active FX type.
  - Low nibble (`& 0x0F`): Active X-PAD beat slot highlight (`1..6` for discrete beats, `0` for continuous mode).
- **Byte 86**: Active Beat division preset index:
  - Presets: `1` (1/16), `3` (1/8), `5` (1/4), `6` (1/3), `7` (1/2), `8` (2/3), `9` (3/4), `10` (1), `12` (2), `14` (4), `16` (8), `17` (16).
- **Byte 87**: Touch modulation flag / position indicator (`0x00` when released).
- **Bytes 90..91** (16-bit Little Endian): Display parameter value:
  - **Standard Effects**: Time in milliseconds (`31`..`8000` ms).
  - **REVERB**: Length percentage (`1`..`100`%, default `50`%).
  - **FILTER / TRIPLET FILTER**: Cutoff time (`1000` ms).

---

## 6. X-PAD Touch Strip Modulation Architecture

The XDJ-AZ touchscreen UI features an interactive X-PAD ribbon located at screen coordinates `X = 1090..1270` (180px wide) and `Y = 370..425` (55px high).

```
  1090               1180               1270
   ┌──────┬──────┬──────┬──────┬──────┬──────┐  y=370
   │ 1/16 │ 1/8  │ 1/4  │ 1/2  │ 3/4  │  1   │
   └──────┴──────┴──────┴──────┴──────┴──────┘  y=425
    Slot 1 Slot 2 Slot 3 Slot 4 Slot 5 Slot 6
```

### 6.1 Hardware SPI Registers

- `MIXER_OFF_XPAD_POS` (`0x1D` / Byte 29): 7-bit touch position (`0..127`).
- `MIXER_OFF_XPAD_ZONE` (`0x1E` / Byte 30): Zone bitmask (`1 << (slot - 1)` while touched).
- Frame Byte 87: Real-time modulation position.

### 6.2 Dual Operational Modes

#### 1. Discrete Snapping Mode
Active on effects with discrete rhythmic fractions:
- DELAY (0), ECHO (1), PING PONG (2), TRANS (10), ROLL (11), TRIPLET ROLL (12).
- **Slot Division**: 6 equal 30-pixel slots across the 180-pixel width.
- **Position Snapping**: Snaps to slot center values `{11, 32, 53, 74, 96, 117}`.
- **TIME (ms) Snapping**:

| Slot | Width Range | Center Val | Standard Fractions | Triplet Fractions (TRIPLET ROLL) |
|---|---|---|---|---|
| **1** | `1090..1119` | 11 | **1/16** (31 ms) | **1/6** (83 ms) |
| **2** | `1120..1149` | 32 | **1/8** (62 ms) | **1/4** (125 ms) |
| **3** | `1150..1179` | 53 | **1/4** (125 ms) | **1/3** (166 ms) |
| **4** | `1180..1209` | 74 | **1/2** (250 ms) | **1/2** (250 ms) |
| **5** | `1210..1239` | 96 | **3/4** (375 ms) | **2/3** (333 ms) |
| **6** | `1240..1270` | 117 | **1** (500 ms) | **1** (500 ms) |

- Highlight: Byte 85 low nibble updates to `slot` (1..6), highlighting the touched beat button.

#### 2. Continuous Modulation Mode
Active on analog/continuous frequency sweep effects:
- FILTER (8), TRIPLET FILTER (9), REVERB (5), FLANGER (6), PHASER (7), SPIRAL (3), HELIX (4), MOBIUS (13).
- **Position Modulation**: Smooth linear value: `val = (off_x * 127) / 179` (`0..127`).
- **Center Detent**: `val = 64` represents neutral / bypass.
  - **FILTER / TRIPLET FILTER**: `val < 64` engages Low Pass Filter (LPF); `val > 64` engages High Pass Filter (HPF).
  - **REVERB**: Parameter represents `LENGTH %` (1..100%, center 50%). Middle UI box renders a horizontal level/fill gauge instead of beat fraction buttons.
- **Slot Suppression**: Byte 85 low nibble is masked to `0` (`chan->frame[85] &= 0xF0`), preventing false beat button highlights.

### 6.3 Touch Release Pointer Suppression & Parameter Restoration

When the user lifts their finger (`touched == 0`):
1. **Pointer Line Removal**:
   The hardware idle sentinel is written to Byte 29:
   ```c
   chan->frame[MIXER_OFF_XPAD_POS]  = 0xFF; /* Hardware idle sentinel */
   chan->frame[MIXER_OFF_XPAD_ZONE] = 0x00;
   chan->frame[87]                  = 0x00;
   ```
   Writing `0xFF` tells EP147's UI renderer that the touch ribbon is inactive, completely eliminating the vertical blue pointer line from the screen.
2. **Parameter Restoration**:
   - For **BEAT effects**, TIME (ms) in bytes 90-91 and the slot highlight in byte 85 are restored to the active BEAT division preset (e.g. 500 ms for 1 Beat).
   - For **REVERB**, parameter reverts to 50% (`chan->frame[90] = 50; chan->frame[91] = 0`).
   - For **FILTER / TRIPLET FILTER**, parameter reverts to 1000 ms.

---

## 7. Deck Platter, Jog & Pitch Telemetry (Buses 1 & 3 / `spi2.0`, `spi5.0`)

Buses 1 and 3 interface directly with the left and right CDJ jog assemblies:

| Offset | Size | Field | Description |
|---|---|---|---|
| `9` | 1 Byte | `DECK_OFF_JOG_TOUCH` | `0x0F` = Forward Scratch Touch, `0x0B` = Reverse Scratch Touch, `0x00` = Released / Pitch Bend |
| `10..11` | 2 Bytes (LE) | `DECK_OFF_JOG_POS` | 16-bit incremental optical platter encoder |
| `12..13` | 2 Bytes (LE) | `DECK_OFF_JOG_PERIOD` | 16-bit rotation period (`0xFFFF` stopped, `~450` rotating at 33 1/3 RPM) |
| `16..17` | 2 Bytes (LE) | `DECK_OFF_TEMPO` | 10-bit pitch fader ADC (center: `517`, range `11..1023`) |

---

## 8. 10-Bit ADC Mixer Analog Registers (Bus 2 / `spi3.0`)

All analog knobs and faders are digitized by 10-bit ADCs and packed into SPI3 frames:
- **High 8 bits**: Stored at `hi_byte`.
- **Low 2 bits**: Four controls share a `lo_byte`, shifted by `6, 4, 2, 0` bits:

```c
void subucom_set10(uint8_t *f, size_t hi, size_t lo, uint8_t shift, uint16_t v) {
    if (v > 1023) v = 1023;
    f[hi] = (uint8_t)(v >> 2);
    f[lo] = (uint8_t)((f[lo] & ~(0x03 << shift)) | (((v & 0x03) << shift)));
}
```

### Analog Register Map

| Control | High Byte | Low Byte | Shift |
|---|---|---|---|
| **Ch 1 Fader** | `0x0A` | `0x0E` | 6 |
| **Ch 2 Fader** | `0x0B` | `0x0E` | 4 |
| **Ch 3 Fader** | `0x0C` | `0x0E` | 2 |
| **Ch 4 Fader** | `0x0D` | `0x0E` | 0 |
| **Crossfader** | `0x0F` | `0x10` | 0 |
| **Ch 1 Trim / Hi / Mid / Low** | `0x24`, `0x25`, `0x26`, `0x27` | `0x28` | 6, 4, 2, 0 |
| **Ch 2 Trim / Hi / Mid / Low** | `0x29`, `0x2A`, `0x2B`, `0x2C` | `0x2D` | 6, 4, 2, 0 |
| **Ch 3 Trim / Hi / Mid / Low** | `0x2E`, `0x2F`, `0x30`, `0x31` | `0x32` | 6, 4, 2, 0 |
| **Ch 4 Trim / Hi / Mid / Low** | `0x33`, `0x34`, `0x35`, `0x36` | `0x37` | 6, 4, 2, 0 |
| **Master Level / 3-Band EQ** | `0x38`, `0x39`, `0x3A`, `0x3B` | `0x3C` | 6, 4, 2, 0 |
| **Ch 1..4 Sound Color FX** | `0x3D`, `0x3E`, `0x3F`, `0x40` | `0x41` | 6, 4, 2, 0 |
| **Sound Color Parameter** | `0x4E` (8-bit) | — | — |
| **Beat FX Level / Depth** | `0x45` | `0x46` | 0 |
| **Booth Level** | `0x4A` | `0x4B` | 0 |
| **Headphone Mix / Level** | `0x4C`, `0x4D` | `0x50` | 6, 4 |

---

## 9. Touchscreen Shim & Screen-View Telemetry Extraction (`touch_shim.c`)

The touch shim provides transparent hardware touch interception, coordinate remapping, screen-view detection, and in-process X11 overlay compositing.

### 9.1 Physical Touchscreen Auto-Discovery

On initialization, `touch_shim.c` scans `/dev/input/event*`:
- Queries device properties and axes via `ioctl(fd, EVIOCGBIT(EV_ABS), ...)`.
- Detects multi-touch absolute devices (`ABS_MT_POSITION_X/Y` or `ABS_X/Y`).
- Identifies Goodix Capacitive TouchScreen, WCH CTP, Waveshare, and Ilitek controllers.
- Remaps physical coordinates to native 1280x720 display resolution using calibrated affine scaling.

### 9.2 Waveform Screen Detection Guard (`is_waveform_screen`)

To prevent spurious popup triggers when the DJ is browsing tracks, adjusting utility settings, or selecting music sources, `touch_shim.c` inspects the live framebuffer via `XGetImage`:

```c
static bool is_waveform_screen(Display *dpy, Window root) {
    /* Top-left title 'WAVEFORM' is 8 characters long:
     * - Letter 'A' at x=50, y=35 has bright white ink (lum >= 150).
     * - Letter 'M' at x=116..128, y=32..42 has bright white ink (lum >= 150).
     * On SOURCE, BROWSE, UTILITY, the region x=116..128 is dark background (lum < 60). */
    XImage *chk = XGetImage(dpy, root, 116, 32, 12, 10, AllPlanes, ZPixmap);
    if (!chk) return false;
    ...
}
```

The Beat FX selector popup is strictly guarded by `is_waveform_screen()`.

### 9.3 Touch Hit-Test Geometry & Debounce Engine

Coordinates on the 1280x720 canvas:

| Hit Target | Bounding Box | Debounce Window | Action |
|---|---|---|---|
| **FX Picker Header** | `x=1090..1270, y=85..118` & `y=170..214` | 350 ms open / 300 ms reopen | Opens 14-effect popup |
| **BPM & Quantize** | `x=1090..1270, y=119..169` | 250 ms | Toggles FX Quantize |
| **Beat Division Left** | `x=1090..1150, y=215..268` | 150 ms | Steps fraction down (/2) |
| **Beat Division Mid** | `x=1151..1208, y=215..268` | 150 ms | Resets fraction to 1 Beat |
| **Beat Division Right**| `x=1209..1270, y=215..268` | 150 ms | Steps fraction up (x2) |
| **X-PAD Ribbon** | `x=1090..1270, y=370..425` | 0 ms (Real-time tracking) | Hold-time modulation & snapping |

### 9.4 In-Process X11 Picker Compositor

- **Window**: `s_x11_win` (180x370px at `x=1090, y=85`, `override_redirect = True`).
- **Rows**: 14 effect rows (26px height per row).
- **Auto-Dismiss**: Automatically hides after 8,000 ms of inactivity.
- **Visual Styling**: Matches Pioneer CDJ theme (active row highlighted with blue fill `0x007ACC`, bold text).

---

## 10. Shared Memory ABI (`/dev/shm/xdj_az_shared`) & Cross-Process IPC

All processes in the system communicate through POSIX shared memory (`/dev/shm/xdj_az_shared`):

```c
#define XDJ_AZ_SHM_FILE  "/xdj_az_shared"
#define XDJ_AZ_SHM_MAGIC 0x415A5348u /* 'AZSH' */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         /* XDJ_AZ_SHM_MAGIC */
    uint32_t version;       /* 1 */
    uint32_t seq;           /* Incremented on updates */
    
    /* Audio VU meter levels (0..127) */
    uint8_t  vu_channel[4]; /* Decks 1..4 */
    uint8_t  vu_master_l, vu_master_r;
    uint8_t  vu_cue_l, vu_cue_r;
    
    /* Track Loaded Flags from EP147 */
    uint8_t  track_loaded[4];
    
    /* Hardware MOSI LED & Jog buffers (152 bytes each) */
    uint32_t mosi_size[4];  /* [0]=ErP, [1]=Deck2, [2]=Mixer, [3]=Deck1 */
    uint8_t  mosi_buf[4][152];
    uint32_t mosi_count[4];

    /* DSP Mixer & FX Controls */
    float    channel_gain[4];
    int8_t   cross_assign[4];
    float    cross_position;
    float    master_gain;
    float    headphones_gain;
    float    cue_master_mix;
    uint8_t  cue_mask;

    /* 3-Band EQ per channel */
    float    eq_high[4], eq_mid[4], eq_low[4];
    uint8_t  eq_mode[4];

    /* Sound Color FX */
    uint8_t  cfx_type[4];
    float    cfx_color[4], cfx_param[4];
    
    uint32_t dsp_seq;

    /* Sample-Accurate Playhead & Transport Telemetry */
    int64_t  playhead_pos[4]; /* Cumulative samples rendered */
    uint8_t  is_playing[4];   /* 1 if audio output active */
    uint8_t  is_sync[4];      /* 1 if Beat Sync enabled */
    uint8_t  is_master[4];    /* 1 if Master Deck */
    uint8_t  is_looping[4];   /* 1 if Loop active */
} xdj_az_shared_t;
#pragma pack(pop)
```

### IPC Communication Channels

1. **In-Process Linkage (`dlsym`)**: When `touch_shim` and `subucom_emu` run inside the same process space, calls to `subucom_set_bfx_type`, `subucom_set_xpad`, etc., resolve directly via `dlsym(RTLD_DEFAULT, ...)`, achieving sub-microsecond latency.
2. **UNIX Domain Socket (`/tmp/subucom_ctrl.sock`)**: Used by external CLI tools.
3. **UDP Loopback Socket (`127.0.0.1:9999`)**: Used by `flx6_bridge` to inject 153-byte framed packets (`[bus_id] + frame[152]`).

---

## 11. Controller Mapping Specifications (DDJ-FLX6)

| Physical Control | MIDI Channel / Note | Target Function |
|---|---|---|
| **SHIFT + DECK 3** (Left) | `CH_MIX` (Ch 6), Note 124 | Toggles **SLIP Mode** (`K_SLIP`) on active left deck (1 or 3) |
| **SHIFT + DECK 4** (Right) | `CH_MIX` (Ch 6), Note 125 | Toggles **SLIP Mode** (`K_SLIP`) on active right deck (2 or 4) |
| **SHIFT + JOG CUTTER** | Ch 0..3, Note 23 (`0x17`) | Toggles **Vinyl / CDJ Mode** (`K_VINYL`) on active deck |
| **DECK 3 / 4** (Unshifted) | Ch 0..3, Note 114 | Toggles active platter deck: Left (1 <-> 3), Right (2 <-> 4) |

---

## 12. Automated Diagnostic & Telemetry Capture Pipeline

The repository provides automated diagnostics in `tools/capture_sample.py` and `tools/snap.cmd`:

```
tools/snap.cmd [tag]
```

### Captured Data Artifacts (Stored in `out/snapshot/snapshot_YYYYMMDD_HHMMSS[_tag]/`)
1. **Full Screenshots**:
   - `screen_1280x800.png` via X11 root grabber (`xwd -root`).
   - `fb0_dump.raw` & `fb0_dump.png` via direct `/dev/fb0` ioctl framebuffer dump.
2. **Smart Region Crops**:
   - `crop_bfx_panel.png`: Beat FX display (`x=1080..1280, y=50..460`).
   - `crop_xpad_ribbon.png`: X-PAD touch strip (`x=1090..1270, y=345..425`).
   - `crop_decks_bar.png`: 4-Deck transport status bar (`x=0..1280, y=700..800`).
3. **System & Telemetry Logs**:
   - `ep147.log`: EP147 stdout/stderr application logs.
   - `systemd_journal.log`: `journalctl -u xdjaz.service` system logs.
   - `xdj_panel_state.json`: Active subucom SPI registers and telemetry dump.
   - `system_info.txt`: Kernel version, memory status, and X11 resolution.
