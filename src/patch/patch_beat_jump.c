/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file patch_beat_jump.c
 * @brief Beat Jump 2 First Mod implementation.
 *
 * Intercepts pad press and release async tasks and swaps jump lengths:
 *   1/2 <-> 8 beats
 *   1   <-> 16 beats
 *   2   <-> 32 beats
 *   4   <-> 64 beats
 */

#include "patch_beat_jump.h"
#include "az_patch_core.h"

#define EP147_BJ_PADPRESS_RUN 0x2086748UL
#define EP147_BJ_PADPRESS_VT  0x2e15028UL
#define EP147_BJ_PADREL_RUN   0x2085768UL
#define EP147_BJ_PADREL_VT    0x2e15058UL

static void (*g_orig_bj_press)(long) = NULL;
static void (*g_orig_bj_rel)(long) = NULL;

static uint32_t bj_swap_len(uint32_t len) {
    switch (len) {
    case 0x00020001u: return 0x00010008u; /* 1/2 <-> 8  */
    case 0x00010008u: return 0x00020001u;
    case 0x00010001u: return 0x00010010u; /* 1   <-> 16 */
    case 0x00010010u: return 0x00010001u;
    case 0x00010002u: return 0x00010020u; /* 2   <-> 32 */
    case 0x00010020u: return 0x00010002u;
    case 0x00010004u: return 0x00010040u; /* 4   <-> 64 */
    case 0x00010040u: return 0x00010004u;
    default: return len;
    }
}

static void hook_bj_press(long task) {
    if (task && !(task & 7)) {
        uintptr_t vt = *(uintptr_t *)task;
        if (vt == EP147_BJ_PADPRESS_VT) {
            uint32_t *p_len = (uint32_t *)(task + 0x20);
            *p_len = bj_swap_len(*p_len);
        }
    }
    if (g_orig_bj_press) {
        g_orig_bj_press(task);
    }
}

static void hook_bj_rel(long task) {
    if (task && !(task & 7)) {
        uintptr_t vt = *(uintptr_t *)task;
        if (vt == EP147_BJ_PADREL_VT) {
            uint32_t *p_len = (uint32_t *)(task + 0x20);
            *p_len = bj_swap_len(*p_len);
        }
    }
    if (g_orig_bj_rel) {
        g_orig_bj_rel(task);
    }
}

int patch_beat_jump_init(void) {
    patch_log("Installing Beat Jump 2 First mod...");

    int r1 = az_install_hook(EP147_BJ_PADPRESS_RUN, (void *)hook_bj_press, (void **)&g_orig_bj_press);
    int r2 = az_install_hook(EP147_BJ_PADREL_RUN, (void *)hook_bj_rel, (void **)&g_orig_bj_rel);

    if (r1 > 0 && r2 > 0) {
        patch_log("SUCCESS: Beat Jump 2 First enabled (swaps 1/2..4 <-> 8..64)");
        return 0;
    }
    patch_log("WARNING: Beat Jump hooks partially installed (press=%d, rel=%d)", r1, r2);
    return -1;
}

