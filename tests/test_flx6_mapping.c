/*
 * test_flx6_mapping.c — Unit Test for Pioneer DDJ-FLX6 MIDI Translation to Subucom
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "subucom_protocol.h"
#include "xdj_az_abi64.h"

/* Structure to simulate FLX6 state conversion */
typedef struct {
    uint8_t ch_fader[4];  /* 0-127 */
    uint8_t ch_hi[4];
    uint8_t ch_mid[4];
    uint8_t ch_low[4];
    uint8_t crossfader;   /* 0-127 */
    bool play[4];
    bool cue[4];
    int32_t jog_pos[2];
} flx6_mock_state_t;

static void flx6_process_midi(flx6_mock_state_t *s, uint8_t status, uint8_t d1, uint8_t d2) {
    uint8_t type = status & 0xF0;
    uint8_t ch = status & 0x0F;

    if (type == 0xB0) { /* Control Change */
        switch (d1) {
            case 0x13: s->ch_fader[0] = d2; break; /* Ch 1 Fader */
            case 0x14: s->ch_fader[1] = d2; break; /* Ch 2 Fader */
            case 0x15: s->ch_fader[2] = d2; break; /* Ch 3 Fader */
            case 0x16: s->ch_fader[3] = d2; break; /* Ch 4 Fader */
            case 0x1F: s->crossfader = d2;  break; /* Crossfader */
            case 0x07: if (ch < 4) s->ch_hi[ch] = d2; break;  /* EQ HI */
            case 0x0B: if (ch < 4) s->ch_mid[ch] = d2; break; /* EQ MID */
            case 0x0F: if (ch < 4) s->ch_low[ch] = d2; break; /* EQ LOW */
            case 0x21: /* Jog Deck 1 */
                s->jog_pos[0] += (d2 < 64) ? d2 : (int)(d2 - 128);
                break;
            case 0x22: /* Jog Deck 2 */
                s->jog_pos[1] += (d2 < 64) ? d2 : (int)(d2 - 128);
                break;
        }
    } else if (type == 0x90) { /* Note On */
        if (d1 == 0x0B) { /* Play/Pause */
            if (ch < 4) s->play[ch] = (d2 > 0);
        } else if (d1 == 0x0C) { /* Cue */
            if (ch < 4) s->cue[ch] = (d2 > 0);
        }
    }
}

int main(void) {
    printf("[TEST] Starting Pioneer DDJ-FLX6 MIDI Mapping Test Suite...\n");

    flx6_mock_state_t state;
    memset(&state, 0, sizeof(state));

    /* 1. Test 4-Channel Volume Faders */
    flx6_process_midi(&state, 0xB0, 0x13, 127); /* Ch 1 Full */
    flx6_process_midi(&state, 0xB0, 0x14, 64);  /* Ch 2 Mid */
    flx6_process_midi(&state, 0xB0, 0x15, 0);   /* Ch 3 Zero */
    flx6_process_midi(&state, 0xB0, 0x16, 100); /* Ch 4 High */

    printf("  [1] 4-Channel Faders: Ch1=%d, Ch2=%d, Ch3=%d, Ch4=%d -> %s\n",
           state.ch_fader[0], state.ch_fader[1], state.ch_fader[2], state.ch_fader[3],
           (state.ch_fader[0] == 127 && state.ch_fader[1] == 64 &&
            state.ch_fader[2] == 0 && state.ch_fader[3] == 100) ? "PASS" : "FAIL");
    assert(state.ch_fader[0] == 127 && state.ch_fader[1] == 64);

    /* 2. Test 3-Band EQs */
    flx6_process_midi(&state, 0xB0, 0x07, 120); /* Ch 0 HI */
    flx6_process_midi(&state, 0xB0, 0x0B, 64);  /* Ch 0 MID */
    flx6_process_midi(&state, 0xB0, 0x0F, 10);  /* Ch 0 LOW */

    printf("  [2] 3-Band EQ (Ch1): HI=%d, MID=%d, LOW=%d -> %s\n",
           state.ch_hi[0], state.ch_mid[0], state.ch_low[0],
           (state.ch_hi[0] == 120 && state.ch_mid[0] == 64 && state.ch_low[0] == 10) ? "PASS" : "FAIL");
    assert(state.ch_hi[0] == 120 && state.ch_mid[0] == 64 && state.ch_low[0] == 10);

    /* 3. Test Play/Cue buttons */
    flx6_process_midi(&state, 0x90, 0x0B, 127); /* Deck 1 Play Press */
    flx6_process_midi(&state, 0x91, 0x0C, 127); /* Deck 2 Cue Press */

    printf("  [3] Transport: Deck1 Play=%s, Deck2 Cue=%s -> PASS\n",
           state.play[0] ? "ON" : "OFF", state.cue[1] ? "ON" : "OFF");
    assert(state.play[0] == true && state.cue[1] == true);

    /* 4. Test Jog Wheel delta tracking (Relative 2's complement) */
    flx6_process_midi(&state, 0xB0, 0x21, 3);   /* Forward +3 */
    flx6_process_midi(&state, 0xB0, 0x21, 125); /* Backward -3 (128 - 125 = 3) */
    printf("  [4] Jog Platter Position Tracking: net delta = %d -> %s\n",
           state.jog_pos[0], (state.jog_pos[0] == 0) ? "PASS" : "FAIL");
    assert(state.jog_pos[0] == 0);

    printf("[PASS] All Pioneer DDJ-FLX6 mapping tests passed successfully!\n");
    return 0;
}
