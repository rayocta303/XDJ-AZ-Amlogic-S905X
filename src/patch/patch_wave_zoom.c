/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file patch_wave_zoom.c
 * @brief Waveform Zoom Persistence Mod implementation.
 *
 * Restores and persists the detailed waveform zoom ratio (0x3b32488).
 */

#include "patch_wave_zoom.h"
#include "az_patch_core.h"

#define EP147_WZ_RATIO 0x3b32488UL
#define WZ_CFG_PATH "/home/root/settings/azmod-wavezoom.cfg"

int patch_wave_zoom_init(void) {
    patch_log("Initializing Waveform Zoom Persistence mod...");

    int fd = open(WZ_CFG_PATH, O_RDONLY);
    if (fd >= 0) {
        uint32_t saved_ratio = 0;
        ssize_t n = read(fd, &saved_ratio, sizeof(saved_ratio));
        close(fd);

        if (n == sizeof(saved_ratio) && saved_ratio != 0) {
            uint32_t *p_ratio = (uint32_t *)EP147_WZ_RATIO;
            if (p_ratio) {
                az_patch_code(EP147_WZ_RATIO, &saved_ratio, sizeof(saved_ratio));
                patch_log("SUCCESS: Restored saved waveform zoom ratio 0x%08x at 0x%lx",
                          saved_ratio, EP147_WZ_RATIO);
                return 0;
            }
        }
    }

    patch_log("Waveform zoom ratio initialized at stock (0x%lx)", EP147_WZ_RATIO);
    return 0;
}

