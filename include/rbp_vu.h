/*
 * rbp_vu.h -- DDJ-FLX6 / XDJ-AZ VU Meter and Fader/Trim State Manager
 * Target: AlphaTheta XDJ-AZ on Amlogic S905X.
 */
#ifndef XDJ_AZ_RBP_VU_H
#define XDJ_AZ_RBP_VU_H

#include <stdint.h>
#include <stdbool.h>

/* Channel fader and trim positions (0..1023, index 0..3 = Decks 1..4) */
extern int g_fader[4];
extern int g_fader_seen[4];
extern int g_trim[4];
extern int g_trim_seen[4];

/* Record fader position (ch is 0..3 or 1..4, v is 0..1023) */
void fader_state_set(int ch, int v);

/* Record trim position (ch is 0..3 or 1..4, v is 0..1023) */
void trim_state_set(int ch, int v);

/* Record crossfader position (v is 0..1023) */
void xfader_state_set(int v);

/* Re-assert unity mixer defaults until physical controls report */
void vu_mixer_defaults_tick(unsigned long t);

/* VU meter and level worker thread */
void *vu_thread(void *arg);

#endif /* XDJ_AZ_RBP_VU_H */

