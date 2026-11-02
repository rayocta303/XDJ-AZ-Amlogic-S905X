/*
 * test_subucom.c — Unit Test for Subucom Frame Construction and Verification
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "subucom_protocol.h"

int main(void) {
    printf("[TEST] Starting Subucom Frame Framing Test Suite...\n");

    /* 1. ERP Bus Framing (Bus 1, 128 bytes, CRC at 28-29) */
    uint8_t erp[SUBUCOM_ERP_FRAME_LEN];
    memset(erp, 0, sizeof(erp));
    erp[0] = SUBUCOM_BUS_ERP;
    erp[1] = 0x01; /* Play button pressed */
    subucom_stamp_crc(erp, SUBUCOM_ERP_DATA_LEN, SUBUCOM_ERP_CRC_OFFSET);
    bool erp_ok = subucom_verify_crc(erp, SUBUCOM_ERP_DATA_LEN, SUBUCOM_ERP_CRC_OFFSET);
    printf("  [1] ERP Frame (128 bytes, CRC @ 28): stamped 0x%02X%02X -> %s\n",
           erp[28], erp[29], erp_ok ? "PASS" : "FAIL");
    assert(erp_ok);

    /* 2. Mixer Bus Framing (Bus 3, 128 bytes, CRC at 96-97) */
    uint8_t mixer[SUBUCOM_MIXER_FRAME_LEN];
    memset(mixer, 0, sizeof(mixer));
    mixer[0] = SUBUCOM_BUS_MIXER;
    mixer[4] = 0x7F; /* Channel 1 Fader Max (127) */
    mixer[5] = 0x40; /* Channel 1 EQ Mid (64) */
    subucom_stamp_crc(mixer, SUBUCOM_MIXER_DATA_LEN, SUBUCOM_MIXER_CRC_OFFSET);
    bool mixer_ok = subucom_verify_crc(mixer, SUBUCOM_MIXER_DATA_LEN, SUBUCOM_MIXER_CRC_OFFSET);
    printf("  [2] Mixer Frame (128 bytes, CRC @ 96): stamped 0x%02X%02X -> %s\n",
           mixer[96], mixer[97], mixer_ok ? "PASS" : "FAIL");
    assert(mixer_ok);

    /* 3. Deck 1 Bus Framing (Bus 5, 152 bytes, CRC at 0x78) */
    uint8_t deck1[SUBUCOM_DECK_FRAME_LEN];
    memset(deck1, 0, sizeof(deck1));
    deck1[0] = SUBUCOM_BUS_DECK1;
    deck1[8] = 0x31; /* Deck 1 status header */
    subucom_stamp_crc(deck1, 0x78, SUBUCOM_DECK_CRC_OFFSET);
    bool deck_ok = subucom_verify_crc(deck1, 0x78, SUBUCOM_DECK_CRC_OFFSET);
    printf("  [3] Deck 1 Frame (152 bytes, CRC @ 0x78): stamped 0x%02X%02X -> %s\n",
           deck1[0x78], deck1[0x79], deck_ok ? "PASS" : "FAIL");
    assert(deck_ok);

    /* 4. Deck 2 Bus Framing (Bus 2, 152 bytes, CRC at 0x78) */
    uint8_t deck2[SUBUCOM_DECK_FRAME_LEN];
    memset(deck2, 0, sizeof(deck2));
    deck2[0] = SUBUCOM_BUS_DECK2;
    deck2[8] = 0x41; /* Deck 2 status header */
    subucom_stamp_crc(deck2, 0x78, SUBUCOM_DECK_CRC_OFFSET);
    bool deck2_ok = subucom_verify_crc(deck2, 0x78, SUBUCOM_DECK_CRC_OFFSET);
    printf("  [4] Deck 2 Frame (152 bytes, CRC @ 0x78): stamped 0x%02X%02X -> %s\n",
           deck2[0x78], deck2[0x79], deck2_ok ? "PASS" : "FAIL");
    assert(deck2_ok);

    /* 5. Tamper detection */
    mixer[4] ^= 0x01; /* Mutate 1 bit without restamping */
    bool tamper_detected = !subucom_verify_crc(mixer, SUBUCOM_MIXER_DATA_LEN, SUBUCOM_MIXER_CRC_OFFSET);
    printf("  [5] Tamper Detection: corrupted packet rejected -> %s\n",
           tamper_detected ? "PASS" : "FAIL");
    assert(tamper_detected);

    printf("[PASS] All Subucom Frame tests passed successfully!\n");
    return 0;
}
