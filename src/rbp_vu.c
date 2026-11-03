/*
 * rbp_vu.c -- DDJ-FLX6 / XDJ-AZ VU Meter and Fader/Trim State Implementation
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
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "rbp_vu.h"
#include "flx6_bridge.h"
#include "xdj_az_shm.h"

int g_fader[4]      = { 1023, 1023, 1023, 1023 };
int g_fader_seen[4] = { 0, 0, 0, 0 };

int g_trim[4]       = { 512, 512, 512, 512 }; /* Center detent 0 dB */
int g_trim_seen[4]  = { 0, 0, 0, 0 };

/* Pioneer DDJ-FLX6 / FLX4 7-Step Banded VU Meter Values (CC 2) */
static const uint8_t s_flx6_meter_values[7] = {
    0x00, /* 0: Dark / Silent */
    0x26, /* 1: Green 1 (-24 dB) */
    0x41, /* 2: Green 2 (-12 dB) */
    0x57, /* 3: Orange 1 (-6 dB) */
    0x65, /* 4: Orange 2 (-2 dB) */
    0x77, /* 5: Red (0 dB) */
    0x7F  /* 6: Red full / Peak Clip (+3 dB) */
};

static void update_channel_gain(int idx)
{
    if (idx < 0 || idx >= 4) return;
    float f_norm = (float)g_fader[idx] / 1023.0f;
    float t_mult = 1.0f;

    if (g_trim[idx] <= 512) {
        t_mult = (float)g_trim[idx] / 512.0f; /* 0.0 .. 1.0 */
    } else {
        t_mult = 1.0f + ((float)(g_trim[idx] - 512) / 511.0f); /* 1.0 .. 2.0 (+6dB) */
    }

    float total_gain = f_norm * t_mult;
    if (total_gain > 1.0f) total_gain = 1.0f;
    if (total_gain < 0.0f) total_gain = 0.0f;

    me_set_channel_fader(idx, total_gain);
}

void fader_state_set(int ch, int v)
{
    if (ch < 0 || ch >= 4) return;
    if (v < 0) v = 0;
    if (v > 1023) v = 1023;

    if (g_fader[ch] != v || !g_fader_seen[ch]) {
        g_fader[ch] = v;
        g_fader_seen[ch] = 1;
        update_channel_gain(ch);
    }
}

void trim_state_set(int ch, int v)
{
    if (ch < 0 || ch >= 4) return;
    if (v < 0) v = 0;
    if (v > 1023) v = 1023;

    if (g_trim[ch] != v || !g_trim_seen[ch]) {
        g_trim[ch] = v;
        g_trim_seen[ch] = 1;
        update_channel_gain(ch);
    }
}

void xfader_state_set(int v)
{
    if (v < 0) v = 0;
    if (v > 1023) v = 1023;
    float f = (float)v / 1023.0f;
    me_set_crossfader(f);
}

void vu_mixer_defaults_tick(unsigned long t)
{
    /* Seed unity master level and unseen channel faders for the first 30 seconds */
    if (t < 1200 && (t % 80) == 0) {
        send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);
        for (int m = 0; m < 4; m++) {
            if (!g_fader_seen[m]) {
                update_channel_gain(m);
            }
        }
    }
}

void *vu_thread(void *arg)
{
    (void)arg;
    unsigned long t = 0;
    int shm_fd = -1;
    xdj_az_shared_t *shm = NULL;
    uint8_t last_vu[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

    while (1) {
        usleep(25000); /* 40 Hz / 25 ms */
        t++;

        vu_mixer_defaults_tick(t);

        if (!shm) {
            shm_fd = shm_open(XDJ_AZ_SHM_FILE, O_RDONLY, 0666);
            if (shm_fd < 0) shm_fd = open("/dev/shm/xdj_az_shared", O_RDONLY, 0666);
            if (shm_fd >= 0) {
                void *p = mmap(NULL, sizeof(xdj_az_shared_t), PROT_READ, MAP_SHARED, shm_fd, 0);
                if (p != MAP_FAILED) shm = (xdj_az_shared_t*)p;
            }
        }

        if (shm && shm->magic == XDJ_AZ_SHM_MAGIC) {
            for (int d = 0; d < 4; d++) {
                uint8_t meter_val = shm->vu_channel[d];
                if (meter_val <= 6) {
                    meter_val = s_flx6_meter_values[meter_val];
                }
                if (meter_val != last_vu[d] || (t % 8) == 0) {
                    last_vu[d] = meter_val;
                    midi_cc(d, 2, meter_val);
                }
            }
        }
    }

    if (shm && shm_fd >= 0) {
        munmap(shm, sizeof(xdj_az_shared_t));
        close(shm_fd);
    }
    return NULL;
}

