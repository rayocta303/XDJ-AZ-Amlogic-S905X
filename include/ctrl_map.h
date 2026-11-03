/*
 * ctrl_map.h -- Control surface abstraction and binding table definitions.
 * Ported from XDJ-RX3 reference architecture for AlphaTheta XDJ-AZ / S905X.
 */
#ifndef XDJ_AZ_CTRL_MAP_H
#define XDJ_AZ_CTRL_MAP_H

#include <sound/asequencer.h>   /* struct snd_seq_event */
#include <stdint.h>
#include <stdbool.h>

#define CTRL_NKEYS 384
#define CTRL_NABS  64

/* Button / Note binding: maps an incoming MIDI channel + note to an internal keycode */
struct note_ctrl {
    int rch;              /* receive channel (0-based) */
    int note;             /* MIDI note number */
    int key;              /* internal keycode (0 = log-only) */
    int sch;              /* target deck / channel (1-based, or CH_GLOBAL) */
    int pressed;          /* pressed state tracking (0 / 1) */
    int repeat;           /* send repeat edge */
};
extern struct note_ctrl note_map[];
extern int note_map_n;

/* Absolute control binding: maps a CC to an internal control parameter */
struct abs_ctrl {
    int rch;              /* receive channel (0-based) */
    int cc;               /* CC controller number */
    int key;              /* internal control key */
    int sch;              /* target deck / channel */
    int last;             /* last 10-bit value sent (-1 = unknown) */
};
extern struct abs_ctrl abs_map[];
extern int abs_map_n;

/* Sound Color FX & Beat FX LED identifiers */
enum led_fx {
    LED_FX_BFX_ONOFF = 0,
    LED_FX_CFX_FILTER,
    LED_FX_CFX_DUBECHO,
    LED_FX_CFX_NOISE,
    LED_FX_CFX_SWEEP,
    LED_FX_COUNT
};

enum led_enc {
    LED_ENC_NONE = 0,
    LED_ENC_PRIME_6BIT,
};

enum meter_enc {
    METER_ENC_NONE = 0,
    METER_ENC_PRIME_SEGMENTS,
    METER_ENC_FLX4_LEVEL,
};

/* Panel LED & Meter layout description */
struct led_notes {
    int deck_ch;          /* deck 1's send channel (0-based); deck 2 is +1 */

    int n_sync, n_cue, n_play, n_keylock, n_vinyl, n_slip;
    int n_loopin, n_loopout, n_autoloop;

    int n_mode[4];        /* 0: Hot Cue, 1: Beat Loop, 2: Slip Loop, 3: Beat Jump */

    int pad_ch;           /* deck 1 pads send channel */
    int pad_ch2;          /* deck 2 pads send channel */
    int n_pad_base[4];    /* pad note base per mode */
    int pad_enc;

    int strip_ch_first;   /* mixer strip first channel */
    int strip_count;      /* number of strips */
    int n_strip_cue;      /* channel headphone cue note */

    int master_ch_first;
    int master_ch_count;
    int n_master_cue;

    int fx_ch;
    int n_fx[LED_FX_COUNT];

    int meter_ch_first;
    int n_meter_cc;
    int meter_enc;
    int meter_pre_fader;
    int meter_master_ch;
    int n_meter_master_l;
    int n_meter_master_r;
};

void ctrl_bindings_reset(void);
void add_note(int rch, int note, int key, int sch);
void add_note_repeat(int rch, int note, int key, int sch);
void add_abs(int rch, int cc, int key, int sch);
void ctrl_abs_invalidate(void);

/* Controller map descriptor */
struct ctrl_map {
    const char *name;
    void (*build)(void);
    void (*startup)(void);
    void (*event)(const struct snd_seq_event *ev);
    void (*tick)(void);
    int (*devices)(void);
    void (*input)(int type, int code, int value);
    const struct led_notes *leds;
};

extern const struct ctrl_map map_flx6;

#endif /* XDJ_AZ_CTRL_MAP_H */

