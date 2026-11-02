/*
 * subucom_protocol.h — AlphaTheta XDJ-AZ Subucom Front-Panel Protocol Definitions
 *
 * References:
 * - az-runtime-lab-main (SPI bus captures & frame timings)
 * - Pioneer Pro DJ Link / JUCE EP147 hardware interface
 */

#ifndef SUBUCOM_PROTOCOL_H
#define SUBUCOM_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bus IDs corresponding to /dev/subucom_spi*.0 */
#define SUBUCOM_BUS_ERP    1  /* Buttons, pad modes, pads, deck select */
#define SUBUCOM_BUS_DECK2  2  /* Deck 2: Jog wheel, tempo, display msgs */
#define SUBUCOM_BUS_MIXER  3  /* Mixer knobs, faders, browse rotary */
#define SUBUCOM_BUS_DECK1  5  /* Deck 1: Jog wheel, tempo, display msgs */

/* Frame sizes */
#define SUBUCOM_ERP_FRAME_LEN    128
#define SUBUCOM_MIXER_FRAME_LEN  128
#define SUBUCOM_DECK_FRAME_LEN   152

/* CRC offsets */
#define SUBUCOM_ERP_DATA_LEN     28
#define SUBUCOM_ERP_CRC_OFFSET   28  /* 2 bytes at 28-29 */

#define SUBUCOM_MIXER_DATA_LEN   96
#define SUBUCOM_MIXER_CRC_OFFSET 96  /* 2 bytes at 96-97 */

#define SUBUCOM_DECK_CRC_OFFSET  0x78 /* 2 bytes at 0x78-0x79 */

/*
 * CRC-16/X-25 Implementation for XDJ-AZ Subucom Frames
 * Polynomial: 0x8408 (reversed/reflected 0x1021)
 * Initial: 0xFFFF, Final XOR: 0xFFFF
 */
static inline uint16_t subucom_calc_crc16(const uint8_t *data, size_t len) {
    if (!data || len == 0) return 0x0000;
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x8408;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

/* Helper to verify frame CRC */
static inline bool subucom_verify_crc(const uint8_t *frame, size_t data_len, size_t crc_offset) {
    if (!frame) return false;
    uint16_t computed = subucom_calc_crc16(frame, data_len);
    uint16_t stored = (uint16_t)frame[crc_offset] | ((uint16_t)frame[crc_offset + 1] << 8);
    return computed == stored;
}

/* Helper to stamp CRC into frame */
static inline void subucom_stamp_crc(uint8_t *frame, size_t data_len, size_t crc_offset) {
    if (!frame) return;
    uint16_t crc = subucom_calc_crc16(frame, data_len);
    frame[crc_offset]     = (uint8_t)(crc & 0xFF);
    frame[crc_offset + 1] = (uint8_t)((crc >> 8) & 0xFF);
}

#ifdef __cplusplus
}
#endif

#endif /* SUBUCOM_PROTOCOL_H */
