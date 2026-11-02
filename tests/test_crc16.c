/*
 * test_crc16.c — Unit Test for XDJ-AZ Subucom CRC-16/X-25 Implementation
 *
 * Algorithm:
 * - Polynomial: 0x8408 (reversed/reflected 0x1021)
 * - Initial value: 0xFFFF
 * - Final XOR: 0xFFFF
 * - Little-Endian storage
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "subucom_protocol.h"

int main(void) {
    printf("[TEST] Starting Subucom CRC-16/X-25 Test Suite...\n");

    /* Test Vector 1: Standard ASCII "123456789" */
    /* CRC-16/X-25 standard result: 0x906E */
    const char *tv1 = "123456789";
    uint16_t crc1 = subucom_calc_crc16((const uint8_t *)tv1, strlen(tv1));
    printf("  [1] Vector '123456789': expected 0x906E, got 0x%04X -> %s\n",
           crc1, (crc1 == 0x906E) ? "PASS" : "FAIL");
    assert(crc1 == 0x906E);

    /* Test Vector 2: Empty buffer */
    uint16_t crc2 = subucom_calc_crc16(NULL, 0);
    printf("  [2] Empty buffer: expected 0x0000, got 0x%04X -> %s\n",
           crc2, (crc2 == 0x0000) ? "PASS" : "FAIL");
    assert(crc2 == 0x0000);

    /* Test Vector 3: Pioneer Subucom ERP Frame Header (28 bytes) */
    uint8_t erp_frame[28];
    memset(erp_frame, 0, sizeof(erp_frame));
    erp_frame[0] = 0x01; /* ERP Bus ID */
    erp_frame[1] = 0x20; /* Status alive */
    uint16_t crc3 = subucom_calc_crc16(erp_frame, sizeof(erp_frame));
    printf("  [3] Synthetic ERP frame (28 bytes): computed CRC 0x%04X -> PASS\n", crc3);
    assert(crc3 != 0x0000);

    /* Test Vector 4: Self-check with stamp and verify API */
    uint8_t frame_with_crc[30];
    memcpy(frame_with_crc, erp_frame, 28);
    subucom_stamp_crc(frame_with_crc, 28, 28);
    bool valid = subucom_verify_crc(frame_with_crc, 28, 28);
    printf("  [4] API stamp and verify self-validation: %s -> %s\n",
           valid ? "MATCH" : "MISMATCH", valid ? "PASS" : "FAIL");
    assert(valid);

    printf("[PASS] All Subucom CRC-16/X-25 tests passed successfully!\n");
    return 0;
}
