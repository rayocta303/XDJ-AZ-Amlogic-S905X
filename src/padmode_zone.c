/*
 * padmode_zone.c -- DDJ-FLX6 / XDJ-AZ Pad Mode Manager Implementation
 * Target: AlphaTheta XDJ-AZ on Amlogic S905X.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "padmode_zone.h"
#include "flx6_bridge.h"

/* The 4 Pioneer Pad Mode keycodes in order */
static const int mode_keys[PM_MODES] = {
    K_HOTCUE,
    K_ALOOP,
    K_SLIPLOOP,
    K_BEATJUMP
};

/* Button notes corresponding to each mode */
static const int mode_notes[PM_MODES] = {
    N_MODE_HOT,
    N_MODE_PFX1,
    N_MODE_JUMP,
    N_MODE_SMPL
};

/* Note base for performance pads 1..8 in each mode */
static const int mode_bases[PM_MODES] = {
    PM_BASE_HOTCUE,
    PM_BASE_BEATLOOP,
    PM_BASE_SLIPLOOP,
    PM_BASE_BEATJUMP
};

/* Active mode per deck (Decks 0..3 default to HOT CUE) */
static int s_deck_padmode[4] = {
    PM_MODE_HOTCUE,
    PM_MODE_HOTCUE,
    PM_MODE_HOTCUE,
    PM_MODE_HOTCUE
};

int padmode_get(int deck)
{
    if (deck < 0 || deck >= 4) return PM_MODE_HOTCUE;
    return s_deck_padmode[deck];
}

void padmode_set(int deck, int mode)
{
    if (deck < 0 || deck >= 4) return;
    if (mode < 0 || mode >= PM_MODES) return;
    if (s_deck_padmode[deck] != mode) {
        s_deck_padmode[deck] = mode;
        klog("[padmode] Deck %d pad mode -> %d (note base %d, keycode 0x%04x)\n",
             deck + 1, mode, mode_bases[mode], mode_keys[mode]);
        send_rx_key(mode_keys[mode], OP_PRESS, deck + 1, 0);
        send_rx_key(mode_keys[mode], OP_RELEASE, deck + 1, 0);
    }
}

int padmode_next(int deck)
{
    if (deck < 0 || deck >= 4) return PM_MODE_HOTCUE;
    int next_mode = (s_deck_padmode[deck] + 1) % PM_MODES;
    padmode_set(deck, next_mode);
    return next_mode;
}

int padmode_note_base(int mode)
{
    if (mode < 0 || mode >= PM_MODES) return PM_BASE_HOTCUE;
    return mode_bases[mode];
}

int padmode_button_note(int mode)
{
    if (mode < 0 || mode >= PM_MODES) return N_MODE_HOT;
    return mode_notes[mode];
}

int padmode_from_button_note(int note)
{
    for (int i = 0; i < PM_MODES; i++) {
        if (mode_notes[i] == note) return i;
    }
    return -1;
}

int padmode_next_keycode(int pad_mode)
{
    if (pad_mode < 0 || pad_mode >= PM_MODES) return 0;
    return mode_keys[(pad_mode + 1) % PM_MODES];
}

