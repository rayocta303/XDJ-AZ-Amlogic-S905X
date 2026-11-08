/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file az_patch_mod.c
 * @brief Master runtime patch and mod dynamic library for AlphaTheta XDJ-AZ EP147.
 *
 * Automatically loaded via LD_PRELOAD to:
 * 1. Catch signals & record crash dumps (patch_crash_reporter_init)
 * 2. Eliminate "Update Available" header banner & utility button (patch_update_hide_init)
 * 3. Correct Beat FX layout, overlapping, and paint fallback (patch_beat_fx_init)
 * 4. Enable Extended Lead-In / negative playhead jog reading (patch_spin_back_init)
 * 5. Enable 60 FPS smooth waveform & UI repainting (patch_waveform_60fps_init)
 * 6. Swap Beat Jump page 1 to 8/16/32/64 beats (patch_beat_jump_init)
 * 7. Format all musical keys to Camelot notation (patch_streaming_key_init)
 * 8. Persist detailed waveform zoom levels (patch_wave_zoom_init)
 * 9. Guard against deviceless QR login null-pointer crashes (patch_qrlogin_guard_init)
 */

#include "az_patch_core.h"
#include "patch_beat_fx.h"
#include "patch_mods.h"
#include "patch_update_hide.h"
#include "patch_spin_back.h"
#include "patch_beat_jump.h"
#include "patch_streaming_key.h"
#include "patch_wave_zoom.h"

__attribute__((constructor))
void az_patch_mod_init(void) {
    patch_log("==================================================================");
    patch_log("AlphaTheta XDJ-AZ EP147 Master Runtime Patch & Mod Library loading");
    patch_log("Target Firmware: v1.30 (MD5: %s, Size: %ld)", EP147_V130_MD5, (long)EP147_V130_SIZE);

    /* 1. Install Signal Crash Reporter first */
    patch_crash_reporter_init();

    /* 2. Hide "Update Available" text and utility update prompt */
    patch_update_hide_init();

    /* 3. Apply Beat FX Visual Layout & Rendering Patches */
    patch_beat_fx_init();

    /* 4. Apply Extended Lead-In (Negative Playhead Jog Rotation) Mod */
    patch_spin_back_init();

    /* 5. Apply 60 FPS Waveform Repaint Timer Mod */
    patch_waveform_60fps_init();

    /* 6. Apply Beat Jump 2 First Mod */
    patch_beat_jump_init();

    /* 7. Apply Camelot Key Notation Mod */
    patch_streaming_key_init();

    /* 8. Apply Waveform Zoom Persistence Mod */
    patch_wave_zoom_init();

    /* 9. Apply Deviceless QR Login Crash Guard */
    patch_qrlogin_guard_init();

    patch_log("All patches and mods initialized successfully.");
    patch_log("==================================================================");
}
