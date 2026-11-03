/*
 * xdj_az_abi64.h — 64-bit ABI and Hardware Register Definitions for AlphaTheta XDJ-AZ
 * Target: Amlogic S905X (Quad-Core AArch64 / ARMv8-A Cortex-A53)
 *
 * Ground Truth Reference:
 *   - AlphaTheta XDJ-AZ EP147 Native Binary (Rockchip Linux 4.4 -> Linux 6.6)
 *   - Kyle-Hosman/xdj-az-emu subucom_virt & subucom_frames.h hardware captures
 *
 * Implements:
 *   1. Hardware SPI bus device nodes (/dev/subucom_spi{1,2,3,5}.0 and /dev/subucom_ctrl)
 *   2. Native IOCTL codes with magic 'p' (0x70) & 16-byte MOSI command structures
 *   3. 152-byte MISO & MOSI frame layouts (128 real bytes + 24 zero padding)
 *   4. Authentic per-bus CRC-16/X-25 lengths (spi1=28, spi2=38, spi3=96, spi5=38)
 *   5. Captured hardware idle frames for all 4 buses (ErP, Deck2, Mixer, Deck1)
 *   6. Full 10-bit analog mixer registers (Ch 1..4 faders, crossfader, trims, 3-band EQs, Sound Color FX)
 *   7. Full 4-deck transport, pad mode, pad velocity, and deck selection registers
 *   8. Jog wheel touch (0x0F/0x0B/0x00), 16-bit position, and 16-bit rotation period (0xFFFF idle, 450 turning)
 *   9. LED & jog display MOSI output structures for real-time visual feedback
 */

#ifndef XDJ_AZ_ABI64_H
#define XDJ_AZ_ABI64_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#if defined(__linux__)
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#ifndef _IO
#define _IOC_NRBITS     8
#define _IOC_TYPEBITS   8
#define _IOC_SIZEBITS   14
#define _IOC_DIRBITS    2
#define _IOC_NONE       0U
#define _IOC_WRITE      1U
#define _IOC_READ       2U
#define _IOC(dir,type,nr,size) \
    (((dir)  << 30) | \
     ((size) << 16) | \
     ((type) << 8) | \
     (nr))
#define _IO(type,nr)        _IOC(_IOC_NONE,(type),(nr),0)
#define _IOR(type,nr,size)  _IOC(_IOC_READ,(type),(nr),sizeof(size))
#define _IOW(type,nr,size)  _IOC(_IOC_WRITE,(type),(nr),sizeof(size))
#define _IOWR(type,nr,size) _IOC(_IOC_READ|_IOC_WRITE,(type),(nr),sizeof(size))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 1. SPI BUS DEVICE NODES & BUS INDICES
 * ============================================================================
 * The XDJ-AZ front panel exposes FOUR SPI sub-CPU buses to EP147:
 *   spi1.0 -> "ErP": Transport buttons, pads, pad modes, deck select for both decks
 *   spi2.0 -> "Deck 2": Right deck jog wheel (touch/pos/period) & tempo slider (Decks 2 & 4)
 *   spi3.0 -> "Mixer MC": Mixer knobs, faders, browse knob, and panel buttons
 *   spi5.0 -> "Deck 1": Left deck jog wheel (touch/pos/period) & tempo slider (Decks 1 & 3)
 *   ctrl   -> Inter-process injection & LED consumer virtual device node
 */
#define SUBUCOM_DEV_ERP         "/dev/subucom_spi1.0"
#define SUBUCOM_DEV_DECK2       "/dev/subucom_spi2.0"
#define SUBUCOM_DEV_MIXER       "/dev/subucom_spi3.0"
#define SUBUCOM_DEV_DECK1       "/dev/subucom_spi5.0"
#define SUBUCOM_DEV_CTRL        "/dev/subucom_ctrl"

/* Aliases for backwards compatibility */
#define SUBUCOM_DEV_MPNL        SUBUCOM_DEV_ERP
#define SUBUCOM_DEV_MPNL_ALT    "/dev/subucom_spi1.1"
#define SUBUCOM_DEV_MCU         SUBUCOM_DEV_MIXER

#define SUBUCOM_BUS_ERP         0
#define SUBUCOM_BUS_DECK2       1
#define SUBUCOM_BUS_MIXER       2
#define SUBUCOM_BUS_DECK1       3
#define SUBUCOM_BUS_COUNT       4

/* Aliases for legacy index names */
#define SUBUCOM_INDEX_DECK1     SUBUCOM_BUS_DECK1
#define SUBUCOM_INDEX_DECK2     SUBUCOM_BUS_DECK2
#define SUBUCOM_INDEX_MPNL      SUBUCOM_BUS_ERP
#define SUBUCOM_INDEX_MCU       SUBUCOM_BUS_MIXER
#define SUBUCOM_DEV_COUNT       SUBUCOM_BUS_COUNT

/* ============================================================================
 * 2. FRAME SIZES & TIMING CONSTANTS
 * ============================================================================
 * EP147 reads 152 bytes on every transfer (128 real subucom data bytes + 24 padding).
 * Polling rate is ~850 Hz (1176 us interval).
 */
#define SUBUCOM_FRAME_SIZE      152   /* Full buffer size read by EP147 */
#define SUBUCOM_REAL_BYTES      128   /* Active hardware payload bytes */
#define SUBUCOM_LED_FRAME_SIZE  152   /* Max MOSI payload size */
#define SUBUCOM_LED_WIRE_SIZE   (1 + SUBUCOM_LED_FRAME_SIZE + 4) /* [bus] + payload + size */
#define SUBUCOM_INTERVAL_US     1176  /* ~850 Hz */

#define SUBUCOM_PACKET_SIZE     SUBUCOM_FRAME_SIZE
#define SUBUCOM_HEADER_MAGIC    0x5AA5

/* ============================================================================
 * 3. IOCTL REQUEST CODES (AlphaTheta XDJ-AZ Native Driver ABI)
 * ============================================================================
 * Driver IOCTL magic is 'p' (0x70).
 */
#define SUBUCOM_IOCTL_MAGIC               'p'
#define IOCTL_TIMER_STATUS_READ           0x80027003u  /* _IOR('p', 3, uint16_t) */
#define IOCTL_TIMER_STATUS_WRITE          0x40027003u  /* _IOW('p', 3, uint16_t) */
#define IOCTL_BITS_PER_WORD_READ          0x80017001u  /* _IOR('p', 1, uint8_t)  */
#define IOCTL_BITS_PER_WORD_WRITE         0x40017001u  /* _IOW('p', 1, uint8_t)  */
#define IOCTL_MOSI_TRANSFER               0x40107000u  /* _IOW('p', 0, struct mosi_cmd) */
#define IOCTL_RX_BYTES_READ               0x80047004u  /* _IOR('p', 4, uint32_t) */
#define IOCTL_RX_BYTES_WRITE              0x40047004u  /* _IOW('p', 4, uint32_t) */
#define IOCTL_INTERVAL_READ               0x80047002u  /* _IOR('p', 2, uint32_t) */
#define IOCTL_INTERVAL_WRITE              0x40047002u  /* _IOW('p', 2, uint32_t) */

/* Layout of the 16-byte MOSI ioctl argument for LED and jog display writes */
#pragma pack(push, 1)
struct mosi_cmd {
    uint32_t magic;    /* 0x01000000 */
    uint32_t size;     /* byte count of data at data_ptr (<= SUBUCOM_LED_FRAME_SIZE) */
    uint64_t data_ptr; /* userspace virtual address of LED/display payload */
};
#pragma pack(pop)

/* Legacy / Rockchip SPI IOCTL definitions for backward compatibility */
#define SUBUCOM_SPI_IOC_MAGIC            'k'
#define SUBUCOM_IOC_TRANSFER             _IOWR(SUBUCOM_SPI_IOC_MAGIC, 1, struct subucom_ioc_transfer)
#define SUBUCOM_IOC_SEND_COMMAND         _IOW(SUBUCOM_SPI_IOC_MAGIC,  2, uint32_t)
#define SUBUCOM_IOC_SET_TIMER            _IOW(SUBUCOM_SPI_IOC_MAGIC,  3, uint32_t)
#define SUBUCOM_IOC_GET_TIMER_STATUS     _IOR(SUBUCOM_SPI_IOC_MAGIC,  4, uint8_t)
#define SUBUCOM_IOC_CLEAR_STATUS         _IO(SUBUCOM_SPI_IOC_MAGIC,   5)
#define SUBUCOM_IOC_SET_MAX_SPEED        _IOW(SUBUCOM_SPI_IOC_MAGIC,  6, uint32_t)
#define SUBUCOM_IOC_SET_BITS_PER_WORD    _IOW(SUBUCOM_SPI_IOC_MAGIC,  7, uint8_t)

struct subucom_ioc_transfer {
    uint64_t tx_buf;
    uint64_t rx_buf;
    uint32_t len;
    uint32_t speed_hz;
    uint16_t delay_usecs;
    uint8_t  bits_per_word;
    uint8_t  cs_change;
    uint32_t pad;
};

/* ============================================================================
 * 4. CRC-16/X-25 PROTOCOL DEFINITIONS
 * ============================================================================
 * CRC-16/X-25: Polynomial 0x8408 (reflected 0x1021), Init 0xFFFF, Final XOR 0xFFFF.
 * Different buses CRC a different prefix length:
 *   spi1 (ErP):    CRC over bytes 0..27 (28 bytes), stored LE at bytes 28-29.
 *   spi2 (Deck 2): CRC over bytes 0..37 (38 bytes), stored LE at bytes 38-39.
 *   spi3 (Mixer):  CRC over bytes 0..95 (96 bytes), stored LE at bytes 96-97.
 *   spi5 (Deck 1): CRC over bytes 0..37 (38 bytes), stored LE at bytes 38-39.
 */
#define SUBUCOM_CRC_SEED        0xFFFF
#define SUBUCOM_CRC_POLY        0x8408

#define SUBUCOM_CRC_LEN_ERP     28
#define SUBUCOM_CRC_LEN_DECK2   38
#define SUBUCOM_CRC_LEN_MIXER   96
#define SUBUCOM_CRC_LEN_DECK1   38

static inline uint16_t subucom_crc16_x25(const uint8_t *data, size_t n) {
    uint16_t crc = SUBUCOM_CRC_SEED;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? ((crc >> 1) ^ SUBUCOM_CRC_POLY) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFF;
}

/* Alias for legacy callers */
static inline uint16_t subucom_crc16(const uint8_t *data, size_t length) {
    return subucom_crc16_x25(data, length);
}

/* Helper to re-stamp the CRC on a 152-byte frame for any given bus (0..3) */
static inline void subucom_restamp_crc(uint8_t *frame, int bus) {
    static const int crc_lens[4] = {
        SUBUCOM_CRC_LEN_ERP,
        SUBUCOM_CRC_LEN_DECK2,
        SUBUCOM_CRC_LEN_MIXER,
        SUBUCOM_CRC_LEN_DECK1
    };
    if (bus < 0 || bus >= 4) return;
    int len = crc_lens[bus];
    if (len + 1 < SUBUCOM_FRAME_SIZE) {
        uint16_t crc = subucom_crc16_x25(frame, (size_t)len);
        frame[len]     = (uint8_t)(crc & 0xFF);
        frame[len + 1] = (uint8_t)((crc >> 8) & 0xFF);
    }
}

/* ============================================================================
 * 5. CAPTURED HARDWARE IDLE FRAMES (Verbatim from AlphaTheta XDJ-AZ)
 * ============================================================================
 */
static const uint8_t az_frame_spi1_erp[SUBUCOM_FRAME_SIZE] = {
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x39, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t az_frame_spi2_deck[SUBUCOM_FRAME_SIZE] = {
    0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x75, 0x02, 0x06, 0x02, 0xCD, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x26, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t az_frame_spi3_mixer[SUBUCOM_FRAME_SIZE] = {
    0x03, 0x01, 0x02, 0x01, 0x01, 0x01, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x7C, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
    0x7B, 0x80, 0x80, 0x80, 0x40, 0x7B, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80,
    0x80, 0x80, 0xC0, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x80, 0x80, 0x80,
    0x00, 0x80, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x80, 0x2A, 0x02, 0xFF,
    0x00, 0x80, 0x00, 0xC0, 0x80, 0x79, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x0A, 0x00, 0xB0, 0x04, 0xF4, 0x01, 0x00, 0x58, 0x00, 0x00,
    0x52, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t az_frame_spi5_deck[SUBUCOM_FRAME_SIZE] = {
    0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x44, 0x00, 0x00, 0x00, 0x68, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x46, 0x02, 0x05, 0x02, 0xC2, 0x00, 0x69, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x8E, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Array of default idle frames by bus id (0=spi1, 1=spi2, 2=spi3, 3=spi5) */
static const uint8_t * const az_bus_idle_frame[SUBUCOM_BUS_COUNT] = {
    az_frame_spi1_erp,
    az_frame_spi2_deck,
    az_frame_spi3_mixer,
    az_frame_spi5_deck
};

/* ============================================================================
 * 6. HARDWARE REGISTER MAP: ERP BUS (spi1.0 / Bus 0)
 * ============================================================================
 * Carries:
 *   - Both decks' Play, Cue, Pad Modes, Performance Pads, Transport buttons
 *   - Deck 2 fields are Deck 1's plus stride (+8 bytes)
 *   - Global Quantize, USB Stop buttons, Event frame marker
 */
#define ERP_DECK2_STRIDE        8

/* Play & Cue: Byte 10 (+ 8*d) */
#define ERP_OFF_PLAY_CUE        10
#define ERP_MASK_PLAY           0x02
#define ERP_MASK_CUE            0x04

/* Pad Modes: Byte 13 (+ 8*d) */
#define ERP_OFF_PADMODE         13
#define ERP_MASK_HOTCUE         0x01
#define ERP_MASK_BEATLOOP       0x02
#define ERP_MASK_SLIPLOOP       0x04
#define ERP_MASK_BEATJUMP       0x08

/* Performance Pads 1..8: Byte 14 (+ 8*d) */
#define ERP_OFF_PADS            14
#define ERP_MASK_PAD1           0x01
#define ERP_MASK_PAD2           0x02
#define ERP_MASK_PAD3           0x04
#define ERP_MASK_PAD4           0x08
#define ERP_MASK_PAD5           0x10
#define ERP_MASK_PAD6           0x20
#define ERP_MASK_PAD7           0x40
#define ERP_MASK_PAD8           0x80

/* Loop Buttons: Bytes 8, 9, 12 (+ 8*d) */
#define ERP_OFF_LOOP_IN         8
#define ERP_MASK_LOOP_IN        0x40
#define ERP_OFF_LOOP_OUT        8
#define ERP_MASK_LOOP_OUT       0x20
#define ERP_OFF_RELOOP          9
#define ERP_MASK_RELOOP         0x08
#define ERP_OFF_LOOP4           12
#define ERP_MASK_LOOP4          0x04
#define ERP_OFF_LOOP8           12
#define ERP_MASK_LOOP8          0x08

/* Cue / Loop Call & Memory: Byte 12 (+ 8*d) */
#define ERP_OFF_CALL_PREV       12
#define ERP_MASK_CALL_PREV      0x20
#define ERP_OFF_CALL_NEXT       12
#define ERP_MASK_CALL_NEXT      0x10
#define ERP_OFF_DELETE          12
#define ERP_MASK_DELETE         0x40
#define ERP_OFF_MEMORY          12
#define ERP_MASK_MEMORY         0x80

/* Beat Jump Nudge: Byte 9 (+ 8*d) */
#define ERP_OFF_BEATJUMP_PREV   9
#define ERP_MASK_BEATJUMP_PREV  0x40
#define ERP_OFF_BEATJUMP_NEXT   9
#define ERP_MASK_BEATJUMP_NEXT  0x20

/* Slip & Vinyl Modes: Bytes 8, 12 (+ 8*d) */
#define ERP_OFF_SLIP            8
#define ERP_MASK_SLIP           0x04
#define ERP_OFF_SLIP_REV        8
#define ERP_MASK_SLIP_REV       0x80
#define ERP_OFF_VINYL_SPEED     12
#define ERP_MASK_VINYL_SPEED    0x01
#define ERP_OFF_JOG_MODE        12
#define ERP_MASK_JOG_MODE       0x02

/* Sync & Tempo Range: Bytes 8, 9 (+ 8*d) */
#define ERP_OFF_BEAT_SYNC       8
#define ERP_MASK_BEAT_SYNC      0x02
#define ERP_OFF_KEY_SYNC        9
#define ERP_MASK_KEY_SYNC       0x02
#define ERP_OFF_MASTER          8
#define ERP_MASK_MASTER         0x10
#define ERP_OFF_TEMPO_RANGE     9
#define ERP_MASK_TEMPO_RANGE    0x10
#define ERP_OFF_TEMPO_RESET     9
#define ERP_MASK_TEMPO_RESET    0x04
#define ERP_OFF_MASTER_TEMPO    8
#define ERP_MASK_MASTER_TEMPO   0x08

/* Track Search & Fast Search: Byte 11 (+ 8*d) */
#define ERP_OFF_TRACK_PREV      11
#define ERP_MASK_TRACK_PREV     0x08
#define ERP_OFF_TRACK_NEXT      11
#define ERP_MASK_TRACK_NEXT     0x04
#define ERP_OFF_SEARCH_REV      11
#define ERP_MASK_SEARCH_REV     0x02
#define ERP_OFF_SEARCH_FWD      11
#define ERP_MASK_SEARCH_FWD     0x01

/* Shift Modifier: Byte 9 (+ 8*d) */
#define ERP_OFF_SHIFT           9
#define ERP_MASK_SHIFT          0x80

/* Deck Select (Primary / Secondary Deck Toggle): */
/* Left side: Deck 1 (Primary) = byte 8 bit 0x01, Deck 3 (Secondary) = byte 9 bit 0x01 */
#define ERP_OFF_DECKSEL_D1      8
#define ERP_MASK_DECKSEL_D1     0x01
#define ERP_OFF_DECKSEL_D3      9
#define ERP_MASK_DECKSEL_D3     0x01
/* Right side: Deck 2 (Primary) = byte 16 bit 0x01, Deck 4 (Secondary) = byte 17 bit 0x01 */
#define ERP_OFF_DECKSEL_D2      16
#define ERP_MASK_DECKSEL_D2     0x01
#define ERP_OFF_DECKSEL_D4      17
#define ERP_MASK_DECKSEL_D4     0x01

/* Global ErP Buttons: */
#define ERP_OFF_GLOBAL_QUANTIZE 10
#define ERP_MASK_GLOBAL_QUANTIZE 0x01

#define ERP_OFF_EVENT_MARKER    24
#define ERP_MASK_EVENT_MARKER   0x01
#define ERP_MASK_USB_STOP1      0x02
#define ERP_MASK_USB_STOP2      0x04

/* ============================================================================
 * 7. HARDWARE REGISTER MAP: DECK BUSES (spi2.0 / Bus 1 & spi5.0 / Bus 3)
 * ============================================================================
 * Carries:
 *   - Jog platter touch (0x0F turning forward, 0x0B turning back, 0x00 released)
 *   - Jog platter 16-bit position (LE)
 *   - Jog platter 16-bit rotation period (0xFFFF stopped, 450 turning median)
 *   - Pitch / Tempo slider (16-bit LE, centered at 517, range 11..1023)
 */
#define DECK_OFF_JOG_TOUCH      9
#define JOG_TOUCH_RELEASE       0x00
#define JOG_TOUCH_FWD           0x0F
#define JOG_TOUCH_BACK          0x0B

#define DECK_OFF_JOG_POS_LO     10
#define DECK_OFF_JOG_POS_HI     11

#define DECK_OFF_JOG_PERIOD_LO  12
#define DECK_OFF_JOG_PERIOD_HI  13
#define JOG_PERIOD_STOPPED      0xFFFF
#define JOG_PERIOD_TURNING      450

#define DECK_OFF_TEMPO_LO       16
#define DECK_OFF_TEMPO_HI       17
#define TEMPO_SLIDER_CENTER     517

/* ============================================================================
 * 8. HARDWARE REGISTER MAP: MIXER BUS (spi3.0 / Bus 2)
 * ============================================================================
 * Every analog mixer control is a 10-bit ADC value:
 *   - High byte (bits 9..2)
 *   - Low 2 bits (bits 1..0) packed four controls to a byte (shift: 6, 4, 2, 0)
 */

/* Shared press latch for panel buttons (0x00 pressed, 0x08 idle) */
#define MIXER_OFF_LATCH         6
#define MIXER_LATCH_IDLE        0x08
#define MIXER_LATCH_PRESSED     0x00

/* Browse rotary encoder */
#define MIXER_OFF_KNOB_CTR      34  /* 8-bit absolute position, starts at 128 (0x80) */
#define MIXER_OFF_KNOB_PUSH     31
#define MIXER_MASK_KNOB_PUSH    0x01

/* Top Screen-Tabs buttons: Byte 32 */
#define MIXER_OFF_TABS          32
#define PANEL_MASK_SOURCE       0x80
#define PANEL_MASK_BROWSE       0x40
#define PANEL_MASK_TAG_LIST     0x20
#define PANEL_MASK_PLAYLIST     0x10
#define PANEL_MASK_SEARCH       0x08
#define PANEL_MASK_MENU         0x04

/* Top Corner & LOAD 1..4 buttons: Byte 33 */
#define MIXER_OFF_CORNERS       33
#define PANEL_MASK_BACK         0x80
#define PANEL_MASK_TAG_TRACK    0x40
#define PANEL_MASK_FILTER_EDIT  0x20
#define PANEL_MASK_SHORTCUT     0x10
#define PANEL_MASK_LOAD1        0x08
#define PANEL_MASK_LOAD2        0x04
#define PANEL_MASK_LOAD3        0x02
#define PANEL_MASK_LOAD4        0x01

/* Mixer Settings (gear icon): Byte 0x18 */
#define MIXER_OFF_SETTINGS      0x18
#define PANEL_MASK_SETTINGS     0x10

/* X-PAD touch strip */
#define MIXER_OFF_XPAD_POS      0x1D  /* 7-bit position: 0..127 */
#define MIXER_OFF_XPAD_ZONE     0x1E  /* Zone bitmap (nonzero while pressed) */

/* Beat FX Select & Controls: Bytes 0x16, 0x1A..0x1B */
#define MIXER_OFF_FX_SEL        0x16  /* High nibble: Gray code 0..15 */
#define MIXER_MASK_FX_ON        0x01  /* Bit 0: FX ON/OFF */
#define MIXER_OFF_FX_TIME_LO    0x1A  /* 16-bit endless encoder counter */
#define MIXER_OFF_FX_TIME_HI    0x1B

/* Mixer Buttons: Bytes 0x14, 0x15, 0x16, 0x17, 0x18 */
#define MIXER_OFF_BTN_ROW1      0x14
#define MIXER_MASK_CUE1         0x08
#define MIXER_MASK_CUE2         0x04
#define MIXER_MASK_CUE3         0x02
#define MIXER_MASK_CUE4         0x01
#define MIXER_MASK_FX_BEAT_FWD  0x10
#define MIXER_MASK_FX_BEAT_REV  0x20
#define MIXER_MASK_FX_TAP       0x80

#define MIXER_OFF_BTN_ROW2      0x15
#define MIXER_MASK_SCFX_SPACE   0x02
#define MIXER_MASK_SCFX_DUBECHO 0x10
#define MIXER_MASK_SCFX_CRUSH   0x01
#define MIXER_MASK_SCFX_SWEEP   0x08
#define MIXER_MASK_SCFX_NOISE   0x20
#define MIXER_MASK_SCFX_FILTER  0x04
#define MIXER_MASK_MASTER_CUE   0x40
#define MIXER_MASK_LINK_CUE     0x80

#define MIXER_OFF_BTN_ROW3      0x16
#define MIXER_MASK_FX_FREQ_LOW  0x02
#define MIXER_MASK_FX_FREQ_MID  0x04
#define MIXER_MASK_FX_FREQ_HI   0x08

#define MIXER_OFF_BTN_ROW4      0x17
#define MIXER_MASK_FX_CH3       0x20
#define MIXER_MASK_FX_CH1       0x80
#define MIXER_MASK_FX_CH2       0x40
#define MIXER_MASK_FX_CH4       0x10
#define MIXER_MASK_FX_CH_MIC    0x01
#define MIXER_MASK_FX_CH_CFA    0x08
#define MIXER_MASK_FX_CH_CFB    0x04
#define MIXER_MASK_FX_CH_MST    0x02

#define MIXER_OFF_BTN_ROW5      0x18
#define MIXER_MASK_FX_QUANTIZE  0x04
#define MIXER_MASK_FX_AUTO_TAP  0x08
#define MIXER_MASK_MIC_TALKOVER 0x20
#define MIXER_MASK_MIC_ON2      0x40
#define MIXER_MASK_MIC_ON1      0x80

/* 10-Bit Analog Control Offsets (High byte, Low bits byte, Shift) */
#define MX_REG_CH1_FADER        0x0A, 0x0E, 6
#define MX_REG_CH2_FADER        0x0B, 0x0E, 4
#define MX_REG_CH3_FADER        0x0C, 0x0E, 2
#define MX_REG_CH4_FADER        0x0D, 0x0E, 0
#define MX_REG_CROSSFADER       0x0F, 0x10, 0

#define MX_REG_CH1_TRIM         0x24, 0x28, 6
#define MX_REG_CH1_HI           0x25, 0x28, 4
#define MX_REG_CH1_MID          0x26, 0x28, 2
#define MX_REG_CH1_LOW          0x27, 0x28, 0

#define MX_REG_CH2_TRIM         0x29, 0x2D, 6
#define MX_REG_CH2_HI           0x2A, 0x2D, 4
#define MX_REG_CH2_MID          0x2B, 0x2D, 2
#define MX_REG_CH2_LOW          0x2C, 0x2D, 0

#define MX_REG_CH3_TRIM         0x2E, 0x32, 6
#define MX_REG_CH3_HI           0x2F, 0x32, 4
#define MX_REG_CH3_MID          0x30, 0x32, 2
#define MX_REG_CH3_LOW          0x31, 0x32, 0

#define MX_REG_CH4_TRIM         0x33, 0x37, 6
#define MX_REG_CH4_HI           0x34, 0x37, 4
#define MX_REG_CH4_MID          0x35, 0x37, 2
#define MX_REG_CH4_LOW          0x36, 0x37, 0

#define MX_REG_MASTER_LEVEL     0x38, 0x3C, 6
#define MX_REG_MASTER_HI        0x39, 0x3C, 4
#define MX_REG_MASTER_MID       0x3A, 0x3C, 2
#define MX_REG_MASTER_LOW       0x3B, 0x3C, 0

#define MX_REG_CH1_COLOR        0x3D, 0x41, 6
#define MX_REG_CH2_COLOR        0x3E, 0x41, 4
#define MX_REG_CH3_COLOR        0x3F, 0x41, 2
#define MX_REG_CH4_COLOR        0x40, 0x41, 0

#define MX_REG_SCFX_PARAM       0x4E  /* 8-bit parameter */

#define MX_REG_MIC1_HI          0x42, 0x46, 6
#define MX_REG_MIC1_MID         0x43, 0x46, 4
#define MX_REG_MIC1_LOW         0x44, 0x46, 2
#define MX_REG_BFX_DEPTH        0x45, 0x46, 0

#define MX_REG_MIC2_HI          0x47, 0x4B, 6
#define MX_REG_MIC2_MID         0x48, 0x4B, 4
#define MX_REG_MIC2_LOW         0x49, 0x4B, 2
#define MX_REG_BOOTH_LEVEL      0x4A, 0x4B, 0

#define MX_REG_HP_MIX           0x4C, 0x50, 6
#define MX_REG_HP_LEVEL         0x4D, 0x50, 4

/* ============================================================================
 * 9. PACKING & UNPACKING HELPER FUNCTIONS
 * ============================================================================
 */
static inline void subucom_set10(uint8_t *f, size_t hi, size_t lo, uint8_t shift, uint16_t v) {
    if (!f || hi >= SUBUCOM_FRAME_SIZE || lo >= SUBUCOM_FRAME_SIZE) return;
    if (v > 1023) v = 1023;
    f[hi] = (uint8_t)(v >> 2);
    f[lo] = (uint8_t)((f[lo] & ~(0x03 << shift)) | (((v & 0x03) << shift)));
}

static inline uint16_t subucom_get10(const uint8_t *f, size_t hi, size_t lo, uint8_t shift) {
    if (!f || hi >= SUBUCOM_FRAME_SIZE || lo >= SUBUCOM_FRAME_SIZE) return 0;
    return (uint16_t)((((uint16_t)f[hi]) << 2) | ((f[lo] >> shift) & 0x03));
}

static inline void subucom_set_deck_jog(uint8_t *f, uint8_t touch, uint16_t pos, uint16_t period) {
    if (!f) return;
    f[DECK_OFF_JOG_TOUCH]     = touch;
    f[DECK_OFF_JOG_POS_LO]    = (uint8_t)(pos & 0xFF);
    f[DECK_OFF_JOG_POS_HI]    = (uint8_t)((pos >> 8) & 0xFF);
    f[DECK_OFF_JOG_PERIOD_LO] = (uint8_t)(period & 0xFF);
    f[DECK_OFF_JOG_PERIOD_HI] = (uint8_t)((period >> 8) & 0xFF);
}

static inline void subucom_set_deck_tempo(uint8_t *f, uint16_t raw_tempo) {
    if (!f) return;
    f[DECK_OFF_TEMPO_LO] = (uint8_t)(raw_tempo & 0xFF);
    f[DECK_OFF_TEMPO_HI] = (uint8_t)((raw_tempo >> 8) & 0xFF);
}

/* ============================================================================
 * 10. HIGH-LEVEL STRUCTURES FOR COMPATIBILITY WITH EXISTING CODE
 * ============================================================================
 */
#pragma pack(push, 1)
typedef struct {
    uint16_t magic;         /* 0x5AA5 */
    uint8_t  cmd;           /* Packet command / sequence */
    uint8_t  deck_id;       /* 0 = Deck 1, 1 = Deck 2, 2 = Deck 3, 3 = Deck 4 */
    
    /* Play / Cue / Transport flags */
    uint8_t  play_state;    /* bit 0: play, bit 1: pause, bit 2: cue */
    uint8_t  sync_state;    /* bit 0: sync, bit 1: master */
    uint8_t  slip_state;    /* bit 0: slip active */
    uint8_t  quantize_state;/* bit 0: quantize active */
    
    /* Jog Wheel (720 pulses per revolution) */
    int16_t  jog_delta;     /* Incremental jog movement */
    uint8_t  jog_touch;     /* 1 = Jog platter touched, 0 = released */
    uint8_t  pitch_bend;    /* Edge ring nudge */
    
    /* Pitch / Tempo Slider (14-bit resolution: 0..16383, center = 8192) */
    uint16_t tempo_slider;
    uint8_t  tempo_range;   /* 0: +/-6%, 1: +/-10%, 2: +/-16%, 3: WIDE */
    uint8_t  master_tempo;  /* Key lock active */
    
    /* Performance Pads (Pads 1-8 velocity & state) */
    uint8_t  pad_mode;      /* Hot Cue, Beat Loop, Slip Loop, Beat Jump */
    uint8_t  pad_states;    /* Bitmask of active pressed pads 1..8 */
    uint8_t  pad_velocities[8];

    /* Deck Select (toggles active deck on this hardware side) */
    uint8_t  deck_select_flags; /* bit 0: primary (Deck 1/2), bit 1: secondary (Deck 3/4) */
    
    /* Reserved bytes / Padding to exactly 126 bytes before CRC */
    uint8_t  reserved[100];
    
    /* Frame Check Sequence (CRC-16/X-25) */
    uint16_t crc16;
} subucom_deck_packet_t;

typedef struct {
    uint16_t magic;         /* 0x5AA5 */
    uint8_t  cmd;           /* Packet command */
    uint8_t  packet_id;
    
    /* Channel Faders & Crossfader (0..1023) */
    uint16_t ch1_fader;
    uint16_t ch2_fader;
    uint16_t ch3_fader;
    uint16_t ch4_fader;
    uint16_t crossfader;
    
    /* Channel 1 EQ & Trim (0..1023, center = 512) */
    uint16_t ch1_hi;
    uint16_t ch1_mid;
    uint16_t ch1_low;
    uint16_t ch1_trim;
    uint16_t ch1_color;
    
    /* Channel 2 EQ & Trim (0..1023, center = 512) */
    uint16_t ch2_hi;
    uint16_t ch2_mid;
    uint16_t ch2_low;
    uint16_t ch2_trim;
    uint16_t ch2_color;

    /* Channel 3 EQ & Trim (0..1023, center = 512) */
    uint16_t ch3_hi;
    uint16_t ch3_mid;
    uint16_t ch3_low;
    uint16_t ch3_trim;
    uint16_t ch3_color;

    /* Channel 4 EQ & Trim (0..1023, center = 512) */
    uint16_t ch4_hi;
    uint16_t ch4_mid;
    uint16_t ch4_low;
    uint16_t ch4_trim;
    uint16_t ch4_color;
    
    /* Cue buttons & routing */
    uint8_t  ch_cue_states; /* bit 0: ch1, bit 1: ch2, bit 2: ch3, bit 3: ch4, bit 4: master cue */
    uint8_t  headphone_mix; /* 0 = Cue, 255 = Master */
    uint8_t  headphone_vol;
    uint8_t  master_level;
    uint8_t  booth_level;
    
    /* Browse Rotary Encoder */
    int16_t  browse_delta;  /* Rotation steps: +1, -1 */
    uint8_t  browse_push;   /* 1 = Clicked */
    uint8_t  browse_back;   /* 1 = Back button pressed */
    uint8_t  browse_view;   /* 1 = View / Tag List button */
    uint8_t  load_deck1;    /* 1 = Load Deck 1 */
    uint8_t  load_deck2;    /* 1 = Load Deck 2 */
    uint8_t  load_deck3;    /* 1 = Load Deck 3 */
    uint8_t  load_deck4;    /* 1 = Load Deck 4 */

    /* Sound Color FX & Beat FX */
    uint8_t  cfx_type;      /* 0: Filter, 1: Dub Echo, 2: Noise, 3: Space */
    uint8_t  bfx_ch;        /* 0: Ch1, 1: Ch2, 2: Ch3, 3: Ch4, 4: Master */
    uint8_t  bfx_slot;      /* 0..5 slot index */
    uint8_t  bfx_on;        /* 1 = On, 0 = Off */
    uint8_t  bfx_depth;     /* 0..127 Level/Depth */
    uint16_t bfx_time_ms;   /* FX Time in milliseconds */
    
    /* Padding to 126 bytes */
    uint8_t  reserved[50];
    
    /* CRC16 */
    uint16_t crc16;
} subucom_mixer_packet_t;
#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* XDJ_AZ_ABI64_H */
