/*
 * rbp_led.c -- DDJ-FLX6 / XDJ-AZ Hardware LED and Illumination Implementation
 * Target: AlphaTheta XDJ-AZ on Amlogic S905X.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "rbp_led.h"
#include "padmode_zone.h"
#include "flx6_bridge.h"
#include "xdj_az_shm.h"

int led_loop_armed[4] = {0, 0, 0, 0};

static signed char led_deck_last[4][L_SLOT_COUNT];
static signed char led_pfl_last[4] = { -1, -1, -1, -1 };
static signed char led_mc_last = -1;
static int led_pad_last[4][8];
static int led_pad_base[4] = { -1, -1, -1, -1 };
static unsigned char s_ring_pos[4] = { 0, 0, 0, 0 };

static const uint8_t s_pad_channels[4] = { 7, 9, 11, 13 };

/* Dedicated helper to send note only when value changed or force refresh */
static inline void led_send(int midi_ch, int note, signed char *last, int on, int force)
{
    if (note < 0) return;
    int vel = on ? 0x7F : 0x00;
    if (vel != *last || force) {
        *last = (signed char)vel;
        midi_note(midi_ch, note, vel);
    }
}

static inline void led_send_deck(int d, int note, signed char *last, int on, int force)
{
    led_send(d, note, last, on, force);
    if (d == flx6_get_active_deck(0) && d != 0) midi_note(0, note, on ? 0x7F : 0x00);
    if (d == flx6_get_active_deck(1) && d != 1) midi_note(1, note, on ? 0x7F : 0x00);
}

/* One-shot track load animation trigger */
void led_trigger_load_animation(int deck)
{
    if (deck < 0 || deck >= 4) return;
    /* Pioneer DDJ-FLX6 Track Load Animation: Channel 15 (0-based), Notes 0..3 */
    midi_note(15, deck, 0x7F);
    klog("[rbp_led] Deck %d Track Load Animation triggered (Ch 15 Note %d 0x7F)\n",
         deck + 1, deck);
}

void *led_thread(void *arg)
{
    (void)arg;
    unsigned long tick = 0;
    int shm_fd = -1;
    xdj_az_shared_t *shm = NULL;
    uint8_t prev_loaded[4] = { 0, 0, 0, 0 };

    memset(led_deck_last, -1, sizeof(led_deck_last));
    memset(led_pad_last, -1, sizeof(led_pad_last));

    while (1) {
        usleep(50000); /* 20 Hz / 50 ms tick */
        tick++;

        /* 100 ms refresh cadence for tactile LEDs (FLX4/FLX6 panel stream requirement) */
        int force = ((tick % 2) == 0);
        bool blink = ((tick / 5) % 2) == 0; /* 250ms half-cycle = 500ms full blink */

        /* Connect to /dev/shm/xdj_az_shared */
        if (!shm) {
            shm_fd = shm_open(XDJ_AZ_SHM_FILE, O_RDONLY, 0666);
            if (shm_fd < 0) shm_fd = open("/dev/shm/xdj_az_shared", O_RDONLY, 0666);
            if (shm_fd >= 0) {
                void *p = mmap(NULL, sizeof(xdj_az_shared_t), PROT_READ, MAP_SHARED, shm_fd, 0);
                if (p != MAP_FAILED) shm = (xdj_az_shared_t*)p;
            }
        }

        /* Check for Track Load transitions (ONE-SHOT TRIGGER ONLY!) */
        if (shm && shm->magic == XDJ_AZ_SHM_MAGIC) {
            for (int d = 0; d < 4; d++) {
                uint8_t cur_ld = shm->track_loaded[d];
                if (cur_ld && !prev_loaded[d]) {
                    led_trigger_load_animation(d);
                }
                prev_loaded[d] = cur_ld;
            }
        }

        /* 1. Deck Transport, Loop, Call & Pad Mode LEDs */
        for (int d = 0; d < 4; d++) {
            int sch = d; /* MIDI Channel 0..3 */
            bool playing = rbp_is_playing(d);
            bool loaded  = rbp_is_loaded(d);
            int armed    = led_loop_armed[d];

            int side = (d == 0 || d == 2) ? 0 : 1;
            int active_on_side = (side == 0) ? active_left_deck : active_right_deck;
            bool is_active_deck = (d == active_on_side);

            int play_on = 0;
            int cue_on = 0;
            int sync_on = 0;
            int master_on = 0;
            int mt_on = 1;
            int in_on = 0;
            int out_on = 0;
            int reloop_on = 0;

            /* Check if live ErP MOSI frame from EP147 is available in SHM */
            if (shm && shm->magic == XDJ_AZ_SHM_MAGIC && shm->mosi_count[0] > 0 && is_active_deck) {
                size_t stride = side * 0x38;
                const uint8_t *erp = &shm->mosi_buf[0][stride];

                /* 2-bit level decoder: 2 = solid ON, 1 = blink/dim, 0 = OFF */
                uint8_t play_lvl = (erp[0x0B] >> 0) & 3;
                play_on = (play_lvl == 2) ? 1 : ((play_lvl == 1) ? (blink ? 1 : 0) : 0);

                uint8_t cue_lvl = (erp[0x0B] >> 2) & 3;
                cue_on = (cue_lvl == 2) ? 1 : ((cue_lvl == 1) ? (blink ? 1 : 0) : 0);

                uint8_t sync_lvl = (erp[0x09] >> 6) & 3;
                sync_on = (sync_lvl == 2) ? 1 : ((sync_lvl == 1) ? (blink ? 1 : 0) : 0);

                uint8_t master_lvl = (erp[0x08] >> 4) & 3;
                master_on = (master_lvl == 2) ? 1 : 0;

                uint8_t mt_lvl = (erp[0x08] >> 2) & 3;
                mt_on = (mt_lvl >= 1) ? 1 : 0;

                uint8_t in_lvl = (erp[0x0A] >> 6) & 3;
                in_on = (in_lvl == 2) ? 1 : ((in_lvl == 1 || armed) ? (blink ? 1 : 0) : 0);

                uint8_t out_lvl = (erp[0x0A] >> 4) & 3;
                out_on = (out_lvl == 2) ? 1 : ((out_lvl == 1) ? (blink ? 1 : 0) : 0);

                uint8_t reloop_lvl = (erp[0x09] >> 0) & 3;
                reloop_on = (reloop_lvl >= 1) ? 1 : 0;
            } else {
                /* Fallback logic (before EP147 connects or for inactive background deck) */
                int looping = (shm) ? shm->is_looping[d] : 0;
                play_on = playing ? 1 : (loaded ? (blink ? 1 : 0) : 0);
                cue_on  = (!playing && loaded) ? 1 : 0;
                sync_on = (shm && shm->is_sync[d]) ? 1 : 0;
                master_on = (shm && shm->is_master[d]) ? 1 : 0;
                mt_on = 1;
                in_on = (looping || armed) ? (blink ? 1 : 0) : (loaded ? 1 : 0);
                out_on = looping ? (blink ? 1 : 0) : 0;
                reloop_on = looping ? 1 : 0;
            }

            /* PLAY/PAUSE: Note 11 (Solid on play, blinking on paused/cued) */
            led_send_deck(d, 11, &led_deck_last[d][L_PLAY], play_on, force);

            /* CUE: Note 12 (Solid on paused/cue ready, dark on play) */
            led_send_deck(d, 12, &led_deck_last[d][L_CUE], cue_on, force);

            /* BEAT SYNC: Note 88 */
            led_send_deck(d, 88, &led_deck_last[d][L_SYNC], sync_on, force);

            /* MASTER: Note 92 */
            led_send_deck(d, 92, &led_deck_last[d][L_MASTER], master_on, force);

            /* MASTER TEMPO / KEY LOCK: Note 93 */
            led_send_deck(d, 93, &led_deck_last[d][L_KEYLOCK], mt_on, force);

            /* LOOP IN: Note 16 */
            led_send_deck(d, 16, &led_deck_last[d][L_LOOPIN], in_on, force);

            /* LOOP OUT: Note 17 */
            led_send_deck(d, 17, &led_deck_last[d][L_LOOPOUT], out_on, force);

            /* RELOOP / EXIT: Note 77 */
            led_send_deck(d, 77, &led_deck_last[d][L_RELOOP], reloop_on, force);

            /* CUE/LOOP CALL < (Note 81) & > (Note 83) */
            led_send_deck(d, 81, &led_deck_last[d][L_CALL_PREV], loaded ? 1 : 0, force);
            led_send_deck(d, 83, &led_deck_last[d][L_CALL_NEXT], loaded ? 1 : 0, force);

            /* 2. Pad Mode Buttons: Exactly ONE lit per deck! */
            int cur_mode = padmode_get(d);
            led_send_deck(d, N_MODE_HOT,  &led_deck_last[d][L_MODE0], (cur_mode == PM_MODE_HOTCUE), force);
            led_send_deck(d, N_MODE_PFX1, &led_deck_last[d][L_MODE1], (cur_mode == PM_MODE_BEATLOOP), force);
            led_send_deck(d, N_MODE_JUMP, &led_deck_last[d][L_MODE2], (cur_mode == PM_MODE_SLIPLOOP), force);
            led_send_deck(d, N_MODE_SMPL, &led_deck_last[d][L_MODE3], (cur_mode == PM_MODE_BEATJUMP), force);

            /* 3. Performance Pads 1..8 LEDs from EP147 ErP MOSI */
            int pch = s_pad_channels[d];
            int base = padmode_note_base(cur_mode);

            /* If mode changed, clear previous base pad notes */
            if (base != led_pad_base[d]) {
                if (led_pad_base[d] >= 0) {
                    for (int p = 0; p < 8; p++) {
                        midi_note(pch, led_pad_base[d] + p, 0x00);
                    }
                    memset(led_pad_last[d], -1, sizeof(led_pad_last[d]));
                }
                led_pad_base[d] = base;
            }

            for (int p = 0; p < 8; p++) {
                int pnote = base + p;
                int want_on = 0;
                if (shm && shm->magic == XDJ_AZ_SHM_MAGIC && shm->mosi_count[0] > 0) {
                    /* Left deck pads: 30 + p*4; Right deck pads: 86 + p*4 */
                    int pad_off = (d == 0 || d == 2) ? (30 + p * 4) : (86 + p * 4);
                    uint8_t r = shm->mosi_buf[0][pad_off + 0];
                    uint8_t g = shm->mosi_buf[0][pad_off + 1];
                    uint8_t b = shm->mosi_buf[0][pad_off + 2];
                    if ((r + g + b) > 30) {
                        want_on = 0x7F; /* Solid ON / Hot Cue present */
                    } else if ((r + g + b) > 0) {
                        want_on = 0x01; /* Dim / ready */
                    } else {
                        want_on = 0x00;
                    }
                } else {
                    want_on = (p < 4) ? 0x7F : 0x00;
                }
                if (want_on != led_pad_last[d][p] || force) {
                    led_pad_last[d][p] = want_on;
                    midi_note(pch, pnote, want_on);
                    if (d == active_left_deck && pch != 7) midi_note(7, pnote, want_on);
                    if (d == active_right_deck && pch != 9) midi_note(9, pnote, want_on);
                }
            }

            /* 4. Mixer Strip CUE (PFL) LED: Note 84 on Ch 0..3 */
            int ch_cue = me_get_cue(d);
            led_send(sch, 84, &led_pfl_last[d], ch_cue, force);

            /* 5. Jog Display Ring Position (CC 0..3 on Channel 11, 1..72) from live playhead */
            if (loaded) {
                long pos = rbp_get_playing_pos(d);
                long p = pos % 79380;
                if (p < 0) p += 79380;
                int led = (int)((double)p * 72.0 / 79380.0) + 1;
                if (led < 1) led = 1;
                if (led > 72) led = 72;
                if (led != (int)s_ring_pos[d] || (tick % 4) == 0) {
                    s_ring_pos[d] = (unsigned char)led;
                    midi_cc(11, d, s_ring_pos[d]);
                }
            } else {
                if (s_ring_pos[d] != 0) {
                    s_ring_pos[d] = 0;
                    midi_cc(11, d, 0);
                }
            }
        }

        /* 6. Master Headphone CUE LED: Note 99 on Channel 6 */
        int mst_cue = me_get_master_cue();
        led_send(6, 99, &led_mc_last, mst_cue, force);
    }

    if (shm && shm_fd >= 0) {
        munmap(shm, sizeof(xdj_az_shared_t));
        close(shm_fd);
    }
    return NULL;
}

