/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file patch_mods.h
 * @brief Performance and stability mods ported from xdj-az-mods for XDJ-AZ EP147.
 *
 * Includes:
 *   1. 60 FPS Smooth Waveform & UI Repaint Patch (timer period reduced from 33ms to 16ms).
 *   2. Deviceless / QR Login Null Dereference Crash Guard.
 *   3. Signal Crash Reporter with register state and stack unwind logging.
 */

#ifndef PATCH_MODS_H
#define PATCH_MODS_H

#ifdef __cplusplus
extern "C" {
#endif

int patch_waveform_60fps_init(void);
int patch_qrlogin_guard_init(void);
int patch_crash_reporter_init(void);
int patch_bluetooth_init(void);

#ifdef __cplusplus
}
#endif

#endif /* PATCH_MODS_H */

