# AlphaTheta XDJ-AZ EP147 Master Runtime Patch & Mod Library (`libaz_patch_mod.so`)

Modular runtime hotpatching and performance modification library for AlphaTheta XDJ-AZ firmware (`EP147`) running on Armbian Linux (Amlogic S905X AArch64).

---

## 1. Overview & Architecture

`libaz_patch_mod.so` is loaded ahead of the main `EP147` executable via `LD_PRELOAD`. Using GCC/Clang `__attribute__((constructor))` initialization, the library unlocks runtime process memory, hooks virtual method tables (VTable), and applies inline AArch64 instruction trampolines prior to the execution of the main UI event loop.

### Comprehensive Capabilities
1. **Suppression of "Update Available" & Firmware Update UI**:
   - Patches `isNewFirmwareAvailable()` (`0x8f4c20`) to permanently return `false`.
   - Hooks `UpdateCheckerWidgetController::setWidgetVisible` (`0x1caa210`) to force `visible = false`.
   - Completely removes the orange "Update Available" banner, info icon in the status bar, and "New Firmware Available" utility button.
2. **Extended Lead-In (Negative Playhead Jog Rotation)**:
   - Expands `PlayerCore` deck pre-roll limit (`0x14a3e10`) from stock `-2000 ms` to `-300000 ms` (5 minutes).
   - Allows reverse jog spinning far before track start without clamping or tripping track reload animations.
   - Hooks `0x1453e80` to allow beat jumps into negative lead-in positions.
3. **Beat FX Display Fix**:
   - Intercepts `BeatFxParamWidget::layout` (`0x1dde9e8`) to eliminate widget overlap.
   - Hijacks `ImagesComponent::paint` (`0x29e9610`) to eliminate the solid white (`#FFFFFF`) box on active beat buttons and restore authentic `#777777` dark gray background with crisp white typography.
4. **60 FPS Waveform & UI Mod**:
   - Patches JUCE repaint timers (`0x24fac4c` and `0x24f6b34`) from 33 ms (~30 FPS) to 16 ms (60 FPS smooth repaint).
5. **Beat Jump 2 First Mod**:
   - Intercepts async tasks `0x2086748` & `0x2085768` to swap pad jump sizes (1/2..4 <-> 8..64 beats) on page 1.
6. **Camelot Key Notation Mod**:
   - Patches `0xd347a4` to force the Camelot boolean flag to 1, rendering all keys in standard Camelot notation (`8A`, `11B`).
7. **Waveform Zoom Persistence**:
   - Restores and persists the detailed waveform zoom ratio (`0x3b32488`).
8. **Deviceless QR Login Crash Guard**:
   - Safeguards against null-pointer dereferencing in cloud QR authentication ticks (`0x1a79ea0`).
9. **Hardware-Level Signal Crash Reporter**:
   - Traps fatal signals (`SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE`, `SIGABRT`) and writes register context (`PC`, `LR`, `SP`, fault address) to `/tmp/azmod-crash.log`.

---

## 2. File Structure

```
src/patch/
├── README.md               # Master documentation
├── Makefile                # Standalone build configuration
├── az_linux_compat.h       # Bare POSIX/Linux AArch64 types & syscall definitions
├── az_patch_core.h         # Memory unlocker, vtable patcher & inline trampoline engine
├── patch_update_hide.h     # Update available suppression declarations
├── patch_update_hide.c     # Update available suppression implementation
├── patch_spin_back.h       # Extended Lead-In declarations
├── patch_spin_back.c       # Extended Lead-In implementation
├── patch_beat_fx.h         # Beat FX layout & paint hook declarations
├── patch_beat_fx.c         # Beat FX layout & paint hook implementation
├── patch_beat_jump.h       # Beat Jump 2 First declarations
├── patch_beat_jump.c       # Beat Jump 2 First implementation
├── patch_streaming_key.h   # Camelot Key Notation declarations
├── patch_streaming_key.c   # Camelot Key Notation implementation
├── patch_wave_zoom.h       # Waveform Zoom Persistence declarations
├── patch_wave_zoom.c       # Waveform Zoom Persistence implementation
├── patch_mods.h            # 60 FPS, QR login guard & crash reporter declarations
├── patch_mods.c            # 60 FPS, QR login guard & crash reporter implementation
├── az_patch_mod.c          # Shared library constructor entrypoint
└── libaz_patch_mod.so      # Compiled AArch64 shared object
```

---

## 3. Compilation

### A. Cross-Compilation on Windows (LLVM Clang)
```powershell
& "C:\Program Files\LLVM\bin\clang.exe" `
    --target=aarch64-linux-gnu `
    -O2 -Wall -Wextra -fPIC `
    -nostdlib -fuse-ld=lld -shared `
    -o "src/patch/libaz_patch_mod.so" `
    "src/patch/az_patch_mod.c" `
    "src/patch/patch_beat_fx.c" `
    "src/patch/patch_mods.c" `
    "src/patch/patch_update_hide.c" `
    "src/patch/patch_spin_back.c" `
    "src/patch/patch_beat_jump.c" `
    "src/patch/patch_streaming_key.c" `
    "src/patch/patch_wave_zoom.c"
```

### B. Native Compilation on Armbian (GCC)
```bash
cd /opt/xdjaz/src/patch
make clean && make
```
Or via top-level Makefile:
```bash
cd /opt/xdjaz
make out/libaz_patch_mod.so
```

---

## 4. Deployment & Verification on Target (`root@192.168.137.22`)

Check verification logs:
```bash
grep -i "az_patch" /opt/xdjaz/log/ep147.log
```
Expected output:
```
[AZ_PATCH] AlphaTheta XDJ-AZ EP147 Master Runtime Patch & Mod Library loading
[AZ_PATCH] Target Firmware: v1.30 (MD5: 3518275e8b4d5d63636d8d7b4468d7ba, Size: 57874328)
[AZ_PATCH] SUCCESS: Signal crash reporter installed (SIGSEGV/BUS/ILL/FPE/ABRT)
[AZ_PATCH] Disabling 'Update Available' and firmware update notifications...
[AZ_PATCH] SUCCESS: Patched isNewFirmwareAvailable() at 0x8f4c20 -> returns false
[AZ_PATCH] SUCCESS: Hooked UpdateCheckerWidgetController::setWidgetVisible at 0x1caa210
[AZ_PATCH] SUCCESS: 'Update Available' banner, text and icon completely suppressed.
[AZ_PATCH] Initializing Beat FX display patches...
[AZ_PATCH] SUCCESS: Hooked BeatFxParamWidget::layout (0x1dde9e8)
[AZ_PATCH] SUCCESS: Patched ImagesComponent paint slot (0x29e9610)
[AZ_PATCH] SUCCESS: Patched EffectValueBarWidgetDelay paint slot (0x2d45fc0)
[AZ_PATCH] Installing Extended Lead-In (Negative Playhead Jog) mod...
[AZ_PATCH] SUCCESS: Extended Lead-In enabled (pre-roll=-300,000ms at 0x14a3e10 & 0x1453e80)
[AZ_PATCH] Applying 60 FPS waveform repaint mod...
[AZ_PATCH] SUCCESS: 60 FPS waveform timer enabled (period=16ms at 0x24fac4c & 0x24f6b34)
[AZ_PATCH] Installing Beat Jump 2 First mod...
[AZ_PATCH] SUCCESS: Beat Jump 2 First enabled (swaps 1/2..4 <-> 8..64)
[AZ_PATCH] Applying Camelot Key Notation mod...
[AZ_PATCH] SUCCESS: Camelot Key Notation enabled at 0xd347a4 (8A, 11B...)
[AZ_PATCH] Initializing Waveform Zoom Persistence mod...
[AZ_PATCH] Waveform zoom ratio initialized at stock (0x3b32488)
[AZ_PATCH] Installing Deviceless QR login crash guard...
[AZ_PATCH] SUCCESS: QR login crash guard installed at 0x1a79ea0
[AZ_PATCH] All patches and mods initialized successfully.
```
