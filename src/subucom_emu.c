/*
 * subucom_emu.c — Virtual /dev/subucom_spi* & /dev/subucom_ctrl LD_PRELOAD Driver Shim
 * Target: Amlogic S905X (ARM64 AArch64) & Linux Embedded Targets
 *
 * Ground Truth Reference:
 *   - AlphaTheta XDJ-AZ EP147 Native Binary
 *   - Kyle-Hosman/xdj-az-emu subucom_virt / subucom_frames.h hardware captures
 *
 * Implements:
 *   1. Interception of /dev/subucom_spi{1,2,3,5}.0 and /dev/subucom_ctrl
 *   2. Native IOCTLs with magic 'p' (0x70) & 16-byte MOSI command structures
 *   3. 152-byte MISO & MOSI frame buffers seeded from authentic hardware captures
 *   4. Authentic per-bus CRC-16/X-25 re-stamping (spi1=28, spi2=38, spi3=96, spi5=38)
 *   5. External control ingress via /dev/subucom_ctrl, UNIX socket (/tmp/subucom_ctrl.sock),
 *      and UDP socket (127.0.0.1:9999)
 *   6. Hardware LED & Jog display frame capture via IOCTL_MOSI_TRANSFER (0x40107000)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#pragma GCC diagnostic ignored "-Wnonnull-compare"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "xdj_az_abi64.h"
#include "xdj_az_shm.h"

#define LOG_TAG             "[subucom_emu] "
#define SUBUCOM_SOCKET_PATH "/tmp/subucom_ctrl.sock"
#define SUBUCOM_UDP_PORT    9999

static xdj_az_shared_t *g_shm = NULL;
static int g_shm_fd = -1;
static bool g_is_ep147 = false;
static uint64_t s_boot_ms = 0;
static int s_probe_pipe[2] = {-1, -1};

static uint64_t get_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* Crash-proof user memory validator using non-blocking pipe copy_from_user */
static inline bool is_safe_ptr(uintptr_t p, size_t sz) {
    if (p < 0x400000 || (p & 7) != 0) return false;
    if (s_probe_pipe[0] < 0) {
        if (pipe2(s_probe_pipe, O_NONBLOCK | O_CLOEXEC) < 0) return false;
    }
    ssize_t ret = write(s_probe_pipe[1], (const void*)p, sz);
    if (ret == (ssize_t)sz) {
        char dummy[64];
        while (read(s_probe_pipe[0], dummy, sizeof(dummy)) > 0) {}
        return true;
    }
    return false;
}

/* Direct telemetry reader for EP147 internal playback engine */
static void subucom_poll_ep147_playheads(void) {
    if (!g_shm || !g_is_ep147) return;

    /* Allow EP147 5000 ms to boot up and initialize GUI/player objects */
    if (s_boot_ms == 0) s_boot_ms = get_now_ms();
    if (get_now_ms() - s_boot_ms < 5000) return;

    /* Cached gui::player_info::LocalPlayer object pointers for decks 0..3 */
    static uintptr_t s_players[4] = {0, 0, 0, 0};
    static bool s_players_found = false;

    if (!s_players_found) {
        /* Fixed address of JUCE TimerThread registry singleton in EP147 .bss */
        uintptr_t reg_addr = 0x03bd7830;
        if (!is_safe_ptr(reg_addr, sizeof(uintptr_t))) return;

        uintptr_t registry = *(volatile uintptr_t *)reg_addr;
        if (!is_safe_ptr(registry + 0x1a0, 16)) return;

        uintptr_t lo = *(volatile uintptr_t *)(registry + 0x1a0);
        uintptr_t hi = *(volatile uintptr_t *)(registry + 0x1a8);
        if (lo < 0x400000 || hi <= lo || (hi - lo) > (256 * sizeof(uintptr_t))) return;
        if (!is_safe_ptr(lo, sizeof(uintptr_t))) return;

        size_t count = (hi - lo) / sizeof(uintptr_t);
        int found_count = 0;
        for (size_t i = 0; i < count; i++) {
            uintptr_t entry_addr = lo + i * sizeof(uintptr_t);
            if (!is_safe_ptr(entry_addr, sizeof(uintptr_t))) break;

            uintptr_t entry = *(volatile uintptr_t *)entry_addr;
            if (!is_safe_ptr(entry, 0x40)) continue;

            uintptr_t vptr = *(volatile uintptr_t *)entry;
            if (vptr == 0x2da1c28) {
                uint32_t deck_idx = *(volatile uint32_t *)(entry + 0x38);
                if (deck_idx < 4) {
                    s_players[deck_idx] = entry;
                    found_count++;
                }
            }
        }
        if (found_count >= 4) {
            s_players_found = true;
            fprintf(stderr, LOG_TAG "Discovered all 4 LocalPlayer instances in EP147 memory\n");
        }
    }

    if (!s_players_found) return;

    /* Read live playhead telemetry directly from dj_player::PlayerStateWithUpdate */
    for (int d = 0; d < 4; d++) {
        uintptr_t player = s_players[d];
        if (!player || !is_safe_ptr(player, 0xc0)) {
            s_players_found = false;
            break;
        }

        /* Verify LocalPlayer vtable integrity */
        if (*(volatile uintptr_t *)player != 0x2da1c28) {
            s_players_found = false; /* Invalidate cache if player was recreated */
            break;
        }

        uintptr_t src = *(volatile uintptr_t *)(player + 0xb0);
        if (!is_safe_ptr(src, 0xf0)) {
            g_shm->track_loaded[d] = 0;
            continue;
        }

        /* Verify dj_player::PlayerStateWithUpdate vtable 0x25eb390 */
        if (*(volatile uintptr_t *)src == 0x25eb390) {
            int64_t pos = *(volatile int64_t *)(src + 0xe8);
            uint32_t is_loaded = *(volatile uint32_t *)(src + 0x40);

            /* Deck has a track loaded when sentinel != INT64_MAX and is_loaded != 0.
             * Playhead position can be negative during pre-gap / lead-in before 0:00. */
            if (pos != 0x7FFFFFFFFFFFFFFFULL && (is_loaded != 0 || (pos > -15876000000LL && pos < 15876000000LL))) {
                g_shm->playhead_pos[d] = pos;
                g_shm->track_loaded[d] = 1;
            } else {
                g_shm->playhead_pos[d] = 0;
                g_shm->track_loaded[d] = 0;
            }
            continue;
        }
        g_shm->track_loaded[d] = 0;
    }
}

static void update_shm_mosi(uint8_t bus, const void *data, uint32_t sz) {
    if (!g_shm) {
        g_shm_fd = shm_open(XDJ_AZ_SHM_FILE, O_CREAT | O_RDWR, 0666);
        if (g_shm_fd < 0) {
            g_shm_fd = open("/dev/shm/xdj_az_shared", O_CREAT | O_RDWR, 0666);
        }
        if (g_shm_fd >= 0) {
            ftruncate(g_shm_fd, sizeof(xdj_az_shared_t));
            void *p = mmap(NULL, sizeof(xdj_az_shared_t), PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
            if (p != MAP_FAILED) {
                g_shm = (xdj_az_shared_t*)p;
                if (g_shm->magic != XDJ_AZ_SHM_MAGIC) {
                    memset(g_shm, 0, sizeof(xdj_az_shared_t));
                    g_shm->magic = XDJ_AZ_SHM_MAGIC;
                    g_shm->version = 1;
                }
            }
        }
    }
    if (g_shm && bus < 4 && data && sz > 0) {
        uint32_t cp = (sz > 152) ? 152 : sz;
        g_shm->mosi_size[bus] = cp;
        memcpy(g_shm->mosi_buf[bus], data, cp);
        g_shm->mosi_count[bus]++;

        /* Authentic Pioneer ErP MOSI Telemetry: Master, Beat Sync, Looping */
        if (bus == SUBUCOM_BUS_ERP && cp >= 0x70) {
            const uint8_t *erp = (const uint8_t *)data;
            /* Side 0: Left Platter (Deck 1 / 3) */
            uint8_t master_lvl0 = (erp[0x08] >> 4) & 3;
            uint8_t sync_lvl0   = (erp[0x09] >> 6) & 3;
            uint8_t out_lvl0    = (erp[0x0A] >> 4) & 3;

            /* Side 1: Right Platter (Deck 2 / 4) */
            uint8_t master_lvl1 = (erp[0x38 + 0x08] >> 4) & 3;
            uint8_t sync_lvl1   = (erp[0x38 + 0x09] >> 6) & 3;
            uint8_t out_lvl1    = (erp[0x38 + 0x0A] >> 4) & 3;

            g_shm->is_master[0] = (master_lvl0 == 2) ? 1 : 0;
            g_shm->is_sync[0]   = (sync_lvl0 == 2) ? 1 : 0;
            g_shm->is_looping[0] = (out_lvl0 == 2) ? 1 : 0;

            g_shm->is_master[1] = (master_lvl1 == 2) ? 1 : 0;
            g_shm->is_sync[1]   = (sync_lvl1 == 2) ? 1 : 0;
            g_shm->is_looping[1] = (out_lvl1 == 2) ? 1 : 0;
        }

        g_shm->seq++;
    }
}

#define CTRL_PSEUDO_BUS_IDX 4

typedef struct {
    const char     *path;
    const char     *name;
    int             bus_id;          /* 0=spi1 ErP, 1=spi2 Deck2, 2=spi3 Mixer, 3=spi5 Deck1 */
    int             efd;             /* eventfd for poll() notifications */
    bool            is_open;
    uint16_t        timer_status;
    uint32_t        interval_us;
    uint32_t        rx_bytes;
    uint8_t         bits_per_word;
    
    pthread_mutex_t lock;
    uint8_t         frame[SUBUCOM_FRAME_SIZE];
    uint8_t         overlay[SUBUCOM_FRAME_SIZE];
    bool            has_overlay;
    
    uint8_t         latest_mosi[SUBUCOM_LED_FRAME_SIZE];
    uint32_t        mosi_size;
    bool            mosi_fresh;
} virt_spi_chan_t;

/* Channel table for the 4 hardware SPI buses */
static virt_spi_chan_t g_channels[SUBUCOM_BUS_COUNT] = {
    { SUBUCOM_DEV_ERP,   "ErP (spi1)",   SUBUCOM_BUS_ERP,   -1, false, 0, SUBUCOM_INTERVAL_US, SUBUCOM_FRAME_SIZE, 8, PTHREAD_MUTEX_INITIALIZER, {0}, {0}, false, {0}, 0, false },
    { SUBUCOM_DEV_DECK2, "Deck2 (spi2)", SUBUCOM_BUS_DECK2, -1, false, 0, SUBUCOM_INTERVAL_US, SUBUCOM_FRAME_SIZE, 8, PTHREAD_MUTEX_INITIALIZER, {0}, {0}, false, {0}, 0, false },
    { SUBUCOM_DEV_MIXER, "Mixer (spi3)", SUBUCOM_BUS_MIXER, -1, false, 0, SUBUCOM_INTERVAL_US, SUBUCOM_FRAME_SIZE, 8, PTHREAD_MUTEX_INITIALIZER, {0}, {0}, false, {0}, 0, false },
    { SUBUCOM_DEV_DECK1, "Deck1 (spi5)", SUBUCOM_BUS_DECK1, -1, false, 0, SUBUCOM_INTERVAL_US, SUBUCOM_FRAME_SIZE, 8, PTHREAD_MUTEX_INITIALIZER, {0}, {0}, false, {0}, 0, false }
};

/* Real libc function pointers */
static int (*real_open)(const char *pathname, int flags, ...) = NULL;
static int (*real_open64)(const char *pathname, int flags, ...) = NULL;
static int (*real_close)(int fd) = NULL;
static ssize_t (*real_read)(int fd, void *buf, size_t count) = NULL;
static ssize_t (*real_write)(int fd, const void *buf, size_t count) = NULL;
static int (*real_ioctl)(int fd, unsigned long request, ...) = NULL;
static int (*real_poll)(struct pollfd *fds, nfds_t nfds, int timeout) = NULL;
static int (*real_ppoll)(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo_p, const sigset_t *sigmask) = NULL;

static pthread_t g_server_thread;
static volatile bool g_running = true;
static bool g_thread_started = false;

/* Mapping from file descriptor to channel index (0..3), or 4 for CTRL node */
static int g_fd_chan_map[1024];
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;

static void init_real_functions(void) {
    if (!real_open) {
        real_open   = dlsym(RTLD_NEXT, "open");
        real_open64 = dlsym(RTLD_NEXT, "open64");
        real_close  = dlsym(RTLD_NEXT, "close");
        real_read   = dlsym(RTLD_NEXT, "read");
        real_write  = dlsym(RTLD_NEXT, "write");
        real_ioctl  = dlsym(RTLD_NEXT, "ioctl");
        real_poll   = dlsym(RTLD_NEXT, "poll");
        real_ppoll  = dlsym(RTLD_NEXT, "ppoll");
    }
}

static void init_chan_map(void) {
    static bool inited = false;
    if (!inited) {
        pthread_mutex_lock(&g_map_lock);
        if (!inited) {
            for (int i = 0; i < 1024; i++) g_fd_chan_map[i] = -1;
            inited = true;
        }
        pthread_mutex_unlock(&g_map_lock);
    }
}

static inline virt_spi_chan_t* get_chan_by_fd(int fd) {
    if (fd < 0 || fd >= 1024) return NULL;
    int idx = g_fd_chan_map[fd];
    if (idx < 0 || idx >= SUBUCOM_BUS_COUNT) return NULL;
    return &g_channels[idx];
}

static inline bool is_ctrl_fd(int fd) {
    if (fd < 0 || fd >= 1024) return false;
    return (g_fd_chan_map[fd] == CTRL_PSEUDO_BUS_IDX);
}

static int get_chan_index_by_path(const char *path) {
    if (!path) return -1;
    for (int i = 0; i < SUBUCOM_BUS_COUNT; i++) {
        if (strcmp(path, g_channels[i].path) == 0) return i;
    }
    if (strcmp(path, SUBUCOM_DEV_MPNL_ALT) == 0) return SUBUCOM_BUS_ERP;
    if (strcmp(path, SUBUCOM_DEV_CTRL) == 0)     return CTRL_PSEUDO_BUS_IDX;
    return -1;
}

static void notify_channel(virt_spi_chan_t *chan) {
    if (chan && chan->efd >= 0) {
        uint64_t val = 1;
        real_write(chan->efd, &val, sizeof(val));
    }
}

static bool is_target_process(void) {
    char exe[256] = {0};
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = '\0';
        if (strstr(exe, "EP147")) return true;
    }
    return false;
}

/* Apply overlay bytes to a channel frame and re-stamp CRC-16/X-25 */
static void apply_bus_overlay(int bus_idx, const uint8_t *payload, size_t len) {
    if (bus_idx < 0 || bus_idx >= SUBUCOM_BUS_COUNT || !payload) return;
    virt_spi_chan_t *chan = &g_channels[bus_idx];
    
    pthread_mutex_lock(&chan->lock);
    size_t cp = (len > SUBUCOM_FRAME_SIZE) ? SUBUCOM_FRAME_SIZE : len;
    memcpy(chan->overlay, payload, cp);
    chan->has_overlay = true;
    
    /* Copy over active frame and re-stamp CRC */
    memcpy(chan->frame, chan->overlay, SUBUCOM_FRAME_SIZE);
    subucom_restamp_crc(chan->frame, bus_idx);
    pthread_mutex_unlock(&chan->lock);
    
    notify_channel(chan);
}

static void clear_bus_overlay(int bus_idx) {
    if (bus_idx < 0 || bus_idx >= SUBUCOM_BUS_COUNT) {
        for (int i = 0; i < SUBUCOM_BUS_COUNT; i++) clear_bus_overlay(i);
        return;
    }
    virt_spi_chan_t *chan = &g_channels[bus_idx];
    pthread_mutex_lock(&chan->lock);
    chan->has_overlay = false;
    memcpy(chan->frame, az_bus_idle_frame[bus_idx], SUBUCOM_FRAME_SIZE);
    subucom_restamp_crc(chan->frame, bus_idx);
    pthread_mutex_unlock(&chan->lock);
    
    notify_channel(chan);
}

/* ============================================================================
 * Exported Beat FX Hardware Control API
 * ============================================================================
 */
void subucom_set_bfx_type(int type) {
    if (type < 0 || type > 15) return;
    virt_spi_chan_t *chan = &g_channels[SUBUCOM_BUS_MIXER];
    pthread_mutex_lock(&chan->lock);
    uint8_t nibble = ((uint8_t)type & 0x0F) << 4;
    chan->frame[MIXER_OFF_FX_SEL] = (chan->frame[MIXER_OFF_FX_SEL] & 0x0F) | nibble;
    chan->frame[85] = (chan->frame[85] & 0x0F) | nibble;
    /* Ensure X-PAD pointer is cleanly suppressed upon FX change */
    chan->frame[MIXER_OFF_XPAD_POS] = 0xFF;
    chan->frame[MIXER_OFF_XPAD_ZONE] = 0x00;
    chan->frame[87] = 0x00;

    /* Parameter calibration per FX kind */
    if (type == 5) {
        /* REVERB: Parameter is LENGTH % (1..100%, default 50%) */
        chan->frame[85] = (chan->frame[85] & 0xF0); /* No beat slot */
        chan->frame[90] = 50;
        chan->frame[91] = 0;
    } else if (type == 8 || type == 9) {
        /* FILTER / TRIPLET FILTER: 1000 ms */
        chan->frame[85] = (chan->frame[85] & 0xF0);
        chan->frame[90] = (uint8_t)(1000 & 0xFF);
        chan->frame[91] = (uint8_t)((1000 >> 8) & 0xFF);
    } else {
        uint8_t cur_preset = chan->frame[86];
        uint16_t time_ms = 500;
        switch (cur_preset) {
            case 1:
            case 2:  time_ms = 31;   break;
            case 3:
            case 4:  time_ms = 62;   break;
            case 5:  time_ms = 125;  break;
            case 6:  time_ms = 166;  break;
            case 7:  time_ms = 250;  break;
            case 8:  time_ms = 333;  break;
            case 9:  time_ms = 375;  break;
            case 10:
            case 11: time_ms = 500;  break;
            case 12:
            case 13: time_ms = 1000; break;
            case 14:
            case 15: time_ms = 2000; break;
            default: time_ms = 500;  break;
        }
        chan->frame[90] = (uint8_t)(time_ms & 0xFF);
        chan->frame[91] = (uint8_t)((time_ms >> 8) & 0xFF);
    }

    subucom_restamp_crc(chan->frame, SUBUCOM_BUS_MIXER);
    pthread_mutex_unlock(&chan->lock);
    notify_channel(chan);
}

void subucom_toggle_bfx_power(void) {
    virt_spi_chan_t *chan = &g_channels[SUBUCOM_BUS_MIXER];
    pthread_mutex_lock(&chan->lock);
    chan->frame[MIXER_OFF_FX_SEL] ^= MIXER_MASK_FX_ON;
    subucom_restamp_crc(chan->frame, SUBUCOM_BUS_MIXER);
    pthread_mutex_unlock(&chan->lock);
    notify_channel(chan);
}

void subucom_toggle_bfx_quantize(void) {
    virt_spi_chan_t *chan = &g_channels[SUBUCOM_BUS_MIXER];
    pthread_mutex_lock(&chan->lock);
    chan->frame[84] ^= 0x40;
    chan->frame[MIXER_OFF_BTN_ROW5] |= MIXER_MASK_FX_QUANTIZE;
    chan->frame[MIXER_OFF_LATCH] = MIXER_LATCH_PRESSED;
    subucom_restamp_crc(chan->frame, SUBUCOM_BUS_MIXER);
    pthread_mutex_unlock(&chan->lock);
    notify_channel(chan);

    usleep(40000);

    pthread_mutex_lock(&chan->lock);
    chan->frame[MIXER_OFF_BTN_ROW5] &= ~MIXER_MASK_FX_QUANTIZE;
    chan->frame[MIXER_OFF_LATCH] = MIXER_LATCH_IDLE;
    subucom_restamp_crc(chan->frame, SUBUCOM_BUS_MIXER);
    pthread_mutex_unlock(&chan->lock);
    notify_channel(chan);
}

void subucom_set_bfx_fraction(int preset) {
    virt_spi_chan_t *chan = &g_channels[SUBUCOM_BUS_MIXER];
    pthread_mutex_lock(&chan->lock);
    chan->frame[86] = (uint8_t)preset;
    int cur_fx = (chan->frame[MIXER_OFF_FX_SEL] >> 4) & 0x0F;
    bool is_beat = (cur_fx == 0 || cur_fx == 1 || cur_fx == 2 || cur_fx == 10 || cur_fx == 11 || cur_fx == 12);

    uint16_t time_ms = 500;
    uint8_t xpad_slot = 0;
    switch (preset) {
        case 1:
        case 2:  time_ms = 31;   xpad_slot = 1; break; /* 1/16 */
        case 3:
        case 4:  time_ms = 62;   xpad_slot = 2; break; /* 1/8  */
        case 5:  time_ms = 125;  xpad_slot = 3; break; /* 1/4  */
        case 6:  time_ms = 166;  xpad_slot = (cur_fx == 12) ? 3 : 0; break; /* 1/3 */
        case 7:  time_ms = 250;  xpad_slot = 4; break; /* 1/2  */
        case 8:  time_ms = 333;  xpad_slot = (cur_fx == 12) ? 5 : 0; break; /* 2/3 */
        case 9:  time_ms = 375;  xpad_slot = (cur_fx == 12) ? 0 : 5; break; /* 3/4 */
        case 10:
        case 11: time_ms = 500;  xpad_slot = 6; break; /* 1    */
        case 12:
        case 13: time_ms = 1000; xpad_slot = 0; break; /* 2    */
        case 14:
        case 15: time_ms = 2000; xpad_slot = 0; break; /* 4    */
        case 16: time_ms = 4000; xpad_slot = 0; break; /* 8    */
        case 17: time_ms = 8000; xpad_slot = 0; break; /* 16   */
        default: time_ms = 500;  xpad_slot = 6; break;
    }
    if (is_beat) {
        chan->frame[85] = (chan->frame[85] & 0xF0) | (xpad_slot & 0x0F);
    } else {
        chan->frame[85] = (chan->frame[85] & 0xF0); /* Continuous: do not highlight beat slot */
    }
    chan->frame[90] = (uint8_t)(time_ms & 0xFF);
    chan->frame[91] = (uint8_t)((time_ms >> 8) & 0xFF);
    subucom_restamp_crc(chan->frame, SUBUCOM_BUS_MIXER);
    pthread_mutex_unlock(&chan->lock);
    notify_channel(chan);
}

void subucom_set_xpad(int region, int val, int time_ms) {
    virt_spi_chan_t *chan = &g_channels[SUBUCOM_BUS_MIXER];
    pthread_mutex_lock(&chan->lock);
    int cur_fx = (chan->frame[MIXER_OFF_FX_SEL] >> 4) & 0x0F;
    bool is_beat = (cur_fx == 0 || cur_fx == 1 || cur_fx == 2 || cur_fx == 10 || cur_fx == 11 || cur_fx == 12);

    if (region > 0) {
        if (is_beat) {
            chan->frame[85] = (chan->frame[85] & 0xF0) | (region & 0x0F);
        } else {
            chan->frame[85] = (chan->frame[85] & 0xF0); /* Continuous: no slot highlight */
        }
        chan->frame[87] = (uint8_t)val;
        chan->frame[MIXER_OFF_XPAD_POS] = (uint8_t)val;
        chan->frame[MIXER_OFF_XPAD_ZONE] = (1 << (region - 1));
        if (time_ms > 0) {
            chan->frame[90] = (uint8_t)(time_ms & 0xFF);
            chan->frame[91] = (uint8_t)((time_ms >> 8) & 0xFF);
        }
    } else {
        /* Release: hide pointer line completely and restore active BEAT fraction parameter */
        chan->frame[MIXER_OFF_XPAD_POS] = 0xFF;  /* Hardware idle sentinel */
        chan->frame[MIXER_OFF_XPAD_ZONE] = 0x00;
        chan->frame[87] = 0x00;

        if (cur_fx == 5) {
            /* REVERB: restore 50% length and no beat slot */
            chan->frame[85] = (chan->frame[85] & 0xF0);
            chan->frame[90] = 50;
            chan->frame[91] = 0;
        } else if (cur_fx == 8 || cur_fx == 9) {
            /* FILTER / TRIPLET FILTER: restore 1000ms */
            chan->frame[85] = (chan->frame[85] & 0xF0);
            chan->frame[90] = (uint8_t)(1000 & 0xFF);
            chan->frame[91] = (uint8_t)((1000 >> 8) & 0xFF);
        } else {
            uint8_t cur_preset = chan->frame[86];
            uint16_t def_time = 500;
            uint8_t xpad_slot = 6;
            switch (cur_preset) {
                case 1:
                case 2:  def_time = 31;   xpad_slot = 1; break; /* 1/16 */
                case 3:
                case 4:  def_time = 62;   xpad_slot = 2; break; /* 1/8  */
                case 5:  def_time = 125;  xpad_slot = 3; break; /* 1/4  */
                case 6:  def_time = 166;  xpad_slot = (cur_fx == 12) ? 3 : 0; break; /* 1/3 */
                case 7:  def_time = 250;  xpad_slot = 4; break; /* 1/2  */
                case 8:  def_time = 333;  xpad_slot = (cur_fx == 12) ? 5 : 0; break; /* 2/3 */
                case 9:  def_time = 375;  xpad_slot = (cur_fx == 12) ? 0 : 5; break; /* 3/4 */
                case 10:
                case 11: def_time = 500;  xpad_slot = 6; break; /* 1    */
                case 12:
                case 13: def_time = 1000; xpad_slot = 0; break; /* 2    */
                case 14:
                case 15: def_time = 2000; xpad_slot = 0; break; /* 4    */
                case 16: def_time = 4000; xpad_slot = 0; break; /* 8    */
                case 17: def_time = 8000; xpad_slot = 0; break; /* 16   */
                default: def_time = 500;  xpad_slot = 6; break;
            }
            if (is_beat) {
                chan->frame[85] = (chan->frame[85] & 0xF0) | (xpad_slot & 0x0F);
            } else {
                chan->frame[85] = (chan->frame[85] & 0xF0); /* Continuous: do not highlight beat slots */
            }
            chan->frame[90] = (uint8_t)(def_time & 0xFF);
            chan->frame[91] = (uint8_t)((def_time >> 8) & 0xFF);
        }
    }
    subucom_restamp_crc(chan->frame, SUBUCOM_BUS_MIXER);
    pthread_mutex_unlock(&chan->lock);
    notify_channel(chan);
}

void subucom_set_bfx_channel(int ch) {
    virt_spi_chan_t *chan = &g_channels[SUBUCOM_BUS_MIXER];
    pthread_mutex_lock(&chan->lock);
    chan->frame[MIXER_OFF_BTN_ROW4] &= ~(MIXER_MASK_FX_CH1 | MIXER_MASK_FX_CH2 |
                                          MIXER_MASK_FX_CH3 | MIXER_MASK_FX_CH4 |
                                          MIXER_MASK_FX_CH_MST | MIXER_MASK_FX_CH_MIC);
    if (ch == 0) chan->frame[MIXER_OFF_BTN_ROW4] |= MIXER_MASK_FX_CH1;
    else if (ch == 1) chan->frame[MIXER_OFF_BTN_ROW4] |= MIXER_MASK_FX_CH2;
    else if (ch == 2) chan->frame[MIXER_OFF_BTN_ROW4] |= MIXER_MASK_FX_CH3;
    else if (ch == 3) chan->frame[MIXER_OFF_BTN_ROW4] |= MIXER_MASK_FX_CH4;
    else chan->frame[MIXER_OFF_BTN_ROW4] |= MIXER_MASK_FX_CH_MST;
    subucom_restamp_crc(chan->frame, SUBUCOM_BUS_MIXER);
    pthread_mutex_unlock(&chan->lock);
    notify_channel(chan);
}

int subucom_get_bfx_type(void) {
    virt_spi_chan_t *chan = &g_channels[SUBUCOM_BUS_MIXER];
    pthread_mutex_lock(&chan->lock);
    int type = (chan->frame[MIXER_OFF_FX_SEL] >> 4) & 0x0F;
    pthread_mutex_unlock(&chan->lock);
    return type;
}

int subucom_get_bfx_channel(void) {
    virt_spi_chan_t *chan = &g_channels[SUBUCOM_BUS_MIXER];
    pthread_mutex_lock(&chan->lock);
    uint8_t row4 = chan->frame[MIXER_OFF_BTN_ROW4];
    pthread_mutex_unlock(&chan->lock);
    if (row4 & MIXER_MASK_FX_CH1) return 0;
    if (row4 & MIXER_MASK_FX_CH2) return 1;
    if (row4 & MIXER_MASK_FX_CH3) return 2;
    if (row4 & MIXER_MASK_FX_CH4) return 3;
    return 4; /* MASTER */
}

int subucom_get_bfx_power(void) {
    virt_spi_chan_t *chan = &g_channels[SUBUCOM_BUS_MIXER];
    pthread_mutex_lock(&chan->lock);
    int on = (chan->frame[MIXER_OFF_FX_SEL] & MIXER_MASK_FX_ON) ? 1 : 0;
    pthread_mutex_unlock(&chan->lock);
    return on;
}

void subucom_step_beat(int dir) {
    virt_spi_chan_t *chan = &g_channels[SUBUCOM_BUS_MIXER];
    pthread_mutex_lock(&chan->lock);
    if (dir > 0) {
        chan->frame[MIXER_OFF_BTN_ROW1] |= MIXER_MASK_FX_BEAT_FWD;
    } else {
        chan->frame[MIXER_OFF_BTN_ROW1] |= MIXER_MASK_FX_BEAT_REV;
    }
    chan->frame[MIXER_OFF_LATCH] = MIXER_LATCH_PRESSED;
    subucom_restamp_crc(chan->frame, SUBUCOM_BUS_MIXER);
    pthread_mutex_unlock(&chan->lock);
    notify_channel(chan);

    usleep(40000); /* 40ms pulse */

    pthread_mutex_lock(&chan->lock);
    chan->frame[MIXER_OFF_BTN_ROW1] &= ~(MIXER_MASK_FX_BEAT_FWD | MIXER_MASK_FX_BEAT_REV);
    chan->frame[MIXER_OFF_LATCH] = MIXER_LATCH_IDLE;
    subucom_restamp_crc(chan->frame, SUBUCOM_BUS_MIXER);
    pthread_mutex_unlock(&chan->lock);
    notify_channel(chan);
}

/* Translates high-level subucom_deck_packet_t into low-level 152B hardware frames */
static void translate_deck_packet(const subucom_deck_packet_t *pkt) {
    if (!pkt) return;
    int deck_id = pkt->deck_id; /* 0..3 */
    int side = (deck_id == 0 || deck_id == 2) ? 0 : 1; /* 0=Left, 1=Right */
    int bus_deck = (side == 0) ? SUBUCOM_BUS_DECK1 : SUBUCOM_BUS_DECK2;
    
    /* 1. Update ErP transport buttons (Bus 0) */
    virt_spi_chan_t *erp = &g_channels[SUBUCOM_BUS_ERP];
    pthread_mutex_lock(&erp->lock);
    int d_off = side * ERP_DECK2_STRIDE;
    
    /* Play & Cue */
    erp->frame[ERP_OFF_PLAY_CUE + d_off] &= ~(ERP_MASK_PLAY | ERP_MASK_CUE);
    if (pkt->play_state & 0x01) erp->frame[ERP_OFF_PLAY_CUE + d_off] |= ERP_MASK_PLAY;
    if (pkt->play_state & 0x04) erp->frame[ERP_OFF_PLAY_CUE + d_off] |= ERP_MASK_CUE;
    
    /* Sync & Master */
    erp->frame[ERP_OFF_BEAT_SYNC + d_off] &= ~(ERP_MASK_BEAT_SYNC | ERP_MASK_MASTER);
    if (pkt->sync_state & 0x01) erp->frame[ERP_OFF_BEAT_SYNC + d_off] |= ERP_MASK_BEAT_SYNC;
    if (pkt->sync_state & 0x02) erp->frame[ERP_OFF_BEAT_SYNC + d_off] |= ERP_MASK_MASTER;
    
    /* Slip */
    erp->frame[ERP_OFF_SLIP + d_off] &= ~ERP_MASK_SLIP;
    if (pkt->slip_state & 0x01) erp->frame[ERP_OFF_SLIP + d_off] |= ERP_MASK_SLIP;
    
    /* Pad Mode */
    erp->frame[ERP_OFF_PADMODE + d_off] = (1 << (pkt->pad_mode & 0x03));
    
    /* Performance Pads 1..8 */
    erp->frame[ERP_OFF_PADS + d_off] = pkt->pad_states;
    
    /* Deck Select */
    if (side == 0) {
        erp->frame[ERP_OFF_DECKSEL_D1] &= ~ERP_MASK_DECKSEL_D1;
        erp->frame[ERP_OFF_DECKSEL_D3] &= ~ERP_MASK_DECKSEL_D3;
        if (deck_id == 2) erp->frame[ERP_OFF_DECKSEL_D3] |= ERP_MASK_DECKSEL_D3;
        else              erp->frame[ERP_OFF_DECKSEL_D1] |= ERP_MASK_DECKSEL_D1;
    } else {
        erp->frame[ERP_OFF_DECKSEL_D2] &= ~ERP_MASK_DECKSEL_D2;
        erp->frame[ERP_OFF_DECKSEL_D4] &= ~ERP_MASK_DECKSEL_D4;
        if (deck_id == 3) erp->frame[ERP_OFF_DECKSEL_D4] |= ERP_MASK_DECKSEL_D4;
        else              erp->frame[ERP_OFF_DECKSEL_D2] |= ERP_MASK_DECKSEL_D2;
    }
    
    /* Event marker */
    erp->frame[ERP_OFF_EVENT_MARKER] = ERP_MASK_EVENT_MARKER;
    subucom_restamp_crc(erp->frame, SUBUCOM_BUS_ERP);
    pthread_mutex_unlock(&erp->lock);
    notify_channel(erp);

    /* 2. Update Deck Jog & Tempo (Bus 1 or 3) */
    virt_spi_chan_t *deck = &g_channels[bus_deck];
    pthread_mutex_lock(&deck->lock);
    
    uint8_t touch = JOG_TOUCH_RELEASE;
    uint16_t period = JOG_PERIOD_STOPPED;
    if (pkt->jog_touch) {
        touch = (pkt->jog_delta < 0) ? JOG_TOUCH_BACK : JOG_TOUCH_FWD;
        period = (pkt->jog_delta != 0) ? JOG_PERIOD_TURNING : JOG_PERIOD_STOPPED;
    }
    uint16_t current_pos = (uint16_t)(deck->frame[DECK_OFF_JOG_POS_LO] | (deck->frame[DECK_OFF_JOG_POS_HI] << 8));
    current_pos += (uint16_t)pkt->jog_delta;
    
    subucom_set_deck_jog(deck->frame, touch, current_pos, period);
    
    /* Calibrate 14-bit tempo slider (0..16383, center 8192) to 16-bit tempo register (center 517) */
    uint16_t raw_tempo = (uint16_t)(517 + ((int32_t)pkt->tempo_slider - 8192) * 506 / 8192);
    subucom_set_deck_tempo(deck->frame, raw_tempo);
    
    subucom_restamp_crc(deck->frame, bus_deck);
    pthread_mutex_unlock(&deck->lock);
    notify_channel(deck);
}

/* Translates high-level subucom_mixer_packet_t into low-level 152B mixer frame */
static void translate_mixer_packet(const subucom_mixer_packet_t *pkt) {
    if (!pkt) return;
    virt_spi_chan_t *mixer = &g_channels[SUBUCOM_BUS_MIXER];
    pthread_mutex_lock(&mixer->lock);
    
    /* 10-bit Channel Faders & Crossfader */
    subucom_set10(mixer->frame, MX_REG_CH1_FADER, pkt->ch1_fader);
    subucom_set10(mixer->frame, MX_REG_CH2_FADER, pkt->ch2_fader);
    subucom_set10(mixer->frame, MX_REG_CH3_FADER, pkt->ch3_fader);
    subucom_set10(mixer->frame, MX_REG_CH4_FADER, pkt->ch4_fader);
    subucom_set10(mixer->frame, MX_REG_CROSSFADER, pkt->crossfader);

    /* 10-bit EQs & Trims */
    subucom_set10(mixer->frame, MX_REG_CH1_TRIM, pkt->ch1_trim);
    subucom_set10(mixer->frame, MX_REG_CH1_HI,   pkt->ch1_hi);
    subucom_set10(mixer->frame, MX_REG_CH1_MID,  pkt->ch1_mid);
    subucom_set10(mixer->frame, MX_REG_CH1_LOW,  pkt->ch1_low);

    subucom_set10(mixer->frame, MX_REG_CH2_TRIM, pkt->ch2_trim);
    subucom_set10(mixer->frame, MX_REG_CH2_HI,   pkt->ch2_hi);
    subucom_set10(mixer->frame, MX_REG_CH2_MID,  pkt->ch2_mid);
    subucom_set10(mixer->frame, MX_REG_CH2_LOW,  pkt->ch2_low);

    subucom_set10(mixer->frame, MX_REG_CH3_TRIM, pkt->ch3_trim);
    subucom_set10(mixer->frame, MX_REG_CH3_HI,   pkt->ch3_hi);
    subucom_set10(mixer->frame, MX_REG_CH3_MID,  pkt->ch3_mid);
    subucom_set10(mixer->frame, MX_REG_CH3_LOW,  pkt->ch3_low);

    subucom_set10(mixer->frame, MX_REG_CH4_TRIM, pkt->ch4_trim);
    subucom_set10(mixer->frame, MX_REG_CH4_HI,   pkt->ch4_hi);
    subucom_set10(mixer->frame, MX_REG_CH4_MID,  pkt->ch4_mid);
    subucom_set10(mixer->frame, MX_REG_CH4_LOW,  pkt->ch4_low);

    /* Master & Color */
    subucom_set10(mixer->frame, MX_REG_MASTER_LEVEL, (uint16_t)(pkt->master_level * 4));
    subucom_set10(mixer->frame, MX_REG_CH1_COLOR, pkt->ch1_color);
    subucom_set10(mixer->frame, MX_REG_CH2_COLOR, pkt->ch2_color);
    subucom_set10(mixer->frame, MX_REG_CH3_COLOR, pkt->ch3_color);
    subucom_set10(mixer->frame, MX_REG_CH4_COLOR, pkt->ch4_color);

    /* Cue Buttons: Byte 0x14 */
    mixer->frame[MIXER_OFF_BTN_ROW1] &= ~(MIXER_MASK_CUE1 | MIXER_MASK_CUE2 | MIXER_MASK_CUE3 | MIXER_MASK_CUE4);
    if (pkt->ch_cue_states & 0x01) mixer->frame[MIXER_OFF_BTN_ROW1] |= MIXER_MASK_CUE1;
    if (pkt->ch_cue_states & 0x02) mixer->frame[MIXER_OFF_BTN_ROW1] |= MIXER_MASK_CUE2;
    if (pkt->ch_cue_states & 0x04) mixer->frame[MIXER_OFF_BTN_ROW1] |= MIXER_MASK_CUE3;
    if (pkt->ch_cue_states & 0x08) mixer->frame[MIXER_OFF_BTN_ROW1] |= MIXER_MASK_CUE4;
    
    /* Master Cue: Byte 0x15 bit 0x40 */
    mixer->frame[MIXER_OFF_BTN_ROW2] &= ~MIXER_MASK_MASTER_CUE;
    if (pkt->ch_cue_states & 0x10) mixer->frame[MIXER_OFF_BTN_ROW2] |= MIXER_MASK_MASTER_CUE;

    /* Browse Knob Counter & Push */
    mixer->frame[MIXER_OFF_KNOB_CTR] = (uint8_t)(mixer->frame[MIXER_OFF_KNOB_CTR] + pkt->browse_delta);
    mixer->frame[MIXER_OFF_KNOB_PUSH] = pkt->browse_push ? MIXER_MASK_KNOB_PUSH : 0x00;

    /* Screen Tab buttons & Back */
    mixer->frame[MIXER_OFF_TABS] &= ~(PANEL_MASK_SOURCE | PANEL_MASK_BROWSE | PANEL_MASK_TAG_LIST | PANEL_MASK_PLAYLIST | PANEL_MASK_SEARCH | PANEL_MASK_MENU);
    if (pkt->browse_view == 1) mixer->frame[MIXER_OFF_TABS] |= PANEL_MASK_BROWSE;
    else if (pkt->browse_view == 2) mixer->frame[MIXER_OFF_TABS] |= PANEL_MASK_SOURCE;
    else if (pkt->browse_view == 3) mixer->frame[MIXER_OFF_TABS] |= PANEL_MASK_MENU;
    else if (pkt->browse_view == 4) mixer->frame[MIXER_OFF_TABS] |= PANEL_MASK_TAG_LIST;
    else if (pkt->browse_view == 5) mixer->frame[MIXER_OFF_TABS] |= PANEL_MASK_PLAYLIST;
    else if (pkt->browse_view == 6) mixer->frame[MIXER_OFF_TABS] |= PANEL_MASK_SEARCH;

    mixer->frame[MIXER_OFF_CORNERS] &= ~(PANEL_MASK_BACK | PANEL_MASK_LOAD1 | PANEL_MASK_LOAD2 | PANEL_MASK_LOAD3 | PANEL_MASK_LOAD4);
    if (pkt->browse_back) mixer->frame[MIXER_OFF_CORNERS] |= PANEL_MASK_BACK;
    if (pkt->load_deck1)  mixer->frame[MIXER_OFF_CORNERS] |= PANEL_MASK_LOAD1;
    if (pkt->load_deck2)  mixer->frame[MIXER_OFF_CORNERS] |= PANEL_MASK_LOAD2;
    if (pkt->load_deck3)  mixer->frame[MIXER_OFF_CORNERS] |= PANEL_MASK_LOAD3;
    if (pkt->load_deck4)  mixer->frame[MIXER_OFF_CORNERS] |= PANEL_MASK_LOAD4;

    /* Shared Press Latch */
    bool pressed = (pkt->browse_push || pkt->browse_back || pkt->browse_view || pkt->load_deck1 || pkt->load_deck2 || pkt->load_deck3 || pkt->load_deck4);
    mixer->frame[MIXER_OFF_LATCH] = pressed ? MIXER_LATCH_PRESSED : MIXER_LATCH_IDLE;

    subucom_restamp_crc(mixer->frame, SUBUCOM_BUS_MIXER);
    pthread_mutex_unlock(&mixer->lock);
    notify_channel(mixer);
}

/* External server worker: listens on UDP 9999 & UNIX socket /tmp/subucom_ctrl.sock */
static void* subucom_server_worker(void *arg) {
    (void)arg;
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd >= 0) {
        int opt = 1;
        setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 }; /* 10ms timeout = 100 Hz */
        setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
        sin.sin_port = htons(SUBUCOM_UDP_PORT);
        if (bind(udp_fd, (struct sockaddr*)&sin, sizeof(sin)) == 0) {
            fprintf(stderr, LOG_TAG "Controller listener bound to UDP port %d\n", SUBUCOM_UDP_PORT);
        } else {
            close(udp_fd);
            udp_fd = -1;
        }
    }

    /* UNIX domain socket server */
    int unix_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (unix_fd >= 0) {
        unlink(SUBUCOM_SOCKET_PATH);
        struct sockaddr_un sun;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        strncpy(sun.sun_path, SUBUCOM_SOCKET_PATH, sizeof(sun.sun_path) - 1);
        if (bind(unix_fd, (struct sockaddr*)&sun, sizeof(sun)) == 0) {
            listen(unix_fd, 4);
            int flags = fcntl(unix_fd, F_GETFL, 0);
            fcntl(unix_fd, F_SETFL, flags | O_NONBLOCK);
            fprintf(stderr, LOG_TAG "Controller listener bound to %s\n", SUBUCOM_SOCKET_PATH);
        } else {
            close(unix_fd);
            unix_fd = -1;
        }
    }

    uint8_t buffer[512];
    int client_fd = -1;

    while (g_running) {
        subucom_poll_ep147_playheads();

        /* Accept incoming UNIX socket connection */
        if (unix_fd >= 0 && client_fd < 0) {
            client_fd = accept(unix_fd, NULL, NULL);
            if (client_fd >= 0) {
                int flags = fcntl(client_fd, F_GETFL, 0);
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
            }
        }

        /* Read from UNIX socket */
        if (client_fd >= 0) {
            ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                if (n == 1) {
                    clear_bus_overlay(buffer[0]);
                } else if (n >= 1 + SUBUCOM_FRAME_SIZE) {
                    uint8_t bus = buffer[0];
                    apply_bus_overlay(bus, buffer + 1, SUBUCOM_FRAME_SIZE);
                }
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(client_fd);
                client_fd = -1;
            }
        }

        /* Read from UDP socket */
        if (udp_fd >= 0) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            ssize_t n = recvfrom(udp_fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &addr_len);
            if (n > 0) {
                /* Case 1: Wire overlay frame [bus_id] + 152 bytes */
                if (n == 1 + SUBUCOM_FRAME_SIZE) {
                    uint8_t bus = buffer[0];
                    apply_bus_overlay(bus, buffer + 1, SUBUCOM_FRAME_SIZE);
                }
                /* Case 2: Clear overlay request */
                else if (n == 1) {
                    clear_bus_overlay(buffer[0]);
                }
                /* Case 3: High-level deck packet */
                else if (n >= (ssize_t)sizeof(subucom_deck_packet_t)) {
                    const subucom_deck_packet_t *dp = (const subucom_deck_packet_t*)buffer;
                    if (dp->magic == SUBUCOM_HEADER_MAGIC) {
                        translate_deck_packet(dp);
                    }
                }
                /* Case 4: High-level mixer packet */
                else if (n >= (ssize_t)sizeof(subucom_mixer_packet_t)) {
                    const subucom_mixer_packet_t *mp = (const subucom_mixer_packet_t*)buffer;
                    if (mp->magic == SUBUCOM_HEADER_MAGIC) {
                        translate_mixer_packet(mp);
                    }
                }
                /* Case 5: Direct BFX control command [0xBF, cmd, val, (val2)] */
                else if (n >= 3 && buffer[0] == 0xBF) {
                    uint8_t cmd = buffer[1];
                    uint8_t val = buffer[2];
                    if (cmd == 1) subucom_set_bfx_type(val);
                    else if (cmd == 2) subucom_toggle_bfx_power();
                    else if (cmd == 3) subucom_set_bfx_channel(val);
                    else if (cmd == 4) subucom_step_beat(1);
                    else if (cmd == 5) subucom_step_beat(-1);
                    else if (cmd == 6) subucom_toggle_bfx_quantize();
                    else if (cmd == 7) {
                        uint8_t val2 = (n >= 4) ? buffer[3] : 0;
                        uint16_t time_ms = (n >= 6) ? ((uint16_t)buffer[4] | ((uint16_t)buffer[5] << 8)) : 0;
                        subucom_set_xpad(val, val2, time_ms);
                    }
                    else if (cmd == 8) subucom_set_bfx_fraction(val);
                }
            }
        } else {
            usleep(20000);
        }
    }

    if (client_fd >= 0) close(client_fd);
    if (unix_fd >= 0) { close(unix_fd); unlink(SUBUCOM_SOCKET_PATH); }
    if (udp_fd >= 0) close(udp_fd);
    return NULL;
}

__attribute__((constructor))
static void subucom_emu_init(void) {
    init_real_functions();
    g_is_ep147 = is_target_process();
    if (!g_is_ep147) return;

    /* Prevent LD_PRELOAD leakage into child processes */
    unsetenv("LD_PRELOAD");

    fprintf(stderr, LOG_TAG "AlphaTheta XDJ-AZ Virtual SPI Subucom & Ctrl Shim loaded (AArch64)\n");

    /* Initialize all 4 SPI channels with authentic captured hardware frames */
    for (int i = 0; i < SUBUCOM_BUS_COUNT; i++) {
        g_channels[i].efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        memcpy(g_channels[i].frame, az_bus_idle_frame[i], SUBUCOM_FRAME_SIZE);
        subucom_restamp_crc(g_channels[i].frame, i);
        g_channels[i].has_overlay = false;
        g_channels[i].mosi_fresh = false;
    }

    pthread_create(&g_server_thread, NULL, subucom_server_worker, NULL);
    g_thread_started = true;
}

__attribute__((destructor))
static void subucom_emu_fini(void) {
    if (!g_is_ep147) return;
    g_running = false;
    if (g_thread_started) {
        pthread_join(g_server_thread, NULL);
    }
    for (int i = 0; i < SUBUCOM_BUS_COUNT; i++) {
        if (g_channels[i].efd >= 0) real_close(g_channels[i].efd);
    }
}

/* --- Syscall Interceptions --- */

int open(const char *pathname, int flags, ...) {
    init_real_functions();
    init_chan_map();
    if (pathname && (strcmp(pathname, "/dev/hidg0") == 0 || strcmp(pathname, "/dev/gpiodrv") == 0)) {
        return real_open("/dev/null", flags);
    }
    int idx = get_chan_index_by_path(pathname);
    if (idx >= 0) {
        int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (efd >= 0) {
            pthread_mutex_lock(&g_map_lock);
            if (efd < 1024) g_fd_chan_map[efd] = idx;
            pthread_mutex_unlock(&g_map_lock);
            if (idx < SUBUCOM_BUS_COUNT) {
                g_channels[idx].efd = efd;
                g_channels[idx].is_open = true;
            }
            fprintf(stderr, LOG_TAG "open() -> Emulating %s as eventfd %d\n", pathname, efd);
            return efd;
        }
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
        return real_open(pathname, flags, mode);
    }
    return real_open(pathname, flags);
}

int open64(const char *pathname, int flags, ...) {
    init_real_functions();
    init_chan_map();
    if (pathname && (strcmp(pathname, "/dev/hidg0") == 0 || strcmp(pathname, "/dev/gpiodrv") == 0)) {
        return real_open64 ? real_open64("/dev/null", flags) : real_open("/dev/null", flags);
    }
    int idx = get_chan_index_by_path(pathname);
    if (idx >= 0) {
        int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (efd >= 0) {
            pthread_mutex_lock(&g_map_lock);
            if (efd < 1024) g_fd_chan_map[efd] = idx;
            pthread_mutex_unlock(&g_map_lock);
            if (idx < SUBUCOM_BUS_COUNT) {
                g_channels[idx].efd = efd;
                g_channels[idx].is_open = true;
            }
            fprintf(stderr, LOG_TAG "open64() -> Emulating %s as eventfd %d\n", pathname, efd);
            return efd;
        }
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
        return real_open64 ? real_open64(pathname, flags, mode) : real_open(pathname, flags, mode);
    }
    return real_open64 ? real_open64(pathname, flags) : real_open(pathname, flags);
}

int close(int fd) {
    init_real_functions();
    if (is_ctrl_fd(fd)) {
        pthread_mutex_lock(&g_map_lock);
        if (fd >= 0 && fd < 1024) g_fd_chan_map[fd] = -1;
        pthread_mutex_unlock(&g_map_lock);
        return real_close(fd);
    }
    virt_spi_chan_t *chan = get_chan_by_fd(fd);
    if (chan) {
        chan->is_open = false;
        pthread_mutex_lock(&g_map_lock);
        if (fd >= 0 && fd < 1024) g_fd_chan_map[fd] = -1;
        pthread_mutex_unlock(&g_map_lock);
        return real_close(fd);
    }
    return real_close(fd);
}

ssize_t read(int fd, void *buf, size_t count) {
    init_real_functions();

    /* /dev/subucom_ctrl read: consumes latest LED / Jog display frame */
    if (is_ctrl_fd(fd)) {
        if (!buf || count < SUBUCOM_LED_WIRE_SIZE) return -EINVAL;
        for (int i = 0; i < SUBUCOM_BUS_COUNT; i++) {
            virt_spi_chan_t *chan = &g_channels[i];
            pthread_mutex_lock(&chan->lock);
            if (chan->mosi_fresh) {
                uint8_t *p = (uint8_t*)buf;
                p[0] = (uint8_t)i; /* Bus index */
                memcpy(p + 1, chan->latest_mosi, SUBUCOM_LED_FRAME_SIZE);
                memcpy(p + 1 + SUBUCOM_LED_FRAME_SIZE, &chan->mosi_size, sizeof(uint32_t));
                chan->mosi_fresh = false;
                pthread_mutex_unlock(&chan->lock);
                return SUBUCOM_LED_WIRE_SIZE;
            }
            pthread_mutex_unlock(&chan->lock);
        }
        return 0;
    }

    virt_spi_chan_t *chan = get_chan_by_fd(fd);
    if (chan) {
        uint64_t val = 0;
        real_read(fd, &val, sizeof(val));
        
        size_t to_read = (count > SUBUCOM_FRAME_SIZE) ? SUBUCOM_FRAME_SIZE : count;
        pthread_mutex_lock(&chan->lock);
        /* Re-stamp CRC before delivery */
        subucom_restamp_crc(chan->frame, chan->bus_id);
        memcpy(buf, chan->frame, to_read);
        pthread_mutex_unlock(&chan->lock);
        return to_read;
    }
    return real_read(fd, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    init_real_functions();

    /* /dev/subucom_ctrl write: [bus_id] + 152 bytes */
    if (is_ctrl_fd(fd)) {
        if (!buf) return -EINVAL;
        if (count == 0) {
            clear_bus_overlay(-1); /* clear all */
            return 0;
        }
        const uint8_t *p = (const uint8_t*)buf;
        uint8_t bus = p[0];
        if (count == 1) {
            clear_bus_overlay(bus);
            return 1;
        }
        if (count >= 1 + SUBUCOM_FRAME_SIZE) {
            apply_bus_overlay(bus, p + 1, SUBUCOM_FRAME_SIZE);
            return 1 + SUBUCOM_FRAME_SIZE;
        }
        return -EINVAL;
    }

    virt_spi_chan_t *chan = get_chan_by_fd(fd);
    if (chan) {
        size_t to_write = (count > SUBUCOM_FRAME_SIZE) ? SUBUCOM_FRAME_SIZE : count;
        pthread_mutex_lock(&chan->lock);
        memcpy(chan->latest_mosi, buf, (to_write > SUBUCOM_LED_FRAME_SIZE) ? SUBUCOM_LED_FRAME_SIZE : to_write);
        chan->mosi_size = (uint32_t)to_write;
        chan->mosi_fresh = true;
        pthread_mutex_unlock(&chan->lock);
        update_shm_mosi(chan->bus_id, buf, (uint32_t)to_write);
        return to_write;
    }
    return real_write(fd, buf, count);
}

int ioctl(int fd, unsigned long request, ...) {
    init_real_functions();
    virt_spi_chan_t *chan = get_chan_by_fd(fd);
    if (chan) {
        va_list args;
        va_start(args, request);
        void *argp = va_arg(args, void*);
        va_end(args);

        switch (request) {
            /* 1. Timer Status */
            case IOCTL_TIMER_STATUS_READ: {
                uint16_t *st = (uint16_t*)argp;
                if (st) *st = chan->timer_status;
                return 0;
            }
            case IOCTL_TIMER_STATUS_WRITE: {
                const uint16_t *st = (const uint16_t*)argp;
                if (st) chan->timer_status = *st;
                return 0;
            }

            /* 2. Bits per word */
            case IOCTL_BITS_PER_WORD_READ: {
                uint8_t *bpw = (uint8_t*)argp;
                if (bpw) *bpw = chan->bits_per_word;
                return 0;
            }
            case IOCTL_BITS_PER_WORD_WRITE: {
                const uint8_t *bpw = (const uint8_t*)argp;
                if (bpw) chan->bits_per_word = *bpw;
                return 0;
            }

            /* 3. MOSI Output Transfer (LED and Jog display frames from EP147) */
            case IOCTL_MOSI_TRANSFER: {
                const struct mosi_cmd *cmd = (const struct mosi_cmd*)argp;
                if (!cmd || !cmd->data_ptr) return 0;
                uint32_t sz = cmd->size;
                if (sz > SUBUCOM_LED_FRAME_SIZE) sz = SUBUCOM_LED_FRAME_SIZE;
                
                pthread_mutex_lock(&chan->lock);
                memcpy(chan->latest_mosi, (const void*)(uintptr_t)cmd->data_ptr, sz);
                chan->mosi_size = cmd->size;
                chan->mosi_fresh = true;
                pthread_mutex_unlock(&chan->lock);
                update_shm_mosi(chan->bus_id, (const void*)(uintptr_t)cmd->data_ptr, sz);
                return 0;
            }

            /* 4. RX Bytes */
            case IOCTL_RX_BYTES_READ: {
                uint32_t *rx = (uint32_t*)argp;
                if (rx) *rx = chan->rx_bytes;
                return 0;
            }
            case IOCTL_RX_BYTES_WRITE: {
                const uint32_t *rx = (const uint32_t*)argp;
                if (rx) chan->rx_bytes = *rx;
                return 0;
            }

            /* 5. Polling Interval */
            case IOCTL_INTERVAL_READ: {
                uint32_t *iv = (uint32_t*)argp;
                if (iv) *iv = chan->interval_us;
                return 0;
            }
            case IOCTL_INTERVAL_WRITE: {
                const uint32_t *iv = (const uint32_t*)argp;
                if (iv) chan->interval_us = *iv;
                return 0;
            }

            /* Legacy / Rockchip SPI transfers */
            case SUBUCOM_IOC_TRANSFER: {
                struct subucom_ioc_transfer *tr = (struct subucom_ioc_transfer*)argp;
                if (!tr) { errno = EINVAL; return -1; }
                pthread_mutex_lock(&chan->lock);
                if (tr->tx_buf && tr->len > 0) {
                    size_t cp = (tr->len > SUBUCOM_LED_FRAME_SIZE) ? SUBUCOM_LED_FRAME_SIZE : tr->len;
                    memcpy(chan->latest_mosi, (const void*)tr->tx_buf, cp);
                    chan->mosi_size = (uint32_t)cp;
                    chan->mosi_fresh = true;
                }
                if (tr->rx_buf && tr->len > 0) {
                    size_t cp = (tr->len > SUBUCOM_FRAME_SIZE) ? SUBUCOM_FRAME_SIZE : tr->len;
                    subucom_restamp_crc(chan->frame, chan->bus_id);
                    memcpy((void*)tr->rx_buf, chan->frame, cp);
                }
                pthread_mutex_unlock(&chan->lock);
                return 0;
            }
            case SUBUCOM_IOC_GET_TIMER_STATUS: {
                uint8_t *st = (uint8_t*)argp;
                if (st) *st = (uint8_t)chan->timer_status;
                return 0;
            }
            case SUBUCOM_IOC_SET_TIMER: {
                uint32_t *ms = (uint32_t*)argp;
                if (ms) chan->interval_us = (*ms) * 1000;
                return 0;
            }
            case SUBUCOM_IOC_CLEAR_STATUS: {
                chan->timer_status = 0;
                return 0;
            }
            case SUBUCOM_IOC_SET_MAX_SPEED:
                return 0;
            case SUBUCOM_IOC_SET_BITS_PER_WORD: {
                uint8_t *bpw = (uint8_t*)argp;
                if (bpw) chan->bits_per_word = *bpw;
                return 0;
            }
            default:
                return 0;
        }
    }

    va_list args;
    va_start(args, request);
    void *argp = va_arg(args, void*);
    va_end(args);
    return real_ioctl(fd, request, argp);
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    init_real_functions();
    return real_poll(fds, nfds, timeout);
}

int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo_p, const sigset_t *sigmask) {
    init_real_functions();
    return real_ppoll ? real_ppoll(fds, nfds, tmo_p, sigmask) : real_poll(fds, nfds, tmo_p ? (tmo_p->tv_sec * 1000 + tmo_p->tv_nsec / 1000000) : -1);
}
