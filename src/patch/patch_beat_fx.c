/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file patch_beat_fx.c
 * @brief Implementation of Beat FX layout and painting patches for XDJ-AZ EP147.
 *
 * Resolves:
 *   1. Layout collision: enforces proper Y coordinates:
 *      - NameStrip ([self + 0x158]): y=0, h=38 (Screen y=175)
 *      - EffectValueBarWidgetDelay ([self + 0x160]): y=43, h=45 (Screen y=218)
 *      - BeatFxTimeWidget ([self + 0x168]): y=88, h=57 (Screen y=263)
 *      - Active Beat Button ([self + 0x150]): x=61, y=44, w=58, h=43
 *   2. Solid white square fallback (#FFFFFF):
 *      Intercepts ImagesComponent::paint ONLY for the active beat button,
 *      painting authentic Pioneer dark gray (#777777) fill with crisp text.
 *      Strict pointer identity check ensures Deck BPM, Quantize, and Hotcue badges
 *      are NEVER touched.
 */

#include "patch_beat_fx.h"
#include "az_patch_core.h"

typedef void (*juce_set_bounds_fn)(uintptr_t comp, int x, int y, int w, int h);
typedef void (*juce_set_visible_fn)(uintptr_t comp, int visible);
typedef void (*graphics_set_colour_fn)(void *g, const uint32_t *argb);
typedef void (*graphics_fill_rect_f_fn)(void *g, float x, float y, float w, float h);

#define FXB_SETCOLOUR  ((graphics_set_colour_fn)0x23af140UL)
#define FXB_FILLRECTF  ((graphics_fill_rect_f_fn)0x23af6c0UL)

static void (*g_orig_beatfx_layout)(uintptr_t self, unsigned int type, unsigned int sub) = NULL;
static void (*g_orig_images_comp_paint)(uintptr_t self, void *g) = NULL;

static uintptr_t g_fx_param_widget = 0;
static uintptr_t g_active_beat_button = 0;

/**
 * @brief Intercept BeatFxParamWidget::layout to enforce proper widget bounds & visibility.
 */
static void hook_beatfx_param_layout(uintptr_t self, unsigned int type, unsigned int sub) {
    if (g_orig_beatfx_layout) {
        g_orig_beatfx_layout(self, type, sub);
    }
    if (!self || (self & 7)) return;

    g_fx_param_widget = self;

    juce_set_bounds_fn set_bounds = (juce_set_bounds_fn)EP147_JUCE_SETBOUNDS;

    /* Active Beat Button: [self + 0x150] (ImagesComponent, 58x43) -> (61, 44, 58, 43) */
    uintptr_t active_btn = *(uintptr_t *)(self + 0x150);
    if (active_btn && !(active_btn & 7)) {
        g_active_beat_button = active_btn;
        set_bounds(active_btn, 61, 44, 58, 43);
    }

    /* 1. Name strip: [self + 0x158] (ImagesComponent, 176x26) -> (2, 0, 176, 38) */
    uintptr_t name_strip = *(uintptr_t *)(self + 0x158);
    if (name_strip && !(name_strip & 7)) {
        set_bounds(name_strip, 2, 0, 176, 38);
    }

    /* 2. Delay/Beat Value Bar: [self + 0x160] (EffectValueBarWidgetDelay, 708x45) */
    uintptr_t val_bar = *(uintptr_t *)(self + 0x160);
    if (val_bar && !(val_bar & 7)) {
        /* Enforce correct relative Y offset: y=43 (screen Y: 175 + 43 = 218) */
        set_bounds(val_bar, 2, 43, 708, 45);

        /* Enforce visibility for standard beat-division FX (delay, echo, pingpong, etc.) */
        if (type != 5) { /* 5 is REVERB */
            uintptr_t vt = *(uintptr_t *)val_bar;
            if (vt && !(vt & 7)) {
                juce_set_visible_fn set_vis = *(juce_set_visible_fn *)(vt + 0x60);
                if (set_vis) set_vis(val_bar, 1);
            }
        }
    }

    /* 3. Time Readout: [self + 0x168] (BeatFxTimeWidget, 176x57) */
    uintptr_t time_widget = *(uintptr_t *)(self + 0x168);
    if (time_widget && !(time_widget & 7)) {
        /* Enforce correct relative Y offset: y=88 (screen Y: 175 + 88 = 263) */
        set_bounds(time_widget, 2, 88, 176, 57);

        /* Enforce visibility so TIME / ms parameter is always displayed */
        if (type != 5) {
            uintptr_t vt = *(uintptr_t *)time_widget;
            if (vt && !(vt & 7)) {
                juce_set_visible_fn set_vis = *(juce_set_visible_fn *)(vt + 0x60);
                if (set_vis) set_vis(time_widget, 1);
            }
        }
    }

    /* 4. Reverb Widget: [self + 0x170] (EffectReverbWidget, 180x102) */
    uintptr_t reverb_widget = *(uintptr_t *)(self + 0x170);
    if (reverb_widget && !(reverb_widget & 7)) {
        set_bounds(reverb_widget, 2, 43, 180, 102);
        uintptr_t vt = *(uintptr_t *)reverb_widget;
        if (vt && !(vt & 7)) {
            juce_set_visible_fn set_vis = *(juce_set_visible_fn *)(vt + 0x60);
            if (set_vis) set_vis(reverb_widget, (type == 5) ? 1 : 0);
        }
    }
}

/**
 * @brief Intercept ImagesComponent::paint ONLY for the active beat button,
 *        painting authentic Pioneer dark gray (#777777).
 *        All other ImagesComponents (Deck BPM, Quantize, Hotcues) run stock.
 */
static void hook_images_comp_paint(uintptr_t self, void *g) {
    if (!self || (self & 7) || !g_orig_images_comp_paint || !g) return;

    /* STRICT GUARD: only intercept if self is the active beat button in Beat FX panel */
    bool is_beat_btn = (self == g_active_beat_button);
    if (!is_beat_btn && g_fx_param_widget) {
        uintptr_t cur_btn = *(uintptr_t *)(g_fx_param_widget + 0x150);
        if (self == cur_btn) {
            is_beat_btn = true;
            g_active_beat_button = cur_btn;
        }
    }

    if (is_beat_btn) {
        /* 1. Paint authentic Pioneer button background fill: #777777 (RGB: 119, 119, 119) */
        uint32_t argb = 0xFF777777u;
        FXB_SETCOLOUR(g, &argb);
        FXB_FILLRECTF(g, 0.0f, 0.0f, 58.0f, 43.0f);

        /* 2. Temporarily switch image index to 0 so the bright white text symbol is drawn cleanly */
        uint32_t orig_idx = *(uint32_t *)(self + 0xc8);
        *(uint32_t *)(self + 0xc8) = 0;
        g_orig_images_comp_paint(self, g);
        *(uint32_t *)(self + 0xc8) = orig_idx;
        return;
    }

    /* All other ImagesComponents: 100% stock rendering */
    g_orig_images_comp_paint(self, g);
}

/**
 * @brief Install all Beat FX visual layout and rendering patches.
 */
int patch_beat_fx_init(void) {
    patch_log("Initializing Beat FX display patches...");

    /* 1. Hook BeatFxParamWidget::layout via inline hook trampoline */
    int r1 = az_install_hook(EP147_BFX_LAYOUT, (void *)hook_beatfx_param_layout, (void **)&g_orig_beatfx_layout);
    if (r1 <= 0) {
        patch_log("ERROR: Failed to hook BeatFxParamWidget::layout (0x%lx)", EP147_BFX_LAYOUT);
    } else {
        patch_log("SUCCESS: Hooked BeatFxParamWidget::layout (0x%lx)", EP147_BFX_LAYOUT);
    }

    /* 2. Hook ImagesComponent::paint via vtable slot 0x29e9610 */
    int r2 = az_patch_vtable(EP147_IMAGESCOMP_PAINT_SLOT, (void *)hook_images_comp_paint, (void **)&g_orig_images_comp_paint);
    if (r2 <= 0) {
        patch_log("ERROR: Failed to patch ImagesComponent vtable paint slot (0x%lx)", EP147_IMAGESCOMP_PAINT_SLOT);
    } else {
        patch_log("SUCCESS: Patched ImagesComponent paint slot (0x%lx)", EP147_IMAGESCOMP_PAINT_SLOT);
    }

    return (r1 > 0 && r2 > 0) ? 0 : -1;
}
