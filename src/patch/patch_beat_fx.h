/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file patch_beat_fx.h
 * @brief Beat FX layout and painting patches for XDJ-AZ EP147.
 *
 * Corrects the visual distortion where EffectValueBarWidgetDelay collapsed to y=0
 * overlapping the DELAY title strip, fixes the solid white square fallback (#FFFFFF)
 * to authentic Pioneer dark gray (#777777), and ensures the BeatFxTimeWidget remains visible.
 */

#ifndef PATCH_BEAT_FX_H
#define PATCH_BEAT_FX_H

#ifdef __cplusplus
extern "C" {
#endif

int patch_beat_fx_init(void);

#ifdef __cplusplus
}
#endif

#endif /* PATCH_BEAT_FX_H */

