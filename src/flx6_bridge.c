/*
 * flx6_bridge.c -- Implementation of bridge functions connecting map_flx6
 *                  to XDJ-AZ Subucom SPI emulation and Shared Memory Audio DSP.
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
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdarg.h>
#include <math.h>

#include "ctrl_map.h"
#include "flx6_bridge.h"
#include "rbp_led.h"
#include "xdj_az_abi64.h"
#include "xdj_az_shm.h"

#define UDP_TARGET_IP      "127.0.0.1"
#define UDP_TARGET_PORT    9999

uint32_t g_dummy_mixer_route[4] = {0, 1, 2, 3};

static int g_sock_fd = -1;
static struct sockaddr_in g_udp_addr;

static int g_midi_fd = -1;
static pthread_mutex_t g_midi_lock = PTHREAD_MUTEX_INITIALIZER;

static xdj_az_shared_t *g_shm = NULL;
static int g_shm_fd = -1;

/* Hardware Subucom SPI Frames */
static uint8_t g_frame_mixer[SUBUCOM_FRAME_SIZE];
static uint8_t g_frame_erp[SUBUCOM_FRAME_SIZE];
static uint8_t g_frame_deck1[SUBUCOM_FRAME_SIZE]; /* Left deck (Decks 1 & 3) */
static uint8_t g_frame_deck2[SUBUCOM_FRAME_SIZE]; /* Right deck (Decks 2 & 4) */

/* Deck & Player state */
static bool g_play_state[4] = {false, false, false, false};
static bool g_track_loaded[4] = {true, true, true, true};
static bool s_jog_touched[4] = {false, false, false, false};
static uint8_t g_browse_ctr = 128;

static int g_xfader_assign[4] = { -1, 1, -1, 1 }; /* Ch1/3 -> A, Ch2/4 -> B */
static int g_xfader_curve = 1;
static float g_xfader_pos = 0.5f;

static inline void local_set10(uint8_t *f, size_t hi, size_t lo, uint8_t shift, uint16_t v) {
    if (v > 1023) v = 1023;
    f[hi] = (uint8_t)(v >> 2);
    f[lo] = (uint8_t)((f[lo] & ~(0x03 << shift)) | (((v & 0x03) << shift)));
}

static void send_bus_frame(uint8_t bus_id, const uint8_t *frame) {
    if (g_sock_fd < 0 || !frame) return;
    uint8_t packet[1 + SUBUCOM_FRAME_SIZE];
    packet[0] = bus_id;
    memcpy(packet + 1, frame, SUBUCOM_FRAME_SIZE);
    sendto(g_sock_fd, packet, sizeof(packet), 0, (struct sockaddr*)&g_udp_addr, sizeof(g_udp_addr));
}

void klog(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

unsigned long long shim_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}

int cc_to_10bit(int v) {
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    return (v * 1023) / 127;
}

int env_num(const char *name, int def) {
    const char *s = getenv(name);
    if (!s || !s[0]) return def;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    return (end && *end == '\0') ? (int)v : def;
}

double env_dnum(const char *name, double def) {
    const char *s = getenv(name);
    if (!s || !s[0]) return def;
    char *end = NULL;
    double v = strtod(s, &end);
    return (end && *end == '\0') ? v : def;
}

int env_on(const char *name, int def) {
    const char *s = getenv(name);
    if (!s || !s[0]) return def;
    if (strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0) return 1;
    if (strcmp(s, "0") == 0 || strcasecmp(s, "false") == 0 || strcasecmp(s, "no") == 0) return 0;
    return def;
}

int midi_note(int midi_ch, int note, int val) {
    if (g_midi_fd < 0) return -1;
    uint8_t m[3] = { (uint8_t)(0x90 | (midi_ch & 0x0F)), (uint8_t)(note & 0x7F), (uint8_t)(val & 0x7F) };
    pthread_mutex_lock(&g_midi_lock);
    ssize_t n = write(g_midi_fd, m, 3);
    pthread_mutex_unlock(&g_midi_lock);
    return (n == 3) ? 0 : -1;
}

int midi_cc(int midi_ch, int cc, int val) {
    if (g_midi_fd < 0) return -1;
    uint8_t m[3] = { (uint8_t)(0xB0 | (midi_ch & 0x0F)), (uint8_t)(cc & 0x7F), (uint8_t)(val & 0x7F) };
    pthread_mutex_lock(&g_midi_lock);
    ssize_t n = write(g_midi_fd, m, 3);
    pthread_mutex_unlock(&g_midi_lock);
    return (n == 3) ? 0 : -1;
}

void az_bridge_set_midi_fd(int fd) {
    pthread_mutex_lock(&g_midi_lock);
    g_midi_fd = fd;
    pthread_mutex_unlock(&g_midi_lock);
}

void az_bridge_init(void) {
    memcpy(g_frame_mixer, az_frame_spi3_mixer, SUBUCOM_FRAME_SIZE);
    memcpy(g_frame_erp,   az_frame_spi1_erp,   SUBUCOM_FRAME_SIZE);
    memcpy(g_frame_deck1, az_frame_spi5_deck,  SUBUCOM_FRAME_SIZE);
    memcpy(g_frame_deck2, az_frame_spi2_deck,  SUBUCOM_FRAME_SIZE);
    g_browse_ctr = 128;

    /* Initialize Deck selection (Deck 1 Left, Deck 2 Right): idle frames have no buttons held */

    /* Initialize Deck jog frames stopped (period 0xFFFF, touch released) */
    g_frame_deck1[9] = JOG_TOUCH_RELEASE;
    g_frame_deck1[12] = 0xFF;
    g_frame_deck1[13] = 0xFF;

    g_frame_deck2[9] = JOG_TOUCH_RELEASE;
    g_frame_deck2[12] = 0xFF;
    g_frame_deck2[13] = 0xFF;

    /* UDP client socket to subucom_emu */
    g_sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock_fd >= 0) {
        memset(&g_udp_addr, 0, sizeof(g_udp_addr));
        g_udp_addr.sin_family = AF_INET;
        g_udp_addr.sin_port = htons(UDP_TARGET_PORT);
        inet_pton(AF_INET, UDP_TARGET_IP, &g_udp_addr.sin_addr);

        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        send_bus_frame(SUBUCOM_BUS_DECK1, g_frame_deck1);
        send_bus_frame(SUBUCOM_BUS_DECK2, g_frame_deck2);
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
    }

    /* Shared Memory setup */
    g_shm_fd = shm_open(XDJ_AZ_SHM_FILE, O_RDWR, 0666);
    if (g_shm_fd < 0) g_shm_fd = open("/dev/shm/xdj_az_shared", O_RDWR, 0666);
    if (g_shm_fd < 0) {
        g_shm_fd = shm_open(XDJ_AZ_SHM_FILE, O_CREAT | O_RDWR, 0666);
        if (g_shm_fd < 0) g_shm_fd = open("/dev/shm/xdj_az_shared", O_CREAT | O_RDWR, 0666);
        if (g_shm_fd >= 0) {
            int fr = ftruncate(g_shm_fd, sizeof(xdj_az_shared_t));
            (void)fr;
        }
    }
    if (g_shm_fd >= 0) {
        void *p = mmap(NULL, sizeof(xdj_az_shared_t), PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
        if (p != MAP_FAILED) {
            g_shm = (xdj_az_shared_t*)p;
        }
    }
}

/* Mixer Engine setters/getters */
void *mixer_engine(void) {
    return (void*)1; /* Non-null handle */
}

static float s_channel_fader[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

void me_set_channel_fader(int input, float f) {
    if (input < 0 || input >= 4) return;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    s_channel_fader[input] = f;
    uint16_t fader10 = (uint16_t)(f * 1023.0f);

    if (input == 0)      local_set10(g_frame_mixer, MX_REG_CH1_FADER, fader10);
    else if (input == 1) local_set10(g_frame_mixer, MX_REG_CH2_FADER, fader10);
    else if (input == 2) local_set10(g_frame_mixer, MX_REG_CH3_FADER, fader10);
    else if (input == 3) local_set10(g_frame_mixer, MX_REG_CH4_FADER, fader10);
    send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);

    if (g_shm) {
        g_shm->channel_gain[input] = f;
        g_shm->dsp_seq++;
    }
}

float me_get_channel_fader(int input) {
    if (input >= 0 && input < 4) return s_channel_fader[input];
    return 1.0f;
}

void me_set_crossfader(float f) {
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    g_xfader_pos = f;
    uint16_t xfader10 = (uint16_t)(f * 1023.0f);
    local_set10(g_frame_mixer, MX_REG_CROSSFADER, xfader10);
    send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);

    if (g_shm) {
        g_shm->cross_position = f;
        g_shm->dsp_seq++;
    }
}

float me_get_crossfader(void) {
    return g_xfader_pos;
}

void me_set_crossfader_assign(int input, int assign) {
    if (input < 0 || input >= 4) return;
    /* assign: 0 = Thru, 1 = A (-1), 2 = B (+1) */
    int val = (assign == 1) ? -1 : (assign == 2 ? 1 : 0);
    g_xfader_assign[input] = val;
    if (g_shm) {
        g_shm->cross_assign[input] = val;
        g_shm->dsp_seq++;
    }
}

int me_get_crossfader_assign(int input) {
    if (input < 0 || input >= 4) return 0;
    int val = g_xfader_assign[input];
    return (val < 0) ? 1 : (val > 0 ? 2 : 0);
}

void me_set_crossfader_curve(int curve) {
    g_xfader_curve = curve;
}

int me_get_crossfader_curve(void) {
    return g_xfader_curve;
}

void me_set_cue(int idx, int on) {
    if (idx < 0 || idx >= 4) return;
    if (g_shm) {
        if (on) g_shm->cue_mask |= (1 << idx);
        else    g_shm->cue_mask &= ~(1 << idx);
        g_shm->dsp_seq++;
    }
    uint8_t cue_masks[4] = {MIXER_MASK_CUE1, MIXER_MASK_CUE2, MIXER_MASK_CUE3, MIXER_MASK_CUE4};
    if (on) g_frame_mixer[MIXER_OFF_BTN_ROW1] |= cue_masks[idx];
    else    g_frame_mixer[MIXER_OFF_BTN_ROW1] &= ~cue_masks[idx];
    send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
}

int me_get_cue(int idx) {
    if (g_shm && idx >= 0 && idx < 4) {
        return (g_shm->cue_mask & (1 << idx)) ? 1 : 0;
    }
    return 0;
}

void me_set_master_cue(int on) {
    if (g_shm) {
        if (on) g_shm->cue_mask |= 0x10;
        else    g_shm->cue_mask &= ~0x10;
        g_shm->dsp_seq++;
    }
    if (on) g_frame_mixer[MIXER_OFF_BTN_ROW2] |= MIXER_MASK_MASTER_CUE;
    else    g_frame_mixer[MIXER_OFF_BTN_ROW2] &= ~MIXER_MASK_MASTER_CUE;
    send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
}

int me_get_master_cue(void) {
    if (g_shm) {
        return (g_shm->cue_mask & 0x10) ? 1 : 0;
    }
    return 0;
}

/* Active deck tracking (Left: 0 or 2, Right: 1 or 3) */
static int s_active_deck[2] = { 0, 1 };

int flx6_get_active_deck(int side) {
    if (side < 0 || side > 1) return 0;
    return s_active_deck[side];
}

void flx6_switch_deck(int deck) {
    if (deck < 0 || deck >= 4) return;
    int side = (deck == 0 || deck == 2) ? 0 : 1;
    s_active_deck[side] = deck;

    /* Native Subucom ERP selection pulses (verified from az-runtime-lab DECK-LOAD.md:241-245)
     * Deck 1: byte 8 bit 0
     * Deck 3: byte 9 bit 0
     * Deck 2: byte 16 bit 0
     * Deck 4: byte 17 bit 0
     */
    int offset = 8;
    if (deck == 0) offset = 8;
    else if (deck == 2) offset = 9;
    else if (deck == 1) offset = 16;
    else if (deck == 3) offset = 17;

    /* Ensure other deck select buttons on this side are released */
    if (side == 0) {
        g_frame_erp[8] &= ~0x01;
        g_frame_erp[9] &= ~0x01;
    } else {
        g_frame_erp[16] &= ~0x01;
        g_frame_erp[17] &= ~0x01;
    }

    g_frame_erp[offset] |= 0x01;
    send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
    usleep(25000);
    g_frame_erp[offset] &= ~0x01;
    send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);

    klog("[flx6_bridge] Switched side %d to DECK %d (ERP byte %d bit 0 pulsed)\n",
         side + 1, deck + 1, offset);
}

/* Playback and track query functions */
int rbp_is_playing(int deck) {
    if (deck < 0 || deck >= 4) return 0;
    if (g_shm && g_shm->is_playing[deck]) return 1;
    return g_play_state[deck] ? 1 : 0;
}

int rbp_is_loaded(int deck) {
    if (deck < 0 || deck >= 4) return 1;
    if (g_shm && g_shm->track_loaded[deck]) return 1;
    return g_track_loaded[deck] ? 1 : 0;
}

long rbp_get_playing_pos(int deck) {
    if (deck >= 0 && deck < 4 && g_shm && g_shm->playhead_pos[deck] > 0) {
        return (long)(g_shm->playhead_pos[deck]);
    }
    unsigned long long ms = shim_now_ms();
    return (long)((ms * 44100ULL) / 1000ULL);
}

int rbp_has_arrived_at_end(int deck) {
    (void)deck;
    return 0;
}

static const int s_bfx_fractions[9][2] = {
    {1, 16}, {1, 8}, {1, 4}, {1, 2}, {3, 4}, {1, 1}, {2, 1}, {4, 1}, {8, 1}
};
static int s_bfx_frac_idx = 3; /* Default: 1/2 beat */

int rbp_beatfx_numer(void) {
    if (s_bfx_frac_idx >= 0 && s_bfx_frac_idx < 9) return s_bfx_fractions[s_bfx_frac_idx][0];
    return 1;
}
int rbp_beatfx_denom(void) {
    if (s_bfx_frac_idx >= 0 && s_bfx_frac_idx < 9) return s_bfx_fractions[s_bfx_frac_idx][1];
    return 2;
}
int rbp_beatfx_on(void) {
    return (g_frame_mixer[MIXER_OFF_FX_SEL] & MIXER_MASK_FX_ON) ? 1 : 0;
}
int rbp_beatfx_type(void) {
    return (g_frame_mixer[MIXER_OFF_FX_SEL] >> 4) & 0x0F;
}
int rbp_beatfx_channel(void) {
    uint8_t row4 = g_frame_mixer[MIXER_OFF_BTN_ROW4];
    if (row4 & MIXER_MASK_FX_CH1) return 0;
    if (row4 & MIXER_MASK_FX_CH2) return 1;
    if (row4 & MIXER_MASK_FX_CH3) return 2;
    if (row4 & MIXER_MASK_FX_CH4) return 3;
    if (row4 & MIXER_MASK_FX_CH_MST) return 4;
    return 0;
}
int hotcue_delete(int deck, int pad) {
    (void)deck; (void)pad;
    return 0;
}

/* Dispatch internal keycodes to Subucom SPI frames & SHM */
void send_rx_key(int key, int op, int sch, long val) {
    send_rx_key_fl(key, op, sch, val, 0.0f, val);
}

void send_rx_key_f(int key, int op, int sch, long val, float fval) {
    send_rx_key_fl(key, op, sch, val, fval, val);
}

void send_rx_key_fl(int key, int op, int sch, long val, float fval, long lval) {
    (void)val; (void)lval;
    bool is_press = (op == OP_PRESS || op == OP_REPEAT);
    int deck_idx = (sch >= 1 && sch <= 4) ? (sch - 1) : 0;
    int side = (deck_idx == 0 || deck_idx == 2) ? 0 : 1;
    int d_off = side * ERP_DECK2_STRIDE;
    uint8_t *deck_frame = (side == 0) ? g_frame_deck1 : g_frame_deck2;
    uint8_t bus_deck = (side == 0) ? SUBUCOM_BUS_DECK1 : SUBUCOM_BUS_DECK2;

    switch (key) {
    case K_PLAY:
        if (is_press) {
            g_play_state[deck_idx] = !g_play_state[deck_idx];
            g_frame_erp[ERP_OFF_PLAY_CUE + d_off] |= ERP_MASK_PLAY;
        } else {
            g_frame_erp[ERP_OFF_PLAY_CUE + d_off] &= ~ERP_MASK_PLAY;
        }
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_CUE:
        if (is_press) {
            g_play_state[deck_idx] = false;
            g_frame_erp[ERP_OFF_PLAY_CUE + d_off] |= ERP_MASK_CUE;
        } else {
            g_frame_erp[ERP_OFF_PLAY_CUE + d_off] &= ~ERP_MASK_CUE;
        }
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_SYNC:
        if (is_press) {
            g_frame_erp[ERP_OFF_BEAT_SYNC + d_off] |= ERP_MASK_BEAT_SYNC;
            if (g_shm) g_shm->is_sync[deck_idx] ^= 1;
        } else {
            g_frame_erp[ERP_OFF_BEAT_SYNC + d_off] &= ~ERP_MASK_BEAT_SYNC;
        }
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_MT:
        if (is_press) g_frame_erp[ERP_OFF_MASTER_TEMPO + d_off] ^= ERP_MASK_MASTER_TEMPO;
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_MASTER:
        if (is_press) {
            g_frame_erp[ERP_OFF_MASTER + d_off] |= ERP_MASK_MASTER;
            if (g_shm) {
                for (int i = 0; i < 4; i++) g_shm->is_master[i] = (i == deck_idx) ? 1 : 0;
            }
        } else {
            g_frame_erp[ERP_OFF_MASTER + d_off] &= ~ERP_MASK_MASTER;
        }
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_LOOPIN:
        if (is_press) {
            g_frame_erp[ERP_OFF_LOOP_IN + d_off] |= ERP_MASK_LOOP_IN;
            led_loop_armed[deck_idx] = 1;
        } else {
            g_frame_erp[ERP_OFF_LOOP_IN + d_off] &= ~ERP_MASK_LOOP_IN;
        }
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_LOOPOUT:
        if (is_press) {
            g_frame_erp[ERP_OFF_LOOP_OUT + d_off] |= ERP_MASK_LOOP_OUT;
            led_loop_armed[deck_idx] = 0;
            if (g_shm) g_shm->is_looping[deck_idx] = 1;
        } else {
            g_frame_erp[ERP_OFF_LOOP_OUT + d_off] &= ~ERP_MASK_LOOP_OUT;
        }
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_RELOOP:
        if (is_press) {
            g_frame_erp[ERP_OFF_RELOOP + d_off] |= ERP_MASK_RELOOP;
            led_loop_armed[deck_idx] = 0;
            if (g_shm) g_shm->is_looping[deck_idx] ^= 1;
        } else {
            g_frame_erp[ERP_OFF_RELOOP + d_off] &= ~ERP_MASK_RELOOP;
        }
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_CALL_PREV:
        if (is_press) g_frame_erp[ERP_OFF_CALL_PREV + d_off] |= ERP_MASK_CALL_PREV;
        else          g_frame_erp[ERP_OFF_CALL_PREV + d_off] &= ~ERP_MASK_CALL_PREV;
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_CALL_NEXT:
        if (is_press) g_frame_erp[ERP_OFF_CALL_NEXT + d_off] |= ERP_MASK_CALL_NEXT;
        else          g_frame_erp[ERP_OFF_CALL_NEXT + d_off] &= ~ERP_MASK_CALL_NEXT;
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_MEM_CUE:
        if (is_press) g_frame_erp[ERP_OFF_CALL_PREV + d_off] |= ERP_MASK_CALL_PREV;
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_MEM_DEL:
        if (is_press) g_frame_erp[ERP_OFF_DELETE + d_off] |= ERP_MASK_DELETE;
        else          g_frame_erp[ERP_OFF_DELETE + d_off] &= ~ERP_MASK_DELETE;
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_HOTCUE:
        if (is_press) g_frame_erp[ERP_OFF_PADMODE + d_off] = ERP_MASK_HOTCUE;
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_ALOOP:
        if (is_press) g_frame_erp[ERP_OFF_PADMODE + d_off] = ERP_MASK_BEATLOOP;
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_SLIPLOOP:
        if (is_press) g_frame_erp[ERP_OFF_PADMODE + d_off] = ERP_MASK_SLIPLOOP;
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_BEATJUMP:
        if (is_press) g_frame_erp[ERP_OFF_PADMODE + d_off] = ERP_MASK_BEATJUMP;
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_SELECTOR:
        if (op == OP_ROTATE) {
            int delta = (val < 0) ? -1 : 1;
            g_browse_ctr = (uint8_t)(g_browse_ctr + delta);
            g_frame_mixer[MIXER_OFF_KNOB_CTR] = g_browse_ctr;
        } else {
            g_frame_mixer[MIXER_OFF_KNOB_PUSH] = is_press ? MIXER_MASK_KNOB_PUSH : 0x00;
            g_frame_mixer[MIXER_OFF_LATCH] = is_press ? MIXER_LATCH_PRESSED : MIXER_LATCH_IDLE;
        }
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;

    case K_BACK:
        if (is_press) g_frame_mixer[MIXER_OFF_CORNERS] |= PANEL_MASK_BACK;
        else          g_frame_mixer[MIXER_OFF_CORNERS] &= ~PANEL_MASK_BACK;
        g_frame_mixer[MIXER_OFF_LATCH] = is_press ? MIXER_LATCH_PRESSED : MIXER_LATCH_IDLE;
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;

    case K_BROWSE:
        if (is_press) g_frame_mixer[MIXER_OFF_TABS] |= PANEL_MASK_BROWSE;
        else          g_frame_mixer[MIXER_OFF_TABS] &= ~PANEL_MASK_BROWSE;
        g_frame_mixer[MIXER_OFF_LATCH] = is_press ? MIXER_LATCH_PRESSED : MIXER_LATCH_IDLE;
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;

    case K_SOURCE:
        if (is_press) g_frame_mixer[MIXER_OFF_TABS] |= PANEL_MASK_SOURCE;
        else          g_frame_mixer[MIXER_OFF_TABS] &= ~PANEL_MASK_SOURCE;
        g_frame_mixer[MIXER_OFF_LATCH] = is_press ? MIXER_LATCH_PRESSED : MIXER_LATCH_IDLE;
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;

    case K_LOAD: {
        uint8_t load_masks[4] = { PANEL_MASK_LOAD1, PANEL_MASK_LOAD2, PANEL_MASK_LOAD3, PANEL_MASK_LOAD4 };
        if (deck_idx >= 0 && deck_idx < 4) {
            if (is_press) {
                g_frame_mixer[MIXER_OFF_CORNERS] |= load_masks[deck_idx];
                g_track_loaded[deck_idx] = true;
            } else {
                g_frame_mixer[MIXER_OFF_CORNERS] &= ~load_masks[deck_idx];
            }
            g_frame_mixer[MIXER_OFF_LATCH] = is_press ? MIXER_LATCH_PRESSED : MIXER_LATCH_IDLE;
            send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        }
        break;
    }

    case K_SHORTCUT:
        if (is_press) g_frame_mixer[MIXER_OFF_CORNERS] |= PANEL_MASK_SHORTCUT;
        else          g_frame_mixer[MIXER_OFF_CORNERS] &= ~PANEL_MASK_SHORTCUT;
        g_frame_mixer[MIXER_OFF_LATCH] = is_press ? MIXER_LATCH_PRESSED : MIXER_LATCH_IDLE;
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;

    case K_MENU:
        if (is_press) g_frame_mixer[MIXER_OFF_TABS] |= PANEL_MASK_MENU;
        else          g_frame_mixer[MIXER_OFF_TABS] &= ~PANEL_MASK_MENU;
        g_frame_mixer[MIXER_OFF_LATCH] = is_press ? MIXER_LATCH_PRESSED : MIXER_LATCH_IDLE;
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;

    case K_FILTER:
    case K_DUBECHO:
    case K_NOISE:
    case K_SPACE:
    case K_CRUSH:
    case K_SWEEP: {
        int cfx_type = 1;
        uint8_t mask = MIXER_MASK_SCFX_FILTER;
        if (key == K_FILTER)       { cfx_type = 1; mask = MIXER_MASK_SCFX_FILTER; }
        else if (key == K_DUBECHO) { cfx_type = 4; mask = MIXER_MASK_SCFX_DUBECHO; }
        else if (key == K_NOISE)   { cfx_type = 2; mask = MIXER_MASK_SCFX_NOISE; }
        else if (key == K_SPACE)   { cfx_type = 5; mask = MIXER_MASK_SCFX_SPACE; }
        else if (key == K_CRUSH)   { cfx_type = 6; mask = MIXER_MASK_SCFX_CRUSH; }
        else if (key == K_SWEEP)   { cfx_type = 3; mask = MIXER_MASK_SCFX_SWEEP; }

        if (g_shm) {
            for (int c = 0; c < 4; c++) g_shm->cfx_type[c] = cfx_type;
            g_shm->dsp_seq++;
        }
        g_frame_mixer[MIXER_OFF_BTN_ROW2] &= ~(MIXER_MASK_SCFX_SPACE | MIXER_MASK_SCFX_DUBECHO |
                                               MIXER_MASK_SCFX_CRUSH | MIXER_MASK_SCFX_SWEEP |
                                               MIXER_MASK_SCFX_NOISE | MIXER_MASK_SCFX_FILTER);
        g_frame_mixer[MIXER_OFF_BTN_ROW2] |= mask;
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;
    }

    case K_COLOR: {
        int c = (sch >= 1 && sch <= 4) ? (sch - 1) : 0;
        uint16_t val10 = (uint16_t)(fval * 1023.0f);
        if (c == 0)      local_set10(g_frame_mixer, MX_REG_CH1_COLOR, val10);
        else if (c == 1) local_set10(g_frame_mixer, MX_REG_CH2_COLOR, val10);
        else if (c == 2) local_set10(g_frame_mixer, MX_REG_CH3_COLOR, val10);
        else if (c == 3) local_set10(g_frame_mixer, MX_REG_CH4_COLOR, val10);
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        if (g_shm) {
            g_shm->cfx_color[c] = fval;
            g_shm->dsp_seq++;
        }
        break;
    }

    case K_TRIM: {
        int c = (sch >= 1 && sch <= 4) ? (sch - 1) : 0;
        uint16_t val10 = (uint16_t)(fval * 1023.0f);
        if (c == 0)      local_set10(g_frame_mixer, MX_REG_CH1_TRIM, val10);
        else if (c == 1) local_set10(g_frame_mixer, MX_REG_CH2_TRIM, val10);
        else if (c == 2) local_set10(g_frame_mixer, MX_REG_CH3_TRIM, val10);
        else if (c == 3) local_set10(g_frame_mixer, MX_REG_CH4_TRIM, val10);
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;
    }

    case K_EQH: {
        int c = (sch >= 1 && sch <= 4) ? (sch - 1) : 0;
        uint16_t val10 = (uint16_t)(fval * 1023.0f);
        if (c == 0)      local_set10(g_frame_mixer, MX_REG_CH1_HI, val10);
        else if (c == 1) local_set10(g_frame_mixer, MX_REG_CH2_HI, val10);
        else if (c == 2) local_set10(g_frame_mixer, MX_REG_CH3_HI, val10);
        else if (c == 3) local_set10(g_frame_mixer, MX_REG_CH4_HI, val10);
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        if (g_shm) {
            g_shm->eq_high[c] = fval;
            g_shm->dsp_seq++;
        }
        break;
    }

    case K_EQM: {
        int c = (sch >= 1 && sch <= 4) ? (sch - 1) : 0;
        uint16_t val10 = (uint16_t)(fval * 1023.0f);
        if (c == 0)      local_set10(g_frame_mixer, MX_REG_CH1_MID, val10);
        else if (c == 1) local_set10(g_frame_mixer, MX_REG_CH2_MID, val10);
        else if (c == 2) local_set10(g_frame_mixer, MX_REG_CH3_MID, val10);
        else if (c == 3) local_set10(g_frame_mixer, MX_REG_CH4_MID, val10);
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        if (g_shm) {
            g_shm->eq_mid[c] = fval;
            g_shm->dsp_seq++;
        }
        break;
    }

    case K_EQL: {
        int c = (sch >= 1 && sch <= 4) ? (sch - 1) : 0;
        uint16_t val10 = (uint16_t)(fval * 1023.0f);
        if (c == 0)      local_set10(g_frame_mixer, MX_REG_CH1_LOW, val10);
        else if (c == 1) local_set10(g_frame_mixer, MX_REG_CH2_LOW, val10);
        else if (c == 2) local_set10(g_frame_mixer, MX_REG_CH3_LOW, val10);
        else if (c == 3) local_set10(g_frame_mixer, MX_REG_CH4_LOW, val10);
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        if (g_shm) {
            g_shm->eq_low[c] = fval;
            g_shm->dsp_seq++;
        }
        break;
    }

    case K_FADER:
        me_set_channel_fader(deck_idx, fval);
        break;

    case K_XFADER:
        me_set_crossfader(fval);
        break;

    case K_HPMIX: {
        uint16_t val10 = (uint16_t)(fval * 1023.0f);
        local_set10(g_frame_mixer, MX_REG_HP_MIX, val10);
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        if (g_shm) {
            g_shm->cue_master_mix = fval;
            g_shm->dsp_seq++;
        }
        break;
    }

    case K_HPLEVEL: {
        uint16_t val10 = (uint16_t)(fval * 1023.0f);
        local_set10(g_frame_mixer, MX_REG_HP_LEVEL, val10);
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        if (g_shm) {
            g_shm->headphones_gain = fval;
            g_shm->dsp_seq++;
        }
        break;
    }

    case K_MASTERLVL: {
        uint16_t val10 = (uint16_t)(fval * 1023.0f);
        local_set10(g_frame_mixer, MX_REG_MASTER_LEVEL, val10);
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        if (g_shm) {
            g_shm->master_gain = fval;
            g_shm->dsp_seq++;
        }
        break;
    }

    case K_VINYL:
        if (is_press) g_frame_erp[ERP_OFF_JOG_MODE + d_off] |= ERP_MASK_JOG_MODE;
        else          g_frame_erp[ERP_OFF_JOG_MODE + d_off] &= ~ERP_MASK_JOG_MODE;
        send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        break;

    case K_JOG_TOUCH:
        s_jog_touched[deck_idx] = is_press;
        deck_frame[9] = is_press ? JOG_TOUCH_FWD : JOG_TOUCH_RELEASE;
        deck_frame[12] = 0xFF;
        deck_frame[13] = 0xFF;
        send_bus_frame(bus_deck, deck_frame);
        break;

    case K_JOG_ROT: {
        uint16_t pos = (uint16_t)(lval & 0xFFFF);
        float speed = fabsf(fval);
        int32_t raw_period = JOG_STOPPED;
        if (speed > 0.001f) {
            raw_period = (int32_t)round(1.8f / (speed * 720.0f) * 1000000.0f);
            if (raw_period < 1) raw_period = 1;
            if (raw_period > 65534) raw_period = 65534;
        }
        uint16_t period = (uint16_t)raw_period;
        deck_frame[10] = (uint8_t)(pos & 0xFF);
        deck_frame[11] = (uint8_t)((pos >> 8) & 0xFF);
        deck_frame[12] = (uint8_t)(period & 0xFF);
        deck_frame[13] = (uint8_t)((period >> 8) & 0xFF);

        if (s_jog_touched[deck_idx]) {
            if (speed <= 0.001f) {
                deck_frame[9] = JOG_TOUCH_FWD;
            } else {
                deck_frame[9] = (fval >= 0.0f) ? JOG_TOUCH_FWD : JOG_TOUCH_BACK;
            }
        } else {
            if (speed <= 0.001f) {
                deck_frame[9] = JOG_TOUCH_RELEASE;
            } else {
                deck_frame[9] = (fval >= 0.0f) ? 0x04 : 0x00;
            }
        }
        send_bus_frame(bus_deck, deck_frame);
        break;
    }

    case K_TEMPO_SLIDER: {
        /* norm: -1.0 .. +1.0 -> Subucom position 12..1012 */
        uint16_t pos = (uint16_t)(512 + (fval * 500.0f));
        subucom_set_deck_tempo(deck_frame, pos);
        send_bus_frame(bus_deck, deck_frame);
        break;
    }

    case K_BFX:
        if (is_press) g_frame_mixer[MIXER_OFF_FX_SEL] ^= MIXER_MASK_FX_ON;
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;

    case K_BFXTYPE: {
        uint8_t type_nibble = ((uint8_t)val & 0x0F) << 4;
        g_frame_mixer[MIXER_OFF_FX_SEL] = (g_frame_mixer[MIXER_OFF_FX_SEL] & 0x0F) | type_nibble;
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;
    }

    case K_BFXCH: {
        int ch_idx = (int)val;
        g_frame_mixer[MIXER_OFF_BTN_ROW4] &= ~(MIXER_MASK_FX_CH1 | MIXER_MASK_FX_CH2 |
                                              MIXER_MASK_FX_CH3 | MIXER_MASK_FX_CH4 |
                                              MIXER_MASK_FX_CH_MST | MIXER_MASK_FX_CH_MIC);
        if (ch_idx == 0) g_frame_mixer[MIXER_OFF_BTN_ROW4] |= MIXER_MASK_FX_CH1;
        else if (ch_idx == 1) g_frame_mixer[MIXER_OFF_BTN_ROW4] |= MIXER_MASK_FX_CH2;
        else if (ch_idx == 2) g_frame_mixer[MIXER_OFF_BTN_ROW4] |= MIXER_MASK_FX_CH3;
        else if (ch_idx == 3) g_frame_mixer[MIXER_OFF_BTN_ROW4] |= MIXER_MASK_FX_CH4;
        else if (ch_idx >= 4) g_frame_mixer[MIXER_OFF_BTN_ROW4] |= MIXER_MASK_FX_CH_MST;
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;
    }

    case K_TIME: {
        int delta = (int)val;
        uint16_t cur = (uint16_t)(g_frame_mixer[MIXER_OFF_FX_TIME_LO] | (g_frame_mixer[MIXER_OFF_FX_TIME_HI] << 8));
        cur = (uint16_t)(cur + delta);
        g_frame_mixer[MIXER_OFF_FX_TIME_LO] = (uint8_t)(cur & 0xFF);
        g_frame_mixer[MIXER_OFF_FX_TIME_HI] = (uint8_t)((cur >> 8) & 0xFF);
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;
    }

    case K_DEPTH: {
        uint16_t val10 = (uint16_t)(fval * 1023.0f);
        local_set10(g_frame_mixer, MX_REG_BFX_DEPTH, val10);
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;
    }

    case K_BEATPREV:
        if (is_press) {
            if (s_bfx_frac_idx > 0) s_bfx_frac_idx--;
            g_frame_mixer[MIXER_OFF_BTN_ROW1] |= MIXER_MASK_FX_BEAT_REV;
        } else {
            g_frame_mixer[MIXER_OFF_BTN_ROW1] &= ~MIXER_MASK_FX_BEAT_REV;
        }
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;

    case K_BEATNEXT:
        if (is_press) {
            if (s_bfx_frac_idx < 8) s_bfx_frac_idx++;
            g_frame_mixer[MIXER_OFF_BTN_ROW1] |= MIXER_MASK_FX_BEAT_FWD;
        } else {
            g_frame_mixer[MIXER_OFF_BTN_ROW1] &= ~MIXER_MASK_FX_BEAT_FWD;
        }
        send_bus_frame(SUBUCOM_BUS_MIXER, g_frame_mixer);
        break;

    default:
        /* Performance Pads 1..8 */
        if (key >= K_PAD1 && key <= (K_PAD1 + 7)) {
            uint8_t p = (uint8_t)(key - K_PAD1);
            if (is_press) g_frame_erp[ERP_OFF_PADS + d_off] |= (1 << p);
            else          g_frame_erp[ERP_OFF_PADS + d_off] &= ~(1 << p);
            send_bus_frame(SUBUCOM_BUS_ERP, g_frame_erp);
        }
        break;
    }
}

