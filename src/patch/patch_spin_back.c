/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file patch_spin_back.c
 * @brief Extended Lead-In: enables smooth reverse jog rotation reading negative playhead positions.
 *
 * In stock EP147, PlayerCore restricts deck pre-roll to -2000 ms (-2s).
 * When rotating the jog backwards past -2s, it clamps, turns off jog LEDs,
 * and upon crossing back over 0 triggers the track load animation.
 *
 * This mod expands pre-roll from -2000 ms to -300000 ms (5 minutes lead-in),
 * allowing continuous negative position reading and scratching before the start.
 */

#include "patch_spin_back.h"
#include "az_patch_core.h"

#define EP147_SB_CLAMP      0x14a3e10UL
#define EP147_SB_JUMP_CHECK 0x1453e80UL
#define EP147_CORE_VT       0x2874158UL /* player_basis::PlayerCore */

#define SB_STOCK_MS         (-2000L)
#define SB_CAP_MS           (-300000L)  /* 5 minutes lead-in */

static void (*g_orig_sb_clamp)(long, long, long, long, long) = NULL;
static int (*g_orig_sb_check)(long, long) = NULL;

static void hook_sb_clamp(long strategy, long a1, long pos, long readable, long flag) {
    if (strategy && !(strategy & 7)) {
        uintptr_t core = *(uintptr_t *)(strategy + 0x18);
        if (core && !(core & 7)) {
            uintptr_t vt = *(uintptr_t *)core;
            if (vt == EP147_CORE_VT) {
                int64_t *preroll = (int64_t *)(core + 0x2f0);
                if (*preroll != SB_CAP_MS) {
                    *preroll = SB_CAP_MS;
                }
            }
        }
    }
    if (g_orig_sb_clamp) {
        g_orig_sb_clamp(strategy, a1, pos, readable, flag);
    }
}

static int hook_sb_check(long function, long target) {
    /* If target is negative (before track start), allow within 5 min floor */
    if (target < 0) {
        /* 5 minutes at 48kHz = -14,400,000 samples */
        if (target >= -14400000L) {
            return 1; /* Allow beat jump into negative lead-in */
        }
    }
    if (g_orig_sb_check) {
        return g_orig_sb_check(function, target);
    }
    return 0;
}

int patch_spin_back_init(void) {
    patch_log("Installing Extended Lead-In (Negative Playhead Jog) mod...");

    int r1 = az_install_hook(EP147_SB_CLAMP, (void *)hook_sb_clamp, (void **)&g_orig_sb_clamp);
    int r2 = az_install_hook(EP147_SB_JUMP_CHECK, (void *)hook_sb_check, (void **)&g_orig_sb_check);

    if (r1 > 0 && r2 > 0) {
        patch_log("SUCCESS: Extended Lead-In enabled (pre-roll=-300,000ms at 0x%lx & 0x%lx)",
                  EP147_SB_CLAMP, EP147_SB_JUMP_CHECK);
        return 0;
    }
    patch_log("WARNING: Extended Lead-In hooks partially installed (clamp=%d, check=%d)", r1, r2);
    return -1;
}

