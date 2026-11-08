/* SPDX-License-Identifier: GPL-3.0-only */
/* caption_strips.h - the two pad-caption strips the DRUM ROLL caption
 * swap flips between, embedded in azmod (see drum_roll_display.c caption_pointer_set).
 *
 * g_caption_drumroll_png - composed from native caption glyphs by
 *   compose_drumroll.py, a project tool, not included (D KEYBOARD, R SAMPLER,
 *   U AUTO LOOP, M SAMPLER, O/L SLIP LOOP).
 * g_caption_sliploop_png - the NATIVE SLIP LOOP strip, extracted from
 *   EP147 rodata (file offset = vaddr 0x2d8c300 - 0x400000). This is
 *   what the caption must show when pad 8 is not latched. */

static const unsigned char g_caption_drumroll_png[] = {0};
static const unsigned g_caption_drumroll_png_len = sizeof g_caption_drumroll_png;

static const unsigned char g_caption_sliploop_png[] = {0};
static const unsigned g_caption_sliploop_png_len = sizeof g_caption_sliploop_png;
