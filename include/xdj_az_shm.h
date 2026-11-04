#ifndef XDJ_AZ_SHM_H
#define XDJ_AZ_SHM_H

#include <stdint.h>
#include <stdbool.h>

#define XDJ_AZ_SHM_FILE "/xdj_az_shared"
#define XDJ_AZ_SHM_MAGIC 0x415A5348u /* 'AZSH' */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         /* XDJ_AZ_SHM_MAGIC */
    uint32_t version;       /* 1 */
    uint32_t seq;           /* Incremented on updates */
    
    /* Audio VU meter levels computed from live PCM audio samples (0x00..0x7F) */
    uint8_t  vu_channel[4]; /* Decks 1, 2, 3, 4 (CC 2 value for FLX6) */
    uint8_t  vu_master_l;   /* Master Left (0..127) */
    uint8_t  vu_master_r;   /* Master Right (0..127) */
    uint8_t  vu_cue_l;      /* Headphone Cue Left */
    uint8_t  vu_cue_r;      /* Headphone Cue Right */
    
    /* Track Loaded Flags from EP147 */
    uint8_t  track_loaded[4]; /* 1 if deck has track loaded */
    
    /* EP147 Hardware MOSI LED & Jog buffers (152 bytes each) */
    uint32_t mosi_size[4];    /* [0]=ErP, [1]=Deck2, [2]=Mixer, [3]=Deck1 */
    uint8_t  mosi_buf[4][152];
    uint32_t mosi_count[4];

    /* DSP Mixer & FX Controls (from FLX6 hardware controls) */
    float    channel_gain[4];       /* Channel 1..4 Volume fader (0.0..1.0) */
    int8_t   cross_assign[4];       /* Crossfader assign: -1 (A), 0 (THRU), 1 (B) */
    float    cross_position;        /* Crossfader position (0.0 A .. 1.0 B) */
    float    master_gain;           /* Master Volume (0.0 .. 1.0) */
    float    headphones_gain;       /* Headphone Volume (0.0 .. 1.0) */
    float    cue_master_mix;        /* Headphone Mix (0.0 CUE .. 1.0 MASTER) */
    uint8_t  cue_mask;              /* Headphone Cue bitmask: bit 0=Ch1, 1=Ch2, 2=Ch3, 3=Ch4, 4=Master */

    /* 3-Band EQ per channel (High, Mid, Low: 0.0..1.0, 0.5=center 0dB) */
    float    eq_high[4];
    float    eq_mid[4];
    float    eq_low[4];
    uint8_t  eq_mode[4];            /* 0=EQ, 1=Isolator */

    /* Sound Color FX per channel */
    uint8_t  cfx_type[4];           /* 0=Off, 1=Filter, 2=Noise, 3=Sweep, 4=Dub Echo, 5=Space, 6=Crush */
    float    cfx_color[4];          /* 0.0..1.0 (0.5=center off) */
    float    cfx_param[4];          /* 0.0..1.0 parameter */
    
    uint32_t dsp_seq;               /* Incremented on DSP control updates */

    /* Playhead position in samples and transport telemetry from EP147 */
    int64_t  playhead_pos[4];       /* Cumulative samples rendered (supports negative lead-in) */
    uint8_t  is_playing[4];         /* 1 if audio output actively playing */
    uint8_t  is_sync[4];            /* 1 if Beat Sync enabled */
    uint8_t  is_master[4];          /* 1 if Master Deck */
    uint8_t  is_looping[4];         /* 1 if Loop active / loop adjust */
} xdj_az_shared_t;
#pragma pack(pop)

#endif /* XDJ_AZ_SHM_H */
