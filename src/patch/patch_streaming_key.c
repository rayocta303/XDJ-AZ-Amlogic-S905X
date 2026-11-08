/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file patch_streaming_key.c
 * @brief Camelot Key Notation Mod implementation.
 *
 * Patches EP147's key text formatting routine at 0xd347a4 to force
 * the camelot boolean to 1 (mov w22, #1).
 */

#include "patch_streaming_key.h"
#include "az_patch_core.h"

#define EP147_KEY_FLAG_SITE 0xd347a4UL

int patch_streaming_key_init(void) {
    patch_log("Applying Camelot Key Notation mod...");

    uint32_t want = 0x52800036; /* mov w22, #1 */
    int rc = az_patch_code(EP147_KEY_FLAG_SITE, &want, sizeof(want));
    if (rc > 0) {
        patch_log("SUCCESS: Camelot Key Notation enabled at 0x%lx (8A, 11B...)",
                  EP147_KEY_FLAG_SITE);
        return 0;
    }
    patch_log("WARNING: Failed to patch key text flag at 0x%lx", EP147_KEY_FLAG_SITE);
    return -1;
}

