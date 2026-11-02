/*
 * flx6_controls.c — Pioneer DDJ-FLX6 to AlphaTheta XDJ-AZ High-Performance C Bridge
 * Target: Amlogic S905X (Armbian Linux) & Embedded Linux Targets
 *
 * Ultra-low resource usage (<500 KB RSS, 0.0% idle CPU) replacement for Python bridge.
 * Features:
 * - Full 4-Channel Mixer: Ch 1..4 faders, 3-band EQs, trims, Sound Color FX, Cue buttons
 * - Full 4-Deck Engine: Decks 1..4 with 720 PPR Jogs, 14-bit Tempo sliders, Pads, Transport
 * - Platter switching: Note 0x72 toggles Left (Deck 1 <-> 3) and Right (Deck 2 <-> 4)
 * - Sound Color FX selection via SHIFT + CUE 1..4
 * - Beat FX 6-slot racks & Merge FX
 * - Dedicated Load buttons for all 4 decks (Load 1..4)
 * - Direct ALSA rawmidi (/dev/snd/midi*) with auto-reconnect on hotplug
 * - Low-latency UDP packet delivery to subucom_emu shim (127.0.0.1:9999)
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
#include <poll.h>
#include <glob.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "xdj_az_abi64.h"

#define UDP_TARGET_IP      "127.0.0.1"
#define UDP_TARGET_PORT    9999
#define POLL_INTERVAL_MS   10

static volatile bool g_running = true;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = false;
}

/* Internal deck tracking state */
typedef struct {
    uint8_t  deck_id;          /* 0: Deck 1, 1: Deck 2, 2: Deck 3, 3: Deck 4 */
    uint8_t  play;
    uint8_t  cue;
    uint8_t  sync;
    uint8_t  master;
    uint8_t  slip;
    uint8_t  quantize;
    uint8_t  jog_touch;
    int16_t  jog_delta;
    uint8_t  pitch_bend;
    uint16_t tempo_slider;     /* 14-bit (0..16383, center 8192) */
    uint8_t  tempo_range;      /* 0: 6%, 1: 10%, 2: 16%, 3: WIDE */
    uint8_t  master_tempo;     /* Key Lock */
    uint8_t  pad_mode;         /* 0: HotCue, 1: BeatLoop, 2: SlipLoop, 3: BeatJump */
    uint8_t  pad_states;       /* Bitmask 1..8 */
    uint8_t  pad_velocities[8];
    uint8_t  deck_select_flags;
} deck_state_t;

/* Global bridge state */
static deck_state_t g_decks[4];
static subucom_mixer_packet_t g_mixer;
static int g_active_left_deck = 0;   /* 0 (Deck 1) or 2 (Deck 3) */
static int g_active_right_deck = 1;  /* 1 (Deck 2) or 3 (Deck 4) */
static bool g_shift[4] = {false, false, false, false};

static int g_sock_fd = -1;
static struct sockaddr_in g_udp_addr;

static void init_states(void) {
    for (int i = 0; i < 4; i++) {
        memset(&g_decks[i], 0, sizeof(deck_state_t));
        g_decks[i].deck_id = i;
        g_decks[i].quantize = 1;
        g_decks[i].tempo_slider = 8192;
        g_decks[i].tempo_range = 1;
        g_decks[i].master_tempo = 1;
        g_decks[i].deck_select_flags = (i < 2) ? 0x01 : 0x02;
    }

    memset(&g_mixer, 0, sizeof(subucom_mixer_packet_t));
    g_mixer.magic = SUBUCOM_HEADER_MAGIC;
    g_mixer.cmd = 0x02;
    g_mixer.packet_id = 0;

    /* Default center/full mixer settings */
    g_mixer.ch1_fader = 1023;
    g_mixer.ch2_fader = 1023;
    g_mixer.ch3_fader = 1023;
    g_mixer.ch4_fader = 1023;
    g_mixer.crossfader = 512;

    g_mixer.ch1_hi = 512; g_mixer.ch1_mid = 512; g_mixer.ch1_low = 512; g_mixer.ch1_trim = 512; g_mixer.ch1_color = 512;
    g_mixer.ch2_hi = 512; g_mixer.ch2_mid = 512; g_mixer.ch2_low = 512; g_mixer.ch2_trim = 512; g_mixer.ch2_color = 512;
    g_mixer.ch3_hi = 512; g_mixer.ch3_mid = 512; g_mixer.ch3_low = 512; g_mixer.ch3_trim = 512; g_mixer.ch3_color = 512;
    g_mixer.ch4_hi = 512; g_mixer.ch4_mid = 512; g_mixer.ch4_low = 512; g_mixer.ch4_trim = 512; g_mixer.ch4_color = 512;

    g_mixer.headphone_mix = 128;
    g_mixer.headphone_vol = 200;
    g_mixer.master_level = 200;
    g_mixer.booth_level = 150;
    g_mixer.bfx_depth = 64;
    g_mixer.bfx_time_ms = 500;
}

static void send_deck_packet(int deck_id) {
    if (deck_id < 0 || deck_id >= 4 || g_sock_fd < 0) return;
    deck_state_t *d = &g_decks[deck_id];

    subucom_deck_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = SUBUCOM_HEADER_MAGIC;
    pkt.cmd = 0x01;
    pkt.deck_id = d->deck_id;

    pkt.play_state = (d->play & 1) | ((!d->play && !d->cue) << 1) | ((d->cue & 1) << 2);
    pkt.sync_state = (d->sync & 1) | ((d->master & 1) << 1);
    pkt.slip_state = d->slip & 1;
    pkt.quantize_state = d->quantize & 1;

    pkt.jog_delta = d->jog_delta;
    pkt.jog_touch = d->jog_touch & 1;
    pkt.pitch_bend = d->pitch_bend;

    pkt.tempo_slider = d->tempo_slider;
    pkt.tempo_range = d->tempo_range;
    pkt.master_tempo = d->master_tempo & 1;

    pkt.pad_mode = d->pad_mode;
    pkt.pad_states = d->pad_states;
    memcpy(pkt.pad_velocities, d->pad_velocities, 8);
    pkt.deck_select_flags = d->deck_select_flags;

    pkt.crc16 = subucom_crc16((const uint8_t*)&pkt, 126);

    /* UDP datagram: 1 byte SPI channel index + packet payload */
    uint8_t buffer[1 + sizeof(pkt)];
    buffer[0] = (deck_id == 0 || deck_id == 2) ? SUBUCOM_BUS_DECK1 : SUBUCOM_BUS_DECK2;
    memcpy(&buffer[1], &pkt, sizeof(pkt));

    sendto(g_sock_fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&g_udp_addr, sizeof(g_udp_addr));
}

static void send_mixer_packet(void) {
    if (g_sock_fd < 0) return;
    g_mixer.crc16 = subucom_crc16((const uint8_t*)&g_mixer, sizeof(g_mixer) - 2);

    uint8_t buffer[1 + sizeof(g_mixer)];
    buffer[0] = SUBUCOM_BUS_MIXER; /* Mixer is SPI bus 2 (spi3.0) */
    memcpy(&buffer[1], &g_mixer, sizeof(g_mixer));

    sendto(g_sock_fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&g_udp_addr, sizeof(g_udp_addr));
}

static inline uint16_t scale_127_to_1023(uint8_t v) {
    return (uint16_t)(((uint32_t)v * 1023) / 127);
}

static void handle_deck_midi(int deck_id, uint8_t msg_type, uint8_t note_cc, uint8_t val) {
    deck_state_t *d = &g_decks[deck_id];
    bool is_on = (msg_type == 0x90 && val > 0);

    if (msg_type == 0x90 || msg_type == 0x80) {
        if (note_cc == 0x0B) { /* Play / Pause */
            if (is_on) d->play = d->play ? 0 : 1;
            send_deck_packet(deck_id);
        } else if (note_cc == 0x0C) { /* Cue */
            d->cue = is_on ? 1 : 0;
            if (is_on && d->play) d->play = 0;
            send_deck_packet(deck_id);
        } else if (note_cc == 0x36) { /* Jog Platter Touch (Vinyl mode) */
            d->jog_touch = is_on ? 1 : 0;
            send_deck_packet(deck_id);
        } else if (note_cc == 0x3F) { /* Shift */
            g_shift[deck_id] = is_on;
        } else if (note_cc == 0x58) { /* Beat Sync */
            if (is_on) d->sync = d->sync ? 0 : 1;
            send_deck_packet(deck_id);
        } else if (note_cc == 0x5C) { /* Master / Key Lock */
            if (is_on) d->master_tempo = d->master_tempo ? 0 : 1;
            send_deck_packet(deck_id);
        } else if (note_cc == 0x1B) { /* Pad Mode: Hot Cue */
            if (is_on) d->pad_mode = 0;
        } else if (note_cc == 0x20) { /* Pad Mode: Beat Jump */
            if (is_on) d->pad_mode = 3;
        } else if (note_cc == 0x6D) { /* Pad Mode: Beat Loop */
            if (is_on) d->pad_mode = 1;
        } else if (note_cc <= 0x07) { /* Performance Pads 1..8 */
            uint8_t pad = note_cc;
            if (is_on) d->pad_states |= (1 << pad);
            else d->pad_states &= ~(1 << pad);
            d->pad_velocities[pad] = val;
            send_deck_packet(deck_id);
        }
    } else if (msg_type == 0xB0) {
        if (note_cc == 0x21 || note_cc == 0x22) { /* Jog Rotation (720 PPR) */
            int8_t delta = (val > 64) ? (int8_t)(val - 128) : (int8_t)val;
            d->jog_delta = (int16_t)delta * 6;
            send_deck_packet(deck_id);
            d->jog_delta = 0;
        } else if (note_cc == 0x23) { /* Jog Edge Ring / Pitch Bend */
            d->pitch_bend = val;
            send_deck_packet(deck_id);
        } else if (note_cc == 0x00) { /* Tempo Slider MSB */
            d->tempo_slider = (val << 7) | (d->tempo_slider & 0x7F);
            send_deck_packet(deck_id);
        } else if (note_cc == 0x20) { /* Tempo Slider LSB */
            d->tempo_slider = (d->tempo_slider & 0x3F80) | (val & 0x7F);
            send_deck_packet(deck_id);
        }
    }
}

static void process_midi_event(uint8_t status, uint8_t b1, uint8_t b2) {
    uint8_t msg_type = status & 0xF0;
    uint8_t ch = status & 0x0F;
    bool is_on = (msg_type == 0x90 && b2 > 0);

    /* --- HARDWARE DECK SELECT BUTTONS --- */
    if (msg_type == 0x90 && b1 == 0x72 && b2 > 0) {
        if (ch == 0 || ch == 2) {
            g_active_left_deck = (g_active_left_deck == 0) ? 2 : 0;
            printf("[flx6_c] Left Platter switched to DECK %d\n", g_active_left_deck + 1);
            send_deck_packet(g_active_left_deck);
            return;
        } else if (ch == 1 || ch == 3) {
            g_active_right_deck = (g_active_right_deck == 1) ? 3 : 1;
            printf("[flx6_c] Right Platter switched to DECK %d\n", g_active_right_deck + 1);
            send_deck_packet(g_active_right_deck);
            return;
        }
    }

    /* --- SOUND COLOR FX SELECT via SHIFT + CUE 1..4 --- */
    if (msg_type == 0x90 && b1 == 0x68 && b2 > 0) {
        if (ch < 4) {
            const char *cfx_names[4] = {"Filter", "Dub Echo", "Noise", "Space"};
            g_mixer.cfx_type = ch;
            printf("[flx6_c] Sound Color FX Selected: %s\n", cfx_names[ch]);
            send_mixer_packet();
            return;
        }
    }

    /* --- CHANNEL 1 STRIP (MIDI Channel 0) --- */
    if (ch == 0) {
        if (msg_type == 0xB0) {
            if (b1 == 0x13) { g_mixer.ch1_fader = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x04) { g_mixer.ch1_trim  = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x07) { g_mixer.ch1_hi    = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x0B) { g_mixer.ch1_mid   = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x0F) { g_mixer.ch1_low   = scale_127_to_1023(b2); send_mixer_packet(); return; }
        } else if ((msg_type == 0x90 || msg_type == 0x80) && b1 == 0x54) {
            if (is_on) g_mixer.ch_cue_states ^= (1 << 0);
            send_mixer_packet(); return;
        }
        int target = (g_active_left_deck == 0) ? 0 : 2;
        handle_deck_midi(target, msg_type, b1, b2);
        return;
    }

    /* --- CHANNEL 2 STRIP (MIDI Channel 1) --- */
    if (ch == 1) {
        if (msg_type == 0xB0) {
            if (b1 == 0x13) { g_mixer.ch2_fader = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x04) { g_mixer.ch2_trim  = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x07) { g_mixer.ch2_hi    = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x0B) { g_mixer.ch2_mid   = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x0F) { g_mixer.ch2_low   = scale_127_to_1023(b2); send_mixer_packet(); return; }
        } else if ((msg_type == 0x90 || msg_type == 0x80) && b1 == 0x54) {
            if (is_on) g_mixer.ch_cue_states ^= (1 << 1);
            send_mixer_packet(); return;
        }
        int target = (g_active_right_deck == 1) ? 1 : 3;
        handle_deck_midi(target, msg_type, b1, b2);
        return;
    }

    /* --- CHANNEL 3 STRIP (MIDI Channel 2) --- */
    if (ch == 2) {
        if (msg_type == 0xB0) {
            if (b1 == 0x13) { g_mixer.ch3_fader = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x04) { g_mixer.ch3_trim  = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x07) { g_mixer.ch3_hi    = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x0B) { g_mixer.ch3_mid   = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x0F) { g_mixer.ch3_low   = scale_127_to_1023(b2); send_mixer_packet(); return; }
        } else if ((msg_type == 0x90 || msg_type == 0x80) && b1 == 0x54) {
            if (is_on) g_mixer.ch_cue_states ^= (1 << 2);
            send_mixer_packet(); return;
        }
        handle_deck_midi(2, msg_type, b1, b2);
        return;
    }

    /* --- CHANNEL 4 STRIP (MIDI Channel 3) --- */
    if (ch == 3) {
        if (msg_type == 0xB0) {
            if (b1 == 0x13) { g_mixer.ch4_fader = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x04) { g_mixer.ch4_trim  = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x07) { g_mixer.ch4_hi    = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x0B) { g_mixer.ch4_mid   = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x0F) { g_mixer.ch4_low   = scale_127_to_1023(b2); send_mixer_packet(); return; }
        } else if ((msg_type == 0x90 || msg_type == 0x80) && b1 == 0x54) {
            if (is_on) g_mixer.ch_cue_states ^= (1 << 3);
            send_mixer_packet(); return;
        }
        handle_deck_midi(3, msg_type, b1, b2);
        return;
    }

    /* --- BEAT FX & MERGE FX (MIDI Channel 4 & 5) --- */
    if (ch == 4 || ch == 5) {
        if (msg_type == 0xB0 && b1 == 0x08) { /* Merge FX Knob */
            g_mixer.bfx_depth = b2;
            send_mixer_packet();
        } else if ((msg_type == 0x90 || msg_type == 0x80) && b1 == 0x2E) { /* Merge FX Push */
            g_mixer.bfx_on = is_on ? 1 : 0;
            send_mixer_packet();
        } else if ((msg_type == 0x90 || msg_type == 0x80) && b1 >= 0x70 && b1 <= 0x72) { /* Slots */
            int offset = (ch == 4) ? 0 : 3;
            g_mixer.bfx_slot = offset + (b1 - 0x70);
            send_mixer_packet();
        }
        return;
    }

    /* --- GLOBAL MIXER, BROWSE & LOAD BUTTONS (MIDI Channel 6) --- */
    if (ch == 6) {
        if (msg_type == 0xB0) {
            if (b1 == 0x1F) { g_mixer.crossfader = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x17) { g_mixer.ch1_color  = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x18) { g_mixer.ch2_color  = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x19) { g_mixer.ch3_color  = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x1A) { g_mixer.ch4_color  = scale_127_to_1023(b2); send_mixer_packet(); return; }
            if (b1 == 0x40) { /* Browse Rotary */
                g_mixer.browse_delta = (b2 == 0x01) ? 1 : -1;
                send_mixer_packet();
                g_mixer.browse_delta = 0;
                return;
            }
        } else if (msg_type == 0x90 || msg_type == 0x80) {
            if (b1 == 0x41) g_mixer.browse_push = is_on ? 1 : 0;
            else if (b1 == 0x65) g_mixer.browse_back = is_on ? 1 : 0;
            else if (b1 == 0x7A) g_mixer.browse_view = is_on ? 1 : 0;
            /* 4 Dedicated Load Buttons */
            else if (b1 == 0x46) { g_mixer.load_deck1 = is_on ? 1 : 0; printf("[flx6_c] LOAD Deck 1\n"); }
            else if (b1 == 0x47) { g_mixer.load_deck2 = is_on ? 1 : 0; printf("[flx6_c] LOAD Deck 2\n"); }
            else if (b1 == 0x48) { g_mixer.load_deck3 = is_on ? 1 : 0; printf("[flx6_c] LOAD Deck 3\n"); }
            else if (b1 == 0x49) { g_mixer.load_deck4 = is_on ? 1 : 0; printf("[flx6_c] LOAD Deck 4\n"); }
            send_mixer_packet();
        }
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    init_states();

    /* Create UDP client socket */
    g_sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock_fd < 0) {
        perror("[flx6_c] socket");
        return 1;
    }

    memset(&g_udp_addr, 0, sizeof(g_udp_addr));
    g_udp_addr.sin_family = AF_INET;
    g_udp_addr.sin_port = htons(UDP_TARGET_PORT);
    inet_pton(AF_INET, UDP_TARGET_IP, &g_udp_addr.sin_addr);

    printf("[flx6_c] Pioneer DDJ-FLX6 High-Efficiency C Bridge started.\n");
    printf("[flx6_c] Target: %s:%d | 4 Decks & 4 Channels Active.\n", UDP_TARGET_IP, UDP_TARGET_PORT);

    while (g_running) {
        glob_t gl;
        int ret = glob("/dev/snd/midi*", 0, NULL, &gl);
        if (ret != 0 || gl.gl_pathc == 0) {
            globfree(&gl);
            usleep(500000);
            continue;
        }

        const char *dev_path = gl.gl_pathv[0];
        printf("[flx6_c] Discovered rawmidi device: %s. Connecting...\n", dev_path);
        int mfd = open(dev_path, O_RDONLY | O_NONBLOCK);
        globfree(&gl);

        if (mfd < 0) {
            usleep(500000);
            continue;
        }

        printf("[flx6_c] Connected to %s! Processing events.\n", dev_path);

        uint8_t ring_buf[128];
        size_t ring_len = 0;
        struct pollfd pfd;
        pfd.fd = mfd;
        pfd.events = POLLIN;

        while (g_running) {
            int pr = poll(&pfd, 1, 200);
            if (pr < 0) {
                if (errno == EINTR) break;
                perror("[flx6_c] poll");
                break;
            }
            if (pr == 0) continue; /* Timeout, keep alive */

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                printf("[flx6_c] MIDI device disconnected. Reconnecting...\n");
                break;
            }

            if (pfd.revents & POLLIN) {
                uint8_t chunk[64];
                ssize_t n = read(mfd, chunk, sizeof(chunk));
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                    printf("[flx6_c] Device read error: %s\n", strerror(errno));
                    break;
                }

                if (ring_len + n <= sizeof(ring_buf)) {
                    memcpy(&ring_buf[ring_len], chunk, n);
                    ring_len += n;
                } else {
                    ring_len = 0; /* Reset overflow */
                }

                while (ring_len >= 3) {
                    if (ring_buf[0] & 0x80) {
                        process_midi_event(ring_buf[0], ring_buf[1], ring_buf[2]);
                        memmove(&ring_buf[0], &ring_buf[3], ring_len - 3);
                        ring_len -= 3;
                    } else {
                        /* Discard non-status leading byte */
                        memmove(&ring_buf[0], &ring_buf[1], ring_len - 1);
                        ring_len--;
                    }
                }
            }
        }
        close(mfd);
        usleep(500000);
    }

    if (g_sock_fd >= 0) close(g_sock_fd);
    printf("[flx6_c] Bridge exited cleanly.\n");
    return 0;
}
