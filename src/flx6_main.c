/*
 * flx6_main.c -- Main entry point and worker threads for DDJ-FLX6 Controller Bridge
 * Target: AlphaTheta XDJ-AZ on Amlogic S905X.
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
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sound/asequencer.h>

#include "ctrl_map.h"
#include "flx6_bridge.h"
#include "padmode_zone.h"
#include "rbp_vu.h"
#include "rbp_led.h"
#include "xdj_az_shm.h"

#define UDP_CMD_PORT 9996

static volatile bool g_running = true;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = false;
}

/* Tick heartbeat worker thread (50 Hz / 20ms) */
static void* flx6_tick_worker(void *arg) {
    (void)arg;
    while (g_running) {
        usleep(20000); /* 20 ms */
        if (map_flx6.tick) {
            map_flx6.tick();
        }
    }
    return NULL;
}

/* Dispatch a raw MIDI message into map_flx6 */
static void dispatch_midi_raw(uint8_t status, uint8_t b1, uint8_t b2) {
    struct snd_seq_event ev;
    memset(&ev, 0, sizeof(ev));

    uint8_t type = status & 0xF0;
    uint8_t ch = status & 0x0F;

    if (type == 0x90 || type == 0x80) {
        ev.type = (type == 0x90 && b2 > 0) ? SNDRV_SEQ_EVENT_NOTEON : SNDRV_SEQ_EVENT_NOTEOFF;
        ev.data.note.channel = ch;
        ev.data.note.note = b1;
        ev.data.note.velocity = b2;
    } else if (type == 0xB0) {
        ev.type = SNDRV_SEQ_EVENT_CONTROLLER;
        ev.data.control.channel = ch;
        ev.data.control.param = b1;
        ev.data.control.value = b2;
    } else {
        return;
    }

    if (map_flx6.event) {
        map_flx6.event(&ev);
    }
}

/* External UDP Command Listener */
static void* flx6_cmd_worker(void *arg) {
    (void)arg;
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0) return NULL;

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(UDP_CMD_PORT);

    if (bind(sfd, (struct sockaddr*)&sin, sizeof(sin)) != 0) {
        close(sfd);
        return NULL;
    }

    char buf[128];
    while (g_running) {
        ssize_t n = recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            int deck = 0, val = 0;
            if (strncmp(buf, "MIDI ", 5) == 0) {
                unsigned int s = 0, b1_in = 0, b2_in = 0;
                if (sscanf(buf + 5, "%x %x %x", &s, &b1_in, &b2_in) == 3) {
                    dispatch_midi_raw((uint8_t)s, (uint8_t)b1_in, (uint8_t)b2_in);
                }
            } else if (sscanf(buf, "PLAY %d", &deck) == 1 && deck >= 1 && deck <= 4) {
                dispatch_midi_raw((uint8_t)(0x90 | (deck - 1)), 11, 0x7F);
                usleep(40000);
                dispatch_midi_raw((uint8_t)(0x90 | (deck - 1)), 11, 0x00);
            } else if (sscanf(buf, "CUE %d", &deck) == 1 && deck >= 1 && deck <= 4) {
                dispatch_midi_raw((uint8_t)(0x90 | (deck - 1)), 12, 0x7F);
                usleep(40000);
                dispatch_midi_raw((uint8_t)(0x90 | (deck - 1)), 12, 0x00);
            } else if (sscanf(buf, "TOUCH %d", &deck) == 1 && deck >= 1 && deck <= 4) {
                dispatch_midi_raw((uint8_t)(0x90 | (deck - 1)), 54, 0x7F);
            } else if (sscanf(buf, "RELEASE %d", &deck) == 1 && deck >= 1 && deck <= 4) {
                dispatch_midi_raw((uint8_t)(0x90 | (deck - 1)), 54, 0x00);
            } else if (sscanf(buf, "FADER %d %d", &deck, &val) == 2 && deck >= 1 && deck <= 4) {
                dispatch_midi_raw((uint8_t)(0xB0 | (deck - 1)), 19, (uint8_t)(val & 0x7F));
            }
        }
    }
    close(sfd);
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    printf("[flx6] Initializing DDJ-FLX6 mapping tables...\n");
    ctrl_bindings_reset();
    if (map_flx6.build) {
        map_flx6.build();
    }
    if (map_flx6.startup) {
        map_flx6.startup();
    }

    pthread_t tick_th, vu_th, led_th, cmd_th;
    pthread_create(&tick_th, NULL, flx6_tick_worker, NULL);
    pthread_create(&vu_th,   NULL, vu_thread,        NULL);
    pthread_create(&led_th,  NULL, led_thread,       NULL);
    pthread_create(&cmd_th,  NULL, flx6_cmd_worker,  NULL);

    printf("[flx6] Pioneer DDJ-FLX6 Controller Bridge started.\n");
    printf("[flx6] Architecture: %s map, %d note bindings, %d abs bindings.\n",
           map_flx6.name, note_map_n, abs_map_n);

    while (g_running) {
        glob_t gl;
        int ret = glob("/dev/snd/midi*", 0, NULL, &gl);
        if (ret != 0 || gl.gl_pathc == 0) {
            globfree(&gl);
            usleep(500000);
            continue;
        }

        char dev_path[128];
        strncpy(dev_path, gl.gl_pathv[0], sizeof(dev_path) - 1);
        dev_path[sizeof(dev_path) - 1] = '\0';
        globfree(&gl);

        printf("[flx6] Discovered rawmidi device: %s. Connecting in O_RDWR mode...\n", dev_path);
        int mfd = open(dev_path, O_RDWR | O_NONBLOCK);
        if (mfd < 0) {
            usleep(500000);
            continue;
        }

        az_bridge_set_midi_fd(mfd);
        printf("[flx6] Connected to %s! Processing live events & transmitting MIDI Out.\n", dev_path);

        uint8_t ring_buf[128];
        size_t ring_len = 0;
        struct pollfd pfd;
        pfd.fd = mfd;
        pfd.events = POLLIN;

        while (g_running) {
            int pr = poll(&pfd, 1, 200);
            if (pr < 0) {
                if (errno == EINTR) break;
                perror("[flx6] poll");
                break;
            }
            if (pr == 0) continue;

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                printf("[flx6] MIDI device disconnected. Reconnecting...\n");
                break;
            }

            if (pfd.revents & POLLIN) {
                uint8_t chunk[64];
                ssize_t n = read(mfd, chunk, sizeof(chunk));
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                    break;
                }

                if (ring_len + n <= sizeof(ring_buf)) {
                    memcpy(&ring_buf[ring_len], chunk, n);
                    ring_len += n;
                } else {
                    ring_len = 0;
                }

                while (ring_len >= 3) {
                    if (ring_buf[0] & 0x80) {
                        dispatch_midi_raw(ring_buf[0], ring_buf[1], ring_buf[2]);
                        memmove(&ring_buf[0], &ring_buf[3], ring_len - 3);
                        ring_len -= 3;
                    } else {
                        memmove(&ring_buf[0], &ring_buf[1], ring_len - 1);
                        ring_len--;
                    }
                }
            }
        }

        az_bridge_set_midi_fd(-1);
        close(mfd);
        usleep(500000);
    }

    pthread_join(tick_th, NULL);
    pthread_join(vu_th, NULL);
    pthread_join(led_th, NULL);
    pthread_join(cmd_th, NULL);

    printf("[flx6] Bridge exited cleanly.\n");
    return 0;
}
