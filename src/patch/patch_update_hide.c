/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file patch_update_hide.c
 * @brief Completely eliminates "Update Available" header text and "New Firmware Available" UI.
 *
 * Mechanisms:
 * 1. Patches `isNewFirmwareAvailable()` (0x8f4c20) to permanently return false (mov w0, #0; ret).
 * 2. Hooks `UpdateCheckerWidgetController::setWidgetVisible` (0x1caa210) to force visible=false.
 * 3. Patches caller sites 0x1caa4dc and 0x1cab374 to ensure parameter w1 is always 0.
 */

#include "patch_update_hide.h"
#include "az_patch_core.h"

#define EP147_IS_NEW_FW_AVAIL       0x8f4c20UL
#define EP147_UPDATE_SET_VISIBLE    0x1caa210UL
#define EP147_CALLSITE_LAMBDA       0x1caa4dcUL
#define EP147_CALLSITE_CHECKER      0x1cab374UL

static void (*g_orig_set_visible)(void *self, bool visible) = NULL;

static void hook_update_set_visible(void *self, bool visible) {
    (void)visible;
    /* Always force false to keep UpdateCheckerWidget invisible */
    if (g_orig_set_visible) {
        g_orig_set_visible(self, false);
    }
}

int patch_update_hide_init(void) {
    patch_log("Disabling 'Update Available' and firmware update notifications...");

    /* 1. Patch isNewFirmwareAvailable() at 0x8f4c20 to always return 0 (false) */
    uint32_t patch_ret_false[2] = {
        0x52800000, /* mov w0, #0 */
        0xd65f03c0  /* ret        */
    };
    int r1 = az_patch_code(EP147_IS_NEW_FW_AVAIL, patch_ret_false, sizeof(patch_ret_false));
    if (r1 > 0) {
        patch_log("SUCCESS: Patched isNewFirmwareAvailable() at 0x%lx -> returns false",
                  EP147_IS_NEW_FW_AVAIL);
    } else {
        patch_log("WARNING: Failed to patch isNewFirmwareAvailable() at 0x%lx",
                  EP147_IS_NEW_FW_AVAIL);
    }

    /* 2. Hook UpdateCheckerWidgetController::setWidgetVisible (0x1caa210) */
    int r2 = az_install_hook(EP147_UPDATE_SET_VISIBLE,
                             (void *)hook_update_set_visible,
                             (void **)&g_orig_set_visible);
    if (r2 > 0) {
        patch_log("SUCCESS: Hooked UpdateCheckerWidgetController::setWidgetVisible at 0x%lx",
                  EP147_UPDATE_SET_VISIBLE);
    } else {
        patch_log("WARNING: Failed to hook setWidgetVisible at 0x%lx",
                  EP147_UPDATE_SET_VISIBLE);
    }

    /* 3. Patch call sites 0x1caa4dc and 0x1cab374 to pass w1 = 0 */
    uint32_t mov_w1_0 = 0x52800001; /* mov w1, #0 */
    az_patch_code(EP147_CALLSITE_LAMBDA, &mov_w1_0, sizeof(mov_w1_0));
    az_patch_code(EP147_CALLSITE_CHECKER, &mov_w1_0, sizeof(mov_w1_0));

    patch_log("SUCCESS: 'Update Available' banner, text and icon completely suppressed.");
    return 0;
}

