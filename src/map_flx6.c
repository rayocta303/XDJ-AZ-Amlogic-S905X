/*
 * map_flx6.c -- Pioneer DJ / AlphaTheta DDJ-FLX6 control surface mapping.
 * Target: AlphaTheta XDJ-AZ on Amlogic S905X.
 *
 * PROVENANCE:
 * Every note, CC, and channel assignment is taken directly from Pioneer DJ's
 * official publication: "DDJ-FLX6 List of MIDI messages" (Ver 1.0) and
 * the reference implementation in XDJ-RX3 map_flx6.c.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sound/asequencer.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>

#include "ctrl_map.h"
#include "flx6_bridge.h"
#include "padmode_zone.h"
#include "rbp_vu.h"
#include "rbp_led.h"

/* ---- MIDI receive channels, 0-based (List channel minus 1) ---- */
#define CH_DECK1  0   /* List ch 1  -- deck 1 */
#define CH_DECK2  1   /* List ch 2  -- deck 2 */
#define CH_DECK3  2   /* List ch 3  -- deck 3 */
#define CH_DECK4  3   /* List ch 4  -- deck 4 */
#define CH_FX1    4   /* List ch 5  -- Beat FX 1 */
#define CH_FX2    5   /* List ch 6  -- Beat FX 2 */
#define CH_MIX    6   /* List ch 7  -- mixer knobs, browse, master */

/* Pads (Normal / +SHIFT) */
#define CH_PADS1  7   /* List ch 8  -- deck 1 pads */
#define CH_PADS1S 8   /* List ch 9  -- deck 1 pads +SHIFT */
#define CH_PADS2  9   /* List ch 10 -- deck 2 pads */
#define CH_PADS2S 10  /* List ch 11 -- deck 2 pads +SHIFT */
#define CH_PADS3  11  /* List ch 12 -- deck 3 pads */
#define CH_PADS3S 12  /* List ch 13 -- deck 3 pads +SHIFT */
#define CH_PADS4  13  /* List ch 14 -- deck 4 pads */
#define CH_PADS4S 14  /* List ch 15 -- deck 4 pads +SHIFT */

/* ---- Deck button notes (channels 0..3) ---- */
#define N_PLAY        11   /* PLAY/PAUSE (+SHIFT 71) */
#define N_CUE         12   /* CUE        (+SHIFT 72) */
#define N_SHIFT       63   /* SHIFT */
#define N_JOG_TOUCH   54   /* Platter touch (+SHIFT 103) */
#define N_IN          16   /* Loop IN    (+SHIFT 76) */
#define N_OUT         17   /* Loop OUT   (+SHIFT 119) */
#define N_IN_4BEAT    20   /* Long-press IN -> Auto 4-Beat Loop */
#define N_IN_SHIFT    76   /* +SHIFT IN -> Auto 4-Beat Loop */
#define N_4BEAT       77   /* Reloop/Exit */
#define N_RELOOP_SHIFT 80  /* +SHIFT Reloop/Exit */
#define N_CALL_PREV   81   /* CUE/LOOP CALL < */
#define N_CALL_NEXT   83   /* CUE/LOOP CALL > */
#define N_CALL_PREV_SHIFT 97 /* +SHIFT CUE/LOOP CALL < -> Track Search Prev */
#define N_CALL_NEXT_SHIFT 98 /* +SHIFT CUE/LOOP CALL > -> Track Search Next */
#define N_MEM_CUE     61   /* CUE/LOOP MEMORY */
#define N_MEM_DEL     62   /* +SHIFT CUE/LOOP MEMORY -> Delete */
#define N_SYNC        88   /* BEAT SYNC  (+SHIFT 93) */
#define N_SYNC_SHIFT  93   /* 1-15L/R +SHIFT BEAT SYNC -> Master Tempo / Key Lock */
#define N_MASTER      92   /* MASTER */
#define N_TEMPO_RANGE 96   /* +SHIFT MASTER -> Tempo Range (+-6/10/16/WIDE) */
#define N_CH_CUE      84   /* Channel CUE (PFL) */
#define N_CH_CUE_SHIFT 104 /* +SHIFT Channel CUE 1-4 -> Color FX */

/* Deck select toggle notes */
#define N_DECK_TOGGLE 114  /* Deck 3/4 button press */
#define N_DECK_STATE  60   /* Deck state feedback */
#define N_DECK_L_SHIFT 124 /* SHIFT + DECK 3 (Left) -> SLIP mode toggle (on CH_MIX) */
#define N_DECK_R_SHIFT 125 /* SHIFT + DECK 4 (Right) -> SLIP mode toggle (on CH_MIX) */
#define N_JOG_CUTTER       28 /* JOG CUTTER button (ch 0..3) */
#define N_JOG_CUTTER_SHIFT 23 /* +SHIFT JOG CUTTER -> Vinyl / CDJ Mode toggle (ch 0..3) */

/* ---- Mixer & Browse buttons on CH_MIX (ch 6) ---- */
#define N_BROWSE_PUSH       65  /* Browse knob push */
#define N_BROWSE_PUSH_SHIFT 66  /* SHIFT + browse knob push -> SOURCE screen */
#define N_BACK              101 /* Dedicated BACK button */
#define N_VIEW              122 /* Dedicated VIEW button -> BROWSE screen */
#define N_VIEW_SHIFT        104 /* Dedicated SHIFT + VIEW button -> SOURCE screen */
#define N_LOAD1             70  /* LOAD deck 1 */
#define N_LOAD2             71  /* LOAD deck 2 */
#define N_LOAD3             72  /* LOAD deck 3 */
#define N_LOAD4             73  /* LOAD deck 4 */
#define N_LOAD1_SHIFT       88  /* SHIFT + LOAD 1 -> UTILITY / Setting */
#define N_LOAD2_SHIFT       89  /* SHIFT + LOAD 2 -> SHORTCUT */
#define N_LOAD3_SHIFT       90  /* SHIFT + LOAD 3 -> Open / Toggle MIDI / SOURCE view */
#define N_LOAD4_SHIFT       91  /* SHIFT + LOAD 4 */
#define N_MASTER_CUE        99  /* MASTER CUE */

/* ---- Beat FX notes (channels 4 & 5) ---- */
#define N_FX_ONOFF          71  /* Beat FX ON/OFF */
#define N_FX_BEATPREV       6   /* BEAT < */
#define N_FX_BEATNEXT       7   /* BEAT > */
#define N_FX_SELECT1        112 /* FX Select 1 */
#define N_FX_SELECT2        113 /* FX Select 2 */
#define N_FX_SELECT3        114 /* FX Select 3 */
#define N_MERGE_FX_PRESS    46  /* Merge FX push */

/* ---- Continuous Controls (CCs) ---- */
#define CC_TEMPO_MSB        0
#define CC_TEMPO_LSB        32
#define CC_JOG_WHEEL        33  /* Outer ring relative 0x40 */
#define CC_JOG_VINYL        34  /* Platter vinyl on */
#define CC_JOG_NOVINYL      35  /* Platter vinyl off */
#define CC_JOG_SHIFT        41
#define CC_TRIM             4
#define CC_EQH              7
#define CC_EQM              11
#define CC_EQL              15
#define CC_FADER            19

/* Mixer & Global CCs (CH_MIX = 6) */
#define CC_XFADER           31
#define CC_MASTER_MSB       8
#define CC_MASTER_LSB       40
#define CC_BOOTH            9
#define CC_HP_MIX           12
#define CC_HP_LEVEL         13
#define CC_CFX1             23  /* Filter / Color FX CH1 */
#define CC_CFX2             24  /* Filter / Color FX CH2 */
#define CC_CFX3             25  /* Filter / Color FX CH3 */
#define CC_CFX4             26  /* Filter / Color FX CH4 */
#define CC_BROWSE           64  /* Browse relative 0x01/0x7F */
#define CC_BROWSE_SHIFT     100
#define CC_FX_DEPTH         2   /* Beat FX Level/Depth (ch 4 & 5) */
#define CC_MERGE_FX         8   /* Merge FX rotary (ch 4 & 5) */

#define MIRROR_GAIN_MID_DEFAULT 0.6

/* ---- Runtime calibration variables ---- */
static int knob_scale = 1;
static int jog_scale = 1;
static int jog_ppr = 720;
static int jog_rev = 0;
static int jog_idle_ms = 120;
static int jog_verbose = 0;
static int tempo_verbose = 0;
static int tempo_rev = 0;
static double mastervol_mid = MIRROR_GAIN_MID_DEFAULT;
static int mastervol_pos = 0x2000;
static int mastervol_have_hi = 0;

/* Active Deck tracking (Left Platter = Deck 1 or 3, Right Platter = Deck 2 or 4) */
int active_left_deck = 0;   /* 0 = Deck 1, 2 = Deck 3 */
int active_right_deck = 1;  /* 1 = Deck 2, 3 = Deck 4 */

static void *flx6_trigger_settings(void *arg)
{
    (void)arg;
    send_rx_key(K_MENU, OP_PRESS, CH_GLOBAL, 0);
    usleep(500000);
    send_rx_key(K_MENU, OP_RELEASE, CH_GLOBAL, 0);
    klog("[map_flx6] FLX6 Settings -> K_MENU triggered\n");
    return NULL;
}

struct jog_ctrl {
    int rch;
    int sch;
    unsigned int vpos;
    float speed;
    unsigned long long last_ms;
    int moving;
    int started;
};

static struct jog_ctrl jog_state[4];

struct flx6_hires_ctrl {
    int have_hi;
    int pos;
};

static struct flx6_hires_ctrl flx6_pitch_state[4] = {
    { 0, 0x2000 }, { 0, 0x2000 }, { 0, 0x2000 }, { 0, 0x2000 }
};

static struct flx6_hires_ctrl flx6_fader_state[4] = {
    { 0, 0x3FFF }, { 0, 0x3FFF }, { 0, 0x3FFF }, { 0, 0x3FFF }
};

static struct flx6_hires_ctrl flx6_trim_state[4] = {
    { 0, 0x2000 }, { 0, 0x2000 }, { 0, 0x2000 }, { 0, 0x2000 }
};

static struct flx6_hires_ctrl flx6_xfader_state = { 0, 0x2000 };

/* Sound Color FX slots (assigned to SHIFT + HEADPHONE CUE 1-4) */
static int flx6_cfx_slots[4] = {
    K_FILTER,   /* Slot 1: Filter */
    K_DUBECHO,  /* Slot 2: Dub Echo */
    K_NOISE,    /* Slot 3: Noise */
    K_SPACE     /* Slot 4: Space */
};
static int flx6_active_cfx_slot = 0;

struct flx6_bfx_slot {
    int fx_type;    /* BFX switch position (0..13) */
    int channel;    /* 0=Deck 1, 1=Deck 2, 2=Deck 3, 3=Deck 4, BFX_CH_MASTER=Master */
    int depth;      /* 0..127 */
    int on;         /* 0=off, 1=on */
};

/* 6 slots on DDJ-FLX6:
 * Slots 0..2: FX Rack 1 (ch 4, 0x94), Notes 71..73, CCs 2, 4, 6
 * Slots 3..5: FX Rack 2 (ch 5, 0x95), Notes 71..73, CCs 2, 4, 6 */
static struct flx6_bfx_slot flx6_slots[6] = {
    { 0, 0, 64, 0 },  /* FX1-1: Delay,   Deck 1 (Note 71, CC 2) */
    { 1, 0, 64, 0 },  /* FX1-2: Echo,    Deck 1 (Note 72, CC 4) */
    { 3, 0, 64, 0 },  /* FX1-3: Spiral,  Deck 1 (Note 73, CC 6) */
    { 4, 1, 64, 0 },  /* FX2-1: Reverb,  Deck 2 (Note 71, CC 2) */
    { 6, 1, 64, 0 },  /* FX2-2: Flanger, Deck 2 (Note 72, CC 4) */
    { 9, 1, 64, 0 },  /* FX2-3: Roll,    Deck 2 (Note 73, CC 6) */
};
static int flx6_active_slot = 0;
static int bfx_led_blink = 0;
static unsigned long long bfx_led_last_ms = 0;
static int bfx_led_last_sent[6] = { -1, -1, -1, -1, -1, -1 };

static const char *flx6_cfx_name(int key)
{
    switch (key) {
    case K_FILTER:  return "Filter";
    case K_DUBECHO: return "Dub Echo";
    case K_NOISE:   return "Noise";
    case K_SPACE:   return "Space";
    case K_CRUSH:   return "Crush";
    case K_SWEEP:   return "Sweep";
    default:        return "Unknown";
    }
}

static int flx6_cfx_from_name(const char *name)
{
    if (!name) return K_FILTER;
    if (!strcasecmp(name, "filter"))  return K_FILTER;
    if (!strcasecmp(name, "dubecho") || !strcasecmp(name, "dub echo")) return K_DUBECHO;
    if (!strcasecmp(name, "noise"))   return K_NOISE;
    if (!strcasecmp(name, "space"))   return K_SPACE;
    if (!strcasecmp(name, "crush"))   return K_CRUSH;
    if (!strcasecmp(name, "sweep"))   return K_SWEEP;
    return K_FILTER;
}

static void flx6_select_cfx_slot(int slot)
{
    if (slot < 0 || slot >= 4) return;
    int key = flx6_cfx_slots[slot];
    if (key <= 0) return;

    for (int d = 1; d <= 4; d++) {
        send_rx_key(key, OP_PRESS, d, 0);
        send_rx_key(key, OP_RELEASE, d, 0);
    }
    flx6_active_cfx_slot = slot;

    klog("[map_flx6] FLX6 SHIFT+CUE%d -> Color FX Slot %d: %s (0x%04x)\n",
         slot + 1, slot + 1, flx6_cfx_name(key), key);
}

static unsigned long long flx6_cfg_last_check = 0;

static void flx6_load_fx_config(void)
{
    const char *path = "/run/xdjaz/flx6_fx.conf";
    FILE *f = fopen(path, "r");
    if (!f) {
        path = "/opt/xdjaz/flx6_fx.conf";
        f = fopen(path, "r");
    }
    if (!f) return;

    int changed = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;

        if (strncasecmp(p, "cfx_slot", 8) == 0) {
            int slot = 0;
            char name[32];
            if (sscanf(p, "cfx_slot%d = %31s", &slot, name) == 2 && slot >= 1 && slot <= 4) {
                int key = flx6_cfx_from_name(name);
                if (key != flx6_cfx_slots[slot - 1]) {
                    flx6_cfx_slots[slot - 1] = key;
                    changed = 1;
                }
            }
        }
    }
    fclose(f);
    if (changed) {
        klog("[map_flx6] Sound Color FX configuration reloaded\n");
    }
}

static void flx6_check_fx_config(void)
{
    unsigned long long now = shim_now_ms();
    if (now - flx6_cfg_last_check < 2000) return;
    flx6_cfg_last_check = now;
    flx6_load_fx_config();
}

static float flx6_mastervol_gain(int pos)
{
    float norm = (float)pos / 16383.0f;
    if (norm <= 0.001f) return 0.0f;
    if (norm >= 0.999f) return 1.0f;
    return norm;
}

static void flx6_mastervol(int cc, int val)
{
    if (cc == CC_MASTER_MSB) {
        mastervol_have_hi = 1;
        mastervol_pos = (mastervol_pos & 0x7f) | (val << 7);
        return;
    }
    mastervol_pos = (mastervol_pos & 0x3f80) | (val & 0x7f);
    if (!mastervol_have_hi) return;
    int pos = mastervol_pos;
    if (pos < 0) pos = 0;
    if (pos > 0x3FFF) pos = 0x3FFF;
    float gain = flx6_mastervol_gain(pos);
    send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, (long)(gain * 1023.0f), gain);
}

/* 14-bit pitch fader across decks */
static void flx6_pitch(int ch, int cc, int val)
{
    if (ch < 0 || ch > 3) return;
    int target_deck = ch;
    if (ch == 0) target_deck = active_left_deck;
    else if (ch == 1) target_deck = active_right_deck;
    int sch = target_deck + 1;
    struct flx6_hires_ctrl *s = &flx6_pitch_state[target_deck];

    if (cc == CC_TEMPO_MSB) {
        s->have_hi = 1;
        s->pos = (s->pos & 0x7f) | (val << 7);
        return;
    }
    if (cc != CC_TEMPO_LSB) return;
    s->pos = (s->pos & 0x3f80) | (val & 0x7f);
    if (!s->have_hi) return;

    int pos = s->pos;
    if (pos < 0) pos = 0;
    if (pos > 0x3FFF) pos = 0x3FFF;
    float norm = ((float)pos - 0x2000) / 8192.0f;
    if (norm > 1.0f) norm = 1.0f;
    if (norm < -1.0f) norm = -1.0f;
    if (tempo_rev) norm = -norm;
    int v10 = (int)((norm + 1.0f) * 511.5f);
    if (v10 < 0) v10 = 0;
    if (v10 > 1023) v10 = 1023;

    send_rx_key_fl(K_TEMPO_SLIDER, OP_VALUE, sch, (long)v10, norm, (long)pos);
    if (tempo_verbose)
        klog("[map_flx6] pitch ch%d (deck %d) pos=0x%04x -> norm=%.3f v10=%d\n",
             ch, sch, pos, (double)norm, v10);
}

/* 14-Bit Channel Fader Tracking (CC 19 MSB + CC 51 LSB) */
static void flx6_channel_fader(int ch, int cc, int val)
{
    if (ch < 0 || ch > 3) return;
    struct flx6_hires_ctrl *s = &flx6_fader_state[ch];
    if (cc == CC_FADER) {
        s->have_hi = 1;
        s->pos = (s->pos & 0x7F) | (val << 7);
        int v10 = (val * 1023) / 127;
        fader_state_set(ch, v10);
        return;
    }
    if (cc == (CC_FADER + 32)) {
        s->pos = (s->pos & 0x3F80) | (val & 0x7F);
        if (!s->have_hi) return;
        int pos = s->pos;
        if (pos < 0) pos = 0;
        if (pos > 0x3FFF) pos = 0x3FFF;
        int v10 = (pos * 1023) / 16383;
        fader_state_set(ch, v10);
    }
}

/* 14-Bit Gain / Trim Tracking (CC 4 MSB + CC 36 LSB) */
static void flx6_channel_trim(int ch, int cc, int val)
{
    if (ch < 0 || ch > 3) return;
    struct flx6_hires_ctrl *s = &flx6_trim_state[ch];
    if (cc == CC_TRIM) {
        s->have_hi = 1;
        s->pos = (s->pos & 0x7F) | (val << 7);
        int v10 = (val * 1023) / 127;
        trim_state_set(ch, v10);
        return;
    }
    if (cc == (CC_TRIM + 32)) {
        s->pos = (s->pos & 0x3F80) | (val & 0x7F);
        if (!s->have_hi) return;
        int pos = s->pos;
        if (pos < 0) pos = 0;
        if (pos > 0x3FFF) pos = 0x3FFF;
        int v10 = (pos * 1023) / 16383;
        trim_state_set(ch, v10);
    }
}

/* 14-Bit Crossfader Tracking (CC 31 MSB + CC 63 LSB) */
static void flx6_crossfader_ctrl(int cc, int val)
{
    struct flx6_hires_ctrl *s = &flx6_xfader_state;
    if (cc == CC_XFADER) {
        s->have_hi = 1;
        s->pos = (s->pos & 0x7F) | (val << 7);
        int v10 = (val * 1023) / 127;
        xfader_state_set(v10);
        return;
    }
    if (cc == (CC_XFADER + 32)) {
        s->pos = (s->pos & 0x3F80) | (val & 0x7F);
        if (!s->have_hi) return;
        int pos = s->pos;
        if (pos < 0) pos = 0;
        if (pos > 0x3FFF) pos = 0x3FFF;
        int v10 = (pos * 1023) / 16383;
        xfader_state_set(v10);
    }
}

static int g_xfader_enabled = 1;

static void flx6_sync_crossfader(void)
{
    static unsigned long long last_check = 0;
    static int cached_curve = -1;
    static int cached_enabled = -1;
    unsigned long long now = shim_now_ms();
    if (now - last_check < 500) return;
    last_check = now;

    int en = 1;
    FILE *fe = fopen("/run/xdjaz/xfader_enable", "r");
    if (!fe) fe = fopen("/opt/xdjaz/xfader_enable", "r");
    if (fe) {
        int v = 1;
        if (fscanf(fe, "%d", &v) == 1 && (v == 0 || v == 1))
            en = v;
        fclose(fe);
    }

    int cv = 1;
    FILE *fc = fopen("/run/xdjaz/xfader_curve", "r");
    if (!fc) fc = fopen("/opt/xdjaz/xfader_curve", "r");
    if (fc) {
        int v = 1;
        if (fscanf(fc, "%d", &v) == 1 && v >= 0 && v <= 2)
            cv = v;
        fclose(fc);
    }

    if (en != cached_enabled) {
        cached_enabled = en;
        g_xfader_enabled = en;
        if (en) {
            me_set_crossfader_assign(0, 1); /* Ch1 -> A */
            me_set_crossfader_assign(1, 2); /* Ch2 -> B */
            me_set_crossfader_assign(2, 1); /* Ch3 -> A */
            me_set_crossfader_assign(3, 2); /* Ch4 -> B */
            me_set_crossfader_curve(cv);
            cached_curve = cv;
            klog("[map_flx6] crossfader enabled (Ch1/3->A, Ch2/4->B, curve=%d)\n", cv);
        } else {
            for (int c = 0; c < 4; c++) me_set_crossfader_assign(c, 0); /* Thru */
            klog("[map_flx6] crossfader disabled (all channels THRU)\n");
        }
    } else if (en && cv != cached_curve) {
        cached_curve = cv;
        me_set_crossfader_curve(cv);
        klog("[map_flx6] crossfader curve changed to %d\n", cv);
    }
}

static float flx6_get_jog_multiplier(void)
{
    static unsigned long long last_check = 0;
    static float cached_mult = 1.0f;
    unsigned long long now = shim_now_ms();
    if (now - last_check >= 500) {
        last_check = now;
        FILE *f = fopen("/run/xdjaz/jog_speed", "r");
        if (!f) f = fopen("/opt/xdjaz/jog_speed", "r");
        if (f) {
            float val = 1.0f;
            if (fscanf(f, "%f", &val) == 1 && val >= 0.01f && val <= 1.0f) {
                cached_mult = val;
            }
            fclose(f);
        }
    }
    return cached_mult;
}

static float jog_accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};

/* Relative jog handler */
static void flx6_jog(int ch, int val)
{
    if (ch < 0 || ch > 3) return;
    int target_deck = ch;
    if (ch == 0) target_deck = active_left_deck;
    else if (ch == 1) target_deck = active_right_deck;

    int d_idx = target_deck;
    struct jog_ctrl *s = &jog_state[d_idx];
    int d = val - 0x40;
    if (d == 0) return;

    unsigned long long t = shim_now_ms();
    if (!s->started) {
        s->started = 1;
        s->last_ms = t;
        return;
    }
    if (jog_rev) d = -d;
    float mult = flx6_get_jog_multiplier();
    jog_accum[d_idx] += (float)d * (float)jog_scale * mult;
    int dp = (int)jog_accum[d_idx];
    jog_accum[d_idx] -= (float)dp;
    if (dp == 0) return;

    if (dp > 4096) dp = 4096;
    if (dp < -4096) dp = -4096;

    float dt = (float)(long long)(t - s->last_ms) / 1000.0f;
    s->last_ms = t;
    if (dt < 0.0005f) dt = 0.0005f;

    s->vpos = (unsigned int)(s->vpos + (unsigned int)dp) & 0xFFFFu;
    float speed = ((float)d / (float)jog_ppr / dt) * mult;
    if (speed > 8.0f) speed = 8.0f;
    if (speed < -8.0f) speed = -8.0f;

    s->moving = 1;
    s->speed = speed;
    send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, speed, (long)s->vpos);
    if (jog_verbose)
        klog("[map_flx6] jog ch%d delta=%d speed=%.2f pos=%u (deck %d)\n",
             ch, d, (double)speed, s->vpos, s->sch);
}

/* Jog idle handler */
static void flx6_jog_idle(void)
{
    unsigned long long t = shim_now_ms();
    for (int i = 0; i < 4; i++) {
        struct jog_ctrl *s = &jog_state[i];
        if (!s->started || !s->moving) continue;
        if ((unsigned long long)(t - s->last_ms) < (unsigned long long)jog_idle_ms) continue;
        s->moving = 0;
        s->speed = 0.0f;
        send_rx_key_fl(K_JOG_ROT, OP_ROTATE, s->sch, 0, 0.0f, (long)s->vpos);
    }
}

/* Browse knob relative delta */
static void flx6_browse(int val)
{
    if (val < 0 || val > 127) return;
    int delta = (val >= 64) ? (val - 128) : val;
    if (delta == 0) return;
    long n = (long)delta * (long)knob_scale;
    send_rx_key(K_SELECTOR, OP_ROTATE, CH_GLOBAL, n < 0 ? -1 : 1);
}

/* CC dispatch */
static void flx6_cc_abs(int ch, int cc, int val)
{
    for (int i = 0; i < abs_map_n; i++) {
        if (abs_map[i].rch != ch || abs_map[i].cc != cc)
            continue;
        int v = cc_to_10bit(val);
        if (v == abs_map[i].last)
            return;
        abs_map[i].last = v;
        int key = abs_map[i].key;
        int sch = abs_map[i].sch;
        float fval = (float)v / 1023.0f;

        send_rx_key_f(key, OP_VALUE, sch, v, fval);
        return;
    }
}

/* Beat FX and LED update routines */
static void flx6_update_bfx_led(void)
{
    unsigned long long now = shim_now_ms();
    if (now - bfx_led_last_ms >= 250) {
        bfx_led_last_ms = now;
        bfx_led_blink = !bfx_led_blink;
    }

    for (int s = 0; s < 6; s++) {
        int rch = (s < 3) ? CH_FX1 : CH_FX2;
        int note = 71 + (s % 3);
        int want_vel = 0x00;

        if (flx6_slots[s].on) {
            want_vel = bfx_led_blink ? 0x7F : 0x00;
        } else if (s == flx6_active_slot) {
            want_vel = 0x7F;
        }

        if (want_vel != bfx_led_last_sent[s]) {
            bfx_led_last_sent[s] = want_vel;
            midi_note(rch, note, want_vel);
        }
    }
}

static int last_beat_cc_val = -1;

static int flx6_beat_to_cc100(int numer, int denom)
{
    if (numer <= 0 || denom <= 0) return 0x05;
    if (numer == 1 && denom == 4) return 0x01;
    if (numer == 1 && denom == 2) return 0x03;
    if (numer == 3 && denom == 4) return 0x04;
    if (numer == 1 && denom == 1) return 0x05;
    if (numer == 2 && denom == 1) return 0x07;
    if (numer == 4 && denom == 1) return 0x09;
    return 0x05;
}

static void flx6_update_beat_fraction_led(void)
{
    int num = rbp_beatfx_numer();
    int den = rbp_beatfx_denom();
    if (num < 0 || den < 0) return;
    int cc_val = flx6_beat_to_cc100(num, den);
    if (cc_val != last_beat_cc_val) {
        last_beat_cc_val = cc_val;
        midi_cc(CH_FX1, 100, cc_val);
        midi_cc(CH_FX2, 100, cc_val);
    }
}

/* ---- Build mapping tables ---- */
static void flx6_build(void)
{
    knob_scale = env_num("KNOB_SCALE", 1);
    if (knob_scale < 1) knob_scale = 1;
    jog_scale = env_num("JOG_SCALE", 1);
    if (jog_scale < 1) jog_scale = 1;
    jog_ppr = env_num("JOG_PPR", 720);
    if (jog_ppr < 1) jog_ppr = 1;
    jog_rev = env_on("JOG_REV", 0);
    jog_idle_ms = env_num("JOG_IDLE_MS", 120);
    if (jog_idle_ms < 10) jog_idle_ms = 10;
    mastervol_mid = env_dnum("MIRROR_GAIN_MID", MIRROR_GAIN_MID_DEFAULT);
    jog_verbose = env_on("JOG_VERBOSE", 0);
    tempo_verbose = env_on("TEMPO_VERBOSE", 0);
    tempo_rev = env_on("TEMPO_REV", 0);

    /* Initialize jog state across 4 decks */
    for (int d = 0; d < 4; d++) {
        jog_state[d].rch = d;
        jog_state[d].sch = d + 1;
        jog_state[d].vpos = 0;
        jog_state[d].speed = 0.0f;
        jog_state[d].last_ms = 0;
        jog_state[d].moving = 0;
        jog_state[d].started = 0;
    }

    /* ---- 4 Decks (ch 0..3: Decks 1..4) ---- */
    for (int d = 0; d < 4; d++) {
        int rch = d;
        int sch = d + 1;

        /* Transport & Loop */
        add_note(rch, N_PLAY, K_PLAY, sch);
        add_note(rch, N_CUE, K_CUE, sch);
        add_note(rch, N_JOG_TOUCH, K_JOG_TOUCH, sch);
        add_note(rch, 103, K_JOG_TOUCH, sch); /* SHIFT + Platter touch */
        add_note(rch, N_IN, K_LOOPIN, sch);
        add_note(rch, N_OUT, K_LOOPOUT, sch);
        add_note(rch, N_4BEAT, K_RELOOP, sch);
        add_note(rch, N_SYNC, K_SYNC, sch);
        add_note(rch, N_SYNC_SHIFT, K_MT, sch);
        add_note(rch, N_MASTER, K_MASTER, sch);
        add_note(rch, N_TEMPO_RANGE, K_TEMPO_RANGE, sch);
        add_note(rch, N_SHIFT, 0, sch);

        /* Memory Cue & Call / Track Search */
        add_note(rch, N_CALL_PREV, K_CALL_PREV, sch);
        add_note(rch, N_CALL_NEXT, K_CALL_NEXT, sch);
        add_note(rch, N_CALL_PREV_SHIFT, K_TRREV, sch);
        add_note(rch, N_CALL_NEXT_SHIFT, K_TRFWD, sch);
        add_note(rch, N_MEM_CUE, K_MEM_CUE, sch);
        add_note(rch, N_MEM_DEL, K_MEM_DEL, sch);

        /* 3-Band Channel EQ, Trim, and Volume Fader */
        add_abs(rch, CC_TRIM, K_TRIM, sch);
        add_abs(rch, CC_EQH, K_EQH, sch);
        add_abs(rch, CC_EQM, K_EQM, sch);
        add_abs(rch, CC_EQL, K_EQL, sch);
        add_abs(rch, CC_FADER, K_FADER, sch);
    }

    /* ---- Mixer / Browse / Global controls on CH_MIX (ch 6) ---- */
    /* 4-Channel Sound Color FX (Filter) */
    add_abs(CH_MIX, CC_CFX1, K_COLOR, 1);
    add_abs(CH_MIX, CC_CFX2, K_COLOR, 2);
    add_abs(CH_MIX, CC_CFX3, K_COLOR, 3);
    add_abs(CH_MIX, CC_CFX4, K_COLOR, 4);

    /* Crossfader & Headphone Cue */
    add_abs(CH_MIX, CC_XFADER, K_XFADER, CH_GLOBAL);
    add_abs(CH_MIX, CC_HP_MIX, K_HPMIX, CH_GLOBAL);
    add_abs(CH_MIX, CC_HP_LEVEL, K_HPLEVEL, CH_GLOBAL);

    /* Browse knob */
    add_note(CH_MIX, N_BROWSE_PUSH, K_SELECTOR, CH_GLOBAL);
    add_note(CH_MIX, N_BROWSE_PUSH_SHIFT, K_SOURCE, CH_GLOBAL);

    /* Dedicated navigation buttons on DDJ-FLX6 */
    add_note(CH_MIX, N_BACK, K_BACK, CH_GLOBAL);
    add_note(CH_MIX, N_VIEW, K_BROWSE, CH_GLOBAL);
    add_note(CH_MIX, N_VIEW_SHIFT, K_SOURCE, CH_GLOBAL);

    /* Dedicated Load buttons 1..4 */
    add_note(CH_MIX, N_LOAD1, K_LOAD, 1);
    add_note(CH_MIX, N_LOAD2, K_LOAD, 2);
    add_note(CH_MIX, N_LOAD3, K_LOAD, 3);
    add_note(CH_MIX, N_LOAD4, K_LOAD, 4);

    /* Shift + Load shortcuts */
    add_note(CH_MIX, N_LOAD2_SHIFT, K_SHORTCUT, CH_GLOBAL);
    add_note(CH_MIX, N_LOAD3_SHIFT, K_SOURCE, CH_GLOBAL);

    /* Merge FX controls */
    add_note(CH_FX1, N_MERGE_FX_PRESS, K_BFX, CH_GLOBAL);
    add_note(CH_FX2, N_MERGE_FX_PRESS, K_BFX, CH_GLOBAL);
    add_abs(CH_FX1, CC_MERGE_FX, K_DEPTH, CH_GLOBAL);
}

/* Startup initialization routine */
static void flx6_startup(void)
{
    az_bridge_init();

    *(volatile uint32_t *)ADDR_MIXER_ROUTE_PLAYER0 = MIXER_ROUTE_INPUT0;
    *(volatile uint32_t *)ADDR_MIXER_ROUTE_PLAYER1 = MIXER_ROUTE_INPUT1;
    klog("[map_flx6] routed Mixer Ch1..4 -> Decks 1..4\n");

    /* Default Sound Color FX to Filter on all channels */
    for (int d = 1; d <= 4; d++) {
        send_rx_key(K_FILTER, OP_PRESS, d, 0);
        send_rx_key(K_FILTER, OP_RELEASE, d, 0);
        send_rx_key_f(K_COLOR, OP_VALUE, d, 512, 0.5f);
    }
    klog("[map_flx6] Sound Color FX initialized to Filter on all channels\n");

    /* Master level set to unity */
    send_rx_key_f(K_MASTERLVL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);
    klog("[map_flx6] master level set to unity\n");

    /* Initialize Headphone Mix & Level: 100% CUE (no Master bleed on boot) */
    send_rx_key_f(K_HPMIX, OP_VALUE, CH_GLOBAL, 0, 0.0f);
    send_rx_key_f(K_HPLEVEL, OP_VALUE, CH_GLOBAL, 1023, 1.0f);

    /* Sync Crossfader */
    flx6_sync_crossfader();

    flx6_load_fx_config();

    last_beat_cc_val = -1;
    flx6_update_beat_fraction_led();

    for (int s = 0; s < 6; s++) {
        flx6_slots[s].on = 0;
        bfx_led_last_sent[s] = -1;
    }
    flx6_update_bfx_led();
}

/* Event dispatcher */
static void flx6_event(const struct snd_seq_event *ev)
{
    if (!ev) return;

    if (ev->type == SNDRV_SEQ_EVENT_NOTEON || ev->type == SNDRV_SEQ_EVENT_NOTEOFF) {
        int ch = ev->data.note.channel;
        int note = ev->data.note.note;
        int on = (ev->type == SNDRV_SEQ_EVENT_NOTEON) && (ev->data.note.velocity > 0);

        /* SHIFT + HEADPHONE CUE 1-4: Switch Sound Color FX */
        if (ch >= 0 && ch <= 3 && note == N_CH_CUE_SHIFT) {
            if (on) {
                flx6_select_cfx_slot(ch);
            }
            return;
        }

        /* Channel CUE (PFL) across channels 0..3 */
        if (ch >= 0 && ch <= 3 && note == N_CH_CUE) {
            if (on) {
                int cur = me_get_cue(ch);
                int want = (cur == 0) ? 1 : 0;
                me_set_cue(ch, want);
                midi_note(ch, N_CH_CUE, want ? 0x7F : 0x00);
                klog("[map_flx6] FLX6 CH CUE ch%d -> cue=%d\n", ch + 1, want);
            }
            return;
        }

        /* Pad Mode buttons: HOT CUE (27), PAD FX 1 (30), BEAT JUMP (32), SAMPLER (34) */
        if (ch >= 0 && ch <= 3 && on) {
            int mode = padmode_from_button_note(note);
            if (mode >= 0) {
                int target_deck = ch;
                if (ch == 0) target_deck = active_left_deck;
                else if (ch == 1) target_deck = active_right_deck;
                padmode_set(target_deck, mode);
                return;
            }
        }

        /* Performance Pads 1..8 (Channels 7..14) */
        if (ch >= CH_PADS1 && ch <= CH_PADS4S) {
            int deck_idx = (ch - CH_PADS1) / 2;
            if (deck_idx == 0 && active_left_deck == 2) deck_idx = 2;
            else if (deck_idx == 1 && active_right_deck == 3) deck_idx = 3;
            int pad_idx = note % 16;
            if (pad_idx >= 0 && pad_idx < 8) {
                int sch = deck_idx + 1;
                int cur_mode = padmode_get(deck_idx);
                int key = K_PAD1 + pad_idx;
                if (cur_mode == PM_MODE_BEATLOOP) key = K_ALOOP;
                else if (cur_mode == PM_MODE_SLIPLOOP) key = K_SLIPLOOP;
                else if (cur_mode == PM_MODE_BEATJUMP) key = K_BEATJUMP;

                send_rx_key(key, on ? OP_PRESS : OP_RELEASE, sch, 0);
                return;
            }
        }

        /* Deck 3/4 button toggle */
        if (note == N_DECK_TOGGLE && on) {
            if (ch == 0 || ch == 2) {
                active_left_deck = (active_left_deck == 0) ? 2 : 0;
                flx6_switch_deck(active_left_deck);
                midi_note(0, N_DECK_TOGGLE, (active_left_deck == 2) ? 0x7F : 0x00);
                klog("[map_flx6] Left Platter toggled to DECK %d\n", active_left_deck + 1);
                return;
            } else if (ch == 1 || ch == 3) {
                active_right_deck = (active_right_deck == 1) ? 3 : 1;
                flx6_switch_deck(active_right_deck);
                midi_note(1, N_DECK_TOGGLE, (active_right_deck == 3) ? 0x7F : 0x00);
                klog("[map_flx6] Right Platter toggled to DECK %d\n", active_right_deck + 1);
                return;
            }
        }

        /* Shift + Deck 3/4 button -> SLIP Mode toggle */
        if (ch == CH_MIX && (note == N_DECK_L_SHIFT || note == N_DECK_R_SHIFT)) {
            int target_deck = (note == N_DECK_L_SHIFT) ? active_left_deck : active_right_deck;
            send_rx_key(K_SLIP, on ? OP_PRESS : OP_RELEASE, target_deck + 1, 0);
            if (on) {
                klog("[map_flx6] SHIFT+DECK %s -> Toggle SLIP Mode on Deck %d\n",
                     (note == N_DECK_L_SHIFT) ? "L (Deck 3)" : "R (Deck 4)", target_deck + 1);
            }
            return;
        }

        /* Shift + Jog Cutter -> Vinyl / CDJ Mode toggle */
        if ((ch >= 0 && ch <= 3) && note == N_JOG_CUTTER_SHIFT) {
            int target_deck = (ch == 0 || ch == 2) ? active_left_deck : active_right_deck;
            send_rx_key(K_VINYL, on ? OP_PRESS : OP_RELEASE, target_deck + 1, 0);
            if (on) {
                klog("[map_flx6] SHIFT+JOG_CUTTER (ch%d) -> Toggle Vinyl/CDJ Mode on Deck %d\n",
                     ch + 1, target_deck + 1);
            }
            return;
        }

        /* Master CUE on CH_MIX */
        if (ch == CH_MIX && note == N_MASTER_CUE) {
            if (on) {
                int cur = me_get_master_cue();
                int want = (cur == 0) ? 1 : 0;
                me_set_master_cue(want);
                midi_note(CH_MIX, N_MASTER_CUE, want ? 0x7F : 0x00);
                klog("[map_flx6] FLX6 master cue -> %s\n", want ? "ON" : "OFF");
            }
            return;
        }

        /* Dedicated Load buttons 1..4 on CH_MIX */
        if (ch == CH_MIX && (note >= N_LOAD1 && note <= N_LOAD4)) {
            if (on) {
                int d = note - N_LOAD1;
                send_rx_key(K_LOAD, OP_PRESS, d + 1, 0);
                send_rx_key(K_LOAD, OP_RELEASE, d + 1, 0);
                led_trigger_load_animation(d);
            }
            return;
        }

        /* Loop In / Out / Reloop arming and dispatch */
        if (ch >= 0 && ch <= 3) {
            int target_deck = ch;
            if (ch == 0) target_deck = active_left_deck;
            else if (ch == 1) target_deck = active_right_deck;
            int target_sch = target_deck + 1;

            if (note == N_IN) {
                if (on) {
                    led_loop_armed[target_deck] = 1;
                    send_rx_key(K_LOOPIN, OP_PRESS, target_sch, 0);
                } else {
                    send_rx_key(K_LOOPIN, OP_RELEASE, target_sch, 0);
                }
                return;
            } else if (note == N_OUT) {
                if (on) {
                    led_loop_armed[target_deck] = 0;
                    send_rx_key(K_LOOPOUT, OP_PRESS, target_sch, 0);
                } else {
                    send_rx_key(K_LOOPOUT, OP_RELEASE, target_sch, 0);
                }
                return;
            } else if (note == N_4BEAT) {
                if (on) {
                    led_loop_armed[target_deck] = 0;
                    send_rx_key(K_RELOOP, OP_PRESS, target_sch, 0);
                } else {
                    send_rx_key(K_RELOOP, OP_RELEASE, target_sch, 0);
                }
                return;
            }
        }

        /* Beat FX CH SELECT buttons */
        if ((ch == CH_FX1 || ch == CH_FX2) && on) {
            int target_ch = -1;
            if (note == 28) target_ch = 0;       /* CH 1 */
            else if (note == 29) target_ch = 1;  /* CH 2 */
            else if (note == 30) target_ch = 2;  /* CH 3 */
            else if (note == 31) target_ch = 3;  /* CH 4 */
            else if (note == 20) target_ch = BFX_CH_MASTER; /* MASTER */

            if (target_ch >= 0) {
                flx6_slots[flx6_active_slot].channel = target_ch;
                send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, target_ch);
                klog("[map_flx6] FLX6 FX CH SELECT note%d -> ch %d (slot %d)\n",
                     note, target_ch, flx6_active_slot + 1);
                return;
            }
        }

        /* Beat FX BEAT < / > */
        if ((ch == CH_FX1 || ch == CH_FX2) && (note == N_FX_BEATPREV || note == N_FX_BEATNEXT)) {
            if (on) {
                int key = (note == N_FX_BEATPREV) ? K_BEATPREV : K_BEATNEXT;
                long d = (note == N_FX_BEATPREV) ? -1 : 1;
                send_rx_key_fl(key, OP_PRESS, CH_GLOBAL, d, 0.0f, d);
                flx6_update_beat_fraction_led();
            }
            return;
        }

        /* Beat FX SELECT buttons: Slot 1 (112), Slot 2 (113), Slot 3 (114) */
        if ((ch == CH_FX1 || ch == CH_FX2) && (note >= 112 && note <= 114)) {
            if (on) {
                int sel = (ch == CH_FX1) ? (note - 112) : (3 + (note - 112));
                if (sel >= 0 && sel < 6) {
                    for (int s = 0; s < 6; s++) flx6_slots[s].on = 0;
                    flx6_active_slot = sel;
                    send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, flx6_slots[sel].fx_type);
                    send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, flx6_slots[sel].channel);
                    send_rx_key_f(K_DEPTH, OP_VALUE, CH_GLOBAL,
                                  flx6_slots[sel].depth * 8,
                                  (float)flx6_slots[sel].depth / 127.0f);
                    klog("[map_flx6] FLX6 BFX SELECT -> Slot %d (type %d, ch %d)\n",
                         sel + 1, flx6_slots[sel].fx_type, flx6_slots[sel].channel);
                }
            }
            return;
        }

        /* Beat FX Slot Enable buttons: Slot 1 (71), Slot 2 (72), Slot 3 (73) */
        if ((ch == CH_FX1 || ch == CH_FX2) && (note >= 71 && note <= 73)) {
            if (on) {
                int slot = (ch == CH_FX1) ? (note - 71) : (3 + (note - 71));
                if (slot >= 0 && slot < 6) {
                    flx6_active_slot = slot;
                    int want_on = !flx6_slots[slot].on;
                    for (int s = 0; s < 6; s++) flx6_slots[s].on = 0;
                    flx6_slots[slot].on = want_on;

                    send_rx_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, flx6_slots[slot].fx_type);
                    send_rx_key(K_BFXCH, OP_VALUE, CH_GLOBAL, flx6_slots[slot].channel);
                    send_rx_key_f(K_DEPTH, OP_VALUE, CH_GLOBAL,
                                  flx6_slots[slot].depth * 8,
                                  (float)flx6_slots[slot].depth / 127.0f);
                    send_rx_key(K_BFX, OP_PRESS, CH_GLOBAL, 0);
                    send_rx_key(K_BFX, OP_RELEASE, CH_GLOBAL, 0);
                    klog("[map_flx6] FLX6 BFX Button Slot %d -> %s\n", slot + 1, want_on ? "ON" : "OFF");
                }
            }
            return;
        }

        /* Auto 4-Beat Loop */
        if ((ch >= 0 && ch <= 3) && (note == N_IN_4BEAT || note == N_IN_SHIFT || note == N_RELOOP_SHIFT)) {
            if (on) {
                int sch = ch + 1;
                send_rx_key(K_LOOPIN, OP_REPEAT, sch, 0);
                send_rx_key(K_LOOPIN, OP_RELEASE, sch, 0);
            }
            return;
        }

        /* SHIFT + LOAD 1 -> Utility / Settings menu */
        if (ch == CH_MIX && note == N_LOAD1_SHIFT) {
            if (on) {
                pthread_t th;
                pthread_create(&th, NULL, flx6_trigger_settings, NULL);
                pthread_detach(th);
            }
            return;
        }

        /* SHIFT + LOAD 3 -> Toggle SOURCE view */
        if (ch == CH_MIX && (note == 96 || note == N_LOAD3_SHIFT)) {
            if (on) {
                send_rx_key(K_SOURCE, OP_PRESS, CH_GLOBAL, 0);
                send_rx_key(K_SOURCE, OP_RELEASE, CH_GLOBAL, 0);
            }
            return;
        }

        /* Standard Table lookup */
        for (int i = 0; i < note_map_n; i++) {
            if (note_map[i].rch != ch || note_map[i].note != note)
                continue;
            int key = note_map[i].key;
            if (key == 0) return;

            int target_sch = note_map[i].sch;
            if (ch == 0) target_sch = active_left_deck + 1;
            else if (ch == 1) target_sch = active_right_deck + 1;

            int *p = &note_map[i].pressed;
            if (on && !*p) {
                *p = 1;
                send_rx_key(key, OP_PRESS, target_sch, 0);
                if (note_map[i].repeat)
                    send_rx_key(key, OP_REPEAT, target_sch, 0);
            } else if (!on && *p) {
                *p = 0;
                send_rx_key(key, OP_RELEASE, target_sch, 0);
            }
            return;
        }
    } else if (ev->type == SNDRV_SEQ_EVENT_CONTROLLER) {
        int ch = ev->data.control.channel;
        int cc = ev->data.control.param;
        int val = ev->data.control.value;

        /* Browse knob on CH_MIX */
        if (ch == CH_MIX && (cc == CC_BROWSE || cc == CC_BROWSE_SHIFT)) {
            flx6_browse(val);
            return;
        }

        /* Master level */
        if (ch == CH_MIX && (cc == CC_MASTER_MSB || cc == CC_MASTER_LSB)) {
            flx6_mastervol(cc, val);
            return;
        }

        /* Jog wheel across decks */
        if ((ch >= 0 && ch <= 3) && (cc == CC_JOG_WHEEL || cc == CC_JOG_VINYL ||
                                     cc == CC_JOG_NOVINYL || cc == CC_JOG_SHIFT)) {
            flx6_jog(ch, val);
            return;
        }

        /* Pitch fader across decks */
        if ((ch >= 0 && ch <= 3) && (cc == CC_TEMPO_MSB || cc == CC_TEMPO_LSB)) {
            flx6_pitch(ch, cc, val);
            return;
        }

        /* 14-Bit Channel Fader Tracking (CC 19 & 51) */
        if ((ch >= 0 && ch <= 3) && (cc == CC_FADER || cc == (CC_FADER + 32))) {
            flx6_channel_fader(ch, cc, val);
            return;
        }

        /* 14-Bit Channel Gain / Trim Tracking (CC 4 & 36) */
        if ((ch >= 0 && ch <= 3) && (cc == CC_TRIM || cc == (CC_TRIM + 32))) {
            flx6_channel_trim(ch, cc, val);
            return;
        }

        /* 14-Bit Crossfader Tracking (CC 31 & 63 on CH_MIX) */
        if (ch == CH_MIX && (cc == CC_XFADER || cc == (CC_XFADER + 32))) {
            flx6_crossfader_ctrl(cc, val);
            return;
        }

        /* Beat FX Depth */
        if ((ch == CH_FX1 || ch == CH_FX2) && (cc == 2 || cc == 4 || cc == 6)) {
            int d_slot = (ch == CH_FX1) ? ((cc == 2) ? 0 : (cc == 4 ? 1 : 2))
                                        : ((cc == 2) ? 3 : (cc == 4 ? 4 : 5));
            flx6_slots[d_slot].depth = val;
            if (d_slot == flx6_active_slot) {
                send_rx_key_f(K_DEPTH, OP_VALUE, CH_GLOBAL, val * 8, (float)val / 127.0f);
            }
            return;
        }

        /* Merge FX rotary turn -> Beat FX Time encoder */
        if ((ch == CH_FX1 || ch == CH_FX2) && (cc == CC_MERGE_FX || cc == (CC_MERGE_FX + 32))) {
            int delta = (val >= 64) ? (val - 128) : val;
            if (delta != 0) {
                send_rx_key_fl(K_TIME, OP_ROTATE, CH_GLOBAL, delta, 0.0f, delta);
            }
            return;
        }

        /* Standard continuous controls */
        flx6_cc_abs(ch, cc, val);
    }
}

static void flx6_tick(void)
{
    flx6_jog_idle();
    flx6_check_fx_config();
    flx6_sync_crossfader();
    flx6_update_bfx_led();
    flx6_update_beat_fraction_led();
}

/* LED & Meter definitions */
static const struct led_notes flx6_leds = {
    .deck_ch = 0,
    .n_sync = 88,
    .n_cue = 12,
    .n_play = 11,
    .n_keylock = 93,
    .n_vinyl = -1,
    .n_slip = -1,
    .n_loopin = 16,
    .n_loopout = 17,
    .n_autoloop = -1,

    .n_mode = { 27, 30, 32, 34 },

    .pad_ch = 7,              /* Deck 1 pads */
    .pad_ch2 = 9,             /* Deck 2 pads */
    .n_pad_base = { 0, 16, 32, 48 },
    .pad_enc = LED_ENC_NONE,

    .strip_ch_first = 0,      /* Ch 1..4 */
    .strip_count = 4,
    .n_strip_cue = 84,

    .master_ch_first = 6,
    .master_ch_count = 1,
    .n_master_cue = 99,

    .fx_ch = 4,               /* Ch 5 (0-based 4) for Beat FX */
    .n_fx = {
        [LED_FX_BFX_ONOFF]   = -1,
        [LED_FX_CFX_FILTER]  = -1,
        [LED_FX_CFX_DUBECHO] = -1,
        [LED_FX_CFX_NOISE]   = -1,
        [LED_FX_CFX_SWEEP]   = -1,
    },

    .meter_ch_first = 0,      /* Decks 1..4 -> channels 0..3 */
    .n_meter_cc = 2,          /* CC 2 */
    .meter_enc = METER_ENC_FLX4_LEVEL,
    .meter_pre_fader = 1,
    .meter_master_ch = -1,
    .n_meter_master_l = -1,
    .n_meter_master_r = -1,
};

const struct ctrl_map map_flx6 = {
    "flx6",
    flx6_build,
    flx6_startup,
    flx6_event,
    flx6_tick,
    NULL,
    NULL,
    &flx6_leds,
};
