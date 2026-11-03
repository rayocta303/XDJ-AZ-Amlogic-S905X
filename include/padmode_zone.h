/*
 * padmode_zone.h -- DDJ-FLX6 / XDJ-AZ Pad Mode Manager across 4 Decks
 * Target: AlphaTheta XDJ-AZ on Amlogic S905X.
 */
#ifndef XDJ_AZ_PADMODE_ZONE_H
#define XDJ_AZ_PADMODE_ZONE_H

#include <stdint.h>
#include <stdbool.h>

#define PM_MODES            4

/* The 4 Pioneer DJ Pad Modes */
#define PM_MODE_HOTCUE      0   /* Hot Cue */
#define PM_MODE_BEATLOOP    1   /* Auto Beat Loop / Pad FX 1 */
#define PM_MODE_SLIPLOOP    2   /* Slip Beat Loop / Beat Jump */
#define PM_MODE_BEATJUMP    3   /* Sampler / Beat Jump */

/* Mode button notes on Deck channels 0..3 */
#define N_MODE_HOT          27  /* HOT CUE button */
#define N_MODE_PFX1         30  /* PAD FX 1 / BEAT LOOP button */
#define N_MODE_JUMP         32  /* BEAT JUMP / SLIP LOOP button */
#define N_MODE_SMPL         34  /* SAMPLER / BEAT JUMP button */

/* Pad note bases for each mode */
#define PM_BASE_HOTCUE      0   /* Notes 0..7 */
#define PM_BASE_BEATLOOP    16  /* Notes 16..23 */
#define PM_BASE_SLIPLOOP    32  /* Notes 32..39 */
#define PM_BASE_BEATJUMP    48  /* Notes 48..55 */

/* State queries and setters across Decks 0..3 */
int  padmode_get(int deck);
void padmode_set(int deck, int mode);
int  padmode_next(int deck);

/* Lookup helpers */
int  padmode_note_base(int mode);
int  padmode_button_note(int mode);
int  padmode_from_button_note(int note);
int  padmode_next_keycode(int pad_mode);

#endif /* XDJ_AZ_PADMODE_ZONE_H */

