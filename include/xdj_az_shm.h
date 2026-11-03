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
} xdj_az_shared_t;
#pragma pack(pop)

#endif /* XDJ_AZ_SHM_H */
