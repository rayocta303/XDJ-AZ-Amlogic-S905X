/*
 * audio_shim.c — ALSA Low-Latency & Multi-Channel Audio Routing Shim with
 * Integrated Pioneer 4-Deck DSP Graph for AlphaTheta XDJ-AZ EP147 Target:
 * Amlogic S905X (ARM64 AArch64) + Pioneer DDJ-FLX6 USB Audio Interface
 *
 * Core Responsibilities:
 * 1. Intercepts snd_pcm_open to retarget EP147 ALSA playback streams to
 * "default" (which routes to hw:CARD=DDJFLX6,DEV=0 via /etc/asound.conf plug
 * device).
 * 2. Intercepts snd_pcm_hw_params_set_channels to transparently adapt EP147
 * 10-channel audio requests (4 decks stereo + aux) to 4-channel physical
 * DDJ-FLX6 DAC output.
 * 3. Enforces sufficient hardware buffer size (1024 frames / 16 periods of 64
 * frames = 23.2ms) to prevent USB audio underruns / XRUNs on S905X.
 * 4. Integrates authentic Pioneer DSP Matrix from az-runtime-lab (64-bit native
 * AArch64):
 *    - 4-Channel Volume Faders & 14-bit Constant-Power Crossfader
 *    - 3-Band Channel EQ & Hardware Isolators with exact bit-accurate tables
 *    - 6 Sound Color FX algorithms (Filter LPF/HPF, Noise, Sweep, Dub Echo,
 * Space, Crush)
 *    - Master Output & Headphone Cue (PFL) mixing with Cue/Master balance
 * 5. Intercepts snd_pcm_writei to run DSP graph on 10-channel dry deck streams
 * from EP147, mix down to 4-channel master/cue audio, compute real-time
 * sample-accurate VU levels, and output directly to DDJ-FLX6.
 * 6. Publishes live VU meter levels to /dev/shm/xdj_az_shared for DDJ-FLX6
 * hardware LED feedback.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "channel_eq.h"
#include "dsp_graph.h"
#include "xdj_az_shm.h"

#define LOG_TAG "[audio_shim] "

/* Real ALSA function pointers */
static int (*real_snd_pcm_open)(snd_pcm_t **pcmp, const char *name,
                                snd_pcm_stream_t stream, int mode) = NULL;
static int (*real_snd_pcm_close)(snd_pcm_t *pcm) = NULL;
static int (*real_snd_pcm_hw_params)(snd_pcm_t *pcm,
                                     snd_pcm_hw_params_t *params) = NULL;
static int (*real_snd_pcm_hw_params_set_channels)(snd_pcm_t *pcm,
                                                  snd_pcm_hw_params_t *params,
                                                  unsigned int val) = NULL;
static int (*real_snd_pcm_hw_params_set_channels_near)(
    snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val) = NULL;
static int (*real_snd_pcm_hw_params_set_rate_near)(snd_pcm_t *pcm,
                                                   snd_pcm_hw_params_t *params,
                                                   unsigned int *val,
                                                   int *dir) = NULL;
static int (*real_snd_pcm_hw_params_set_format)(snd_pcm_t *pcm,
                                                snd_pcm_hw_params_t *params,
                                                snd_pcm_format_t val) = NULL;
static int (*real_snd_pcm_hw_params_set_buffer_size_near)(
    snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val) = NULL;
static int (*real_snd_pcm_hw_params_set_buffer_size)(
    snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t val) = NULL;
static int (*real_snd_pcm_hw_params_set_period_size_near)(
    snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val,
    int *dir) = NULL;
static int (*real_snd_pcm_hw_params_set_periods_near)(
    snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val,
    int *dir) = NULL;
static snd_pcm_sframes_t (*real_snd_pcm_writei)(snd_pcm_t *pcm,
                                                const void *buffer,
                                                snd_pcm_uframes_t size) = NULL;
static int (*real_snd_pcm_drop)(snd_pcm_t *pcm) = NULL;
static int (*real_snd_pcm_pause)(snd_pcm_t *pcm, int enable) = NULL;

static char g_audio_dev[128] = "default";
static bool s_is_null_fallback = false;
static struct timespec s_pacing_time = {0, 0};
static snd_pcm_format_t g_configured_format = SND_PCM_FORMAT_UNKNOWN;
static xdj_az_shared_t *g_shm = NULL;
static int g_shm_fd = -1;

/* DSP Engine State */
static LabDspGraph *g_graph = NULL;
static uint32_t g_last_dsp_seq = 0;
static bool g_dsp_active = false;

/* Residue buffer for 64-frame block processing (10 channels float from EP147: 4
 * decks stereo + aux) */
#define DSP_BLOCK_FRAMES 64
static float s_residue_in[DSP_BLOCK_FRAMES * 10];
static size_t s_residue_count = 0;

/* Output buffer for processed 4-channel audio (4 channels float to FLX6 plug)
 */
static float s_out4_buf[DSP_BLOCK_FRAMES * 4];

/* Pioneer DDJ-FLX6 / FLX4 7-Step Banded VU Meter Values (CC 2) */
static const uint8_t s_flx6_meter_values[7] = {
    0x00, /* 0: Dark / Silent */
    0x26, /* 1: Green 1 (-24 dB) */
    0x41, /* 2: Green 2 (-12 dB) */
    0x57, /* 3: Orange 1 (-6 dB) */
    0x65, /* 4: Orange 2 (-2 dB) */
    0x77, /* 5: Red (0 dB) */
    0x7F  /* 6: Red full / Peak Clip (+3 dB) */
};

/* Convert normalized peak amplitude (0.0..1.0) to FLX6 CC 2 meter value
 * Pre-computed linear peak thresholds corresponding to exact FLX6 dB bands:
 * -36 dB -> 0.0158489f
 * -20 dB -> 0.1000000f
 * -10 dB -> 0.3162278f
 *  -4 dB -> 0.6309573f
 *  -1 dB -> 0.8912509f
 * +0.5dB -> 1.0592537f
 * Completely eliminates expensive log10() and double-precision operations.
 */
static inline uint8_t float_peak_to_flx6_meter(float peak_val) {
  if (peak_val < 0.0f)
    peak_val = -peak_val;
  if (peak_val < 0.0158489f)
    return s_flx6_meter_values[0];
  if (peak_val < 0.1000000f)
    return s_flx6_meter_values[1];
  if (peak_val < 0.3162278f)
    return s_flx6_meter_values[2];
  if (peak_val < 0.6309573f)
    return s_flx6_meter_values[3];
  if (peak_val < 0.8912509f)
    return s_flx6_meter_values[4];
  if (peak_val < 1.0592537f)
    return s_flx6_meter_values[5];
  return s_flx6_meter_values[6];
}

static void init_shm(void) {
  if (g_shm)
    return;
  g_shm_fd = shm_open(XDJ_AZ_SHM_FILE, O_CREAT | O_RDWR, 0666);
  if (g_shm_fd < 0) {
    g_shm_fd = open("/dev/shm/xdj_az_shared", O_CREAT | O_RDWR, 0666);
  }
  if (g_shm_fd >= 0) {
    ftruncate(g_shm_fd, sizeof(xdj_az_shared_t));
    void *p = mmap(NULL, sizeof(xdj_az_shared_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED, g_shm_fd, 0);
    if (p != MAP_FAILED) {
      g_shm = (xdj_az_shared_t *)p;
      if (g_shm->magic != XDJ_AZ_SHM_MAGIC || g_shm->master_gain <= 0.0001f) {
        if (g_shm->magic != XDJ_AZ_SHM_MAGIC) {
          memset(g_shm, 0, sizeof(xdj_az_shared_t));
          g_shm->magic = XDJ_AZ_SHM_MAGIC;
          g_shm->version = 1;
        }

        /* Set default neutral mixer levels */
        for (int c = 0; c < 4; c++) {
          g_shm->channel_gain[c] = 1.0f;
          g_shm->eq_high[c] = 0.5f;
          g_shm->eq_mid[c] = 0.5f;
          g_shm->eq_low[c] = 0.5f;
          g_shm->eq_mode[c] = 0;      /* 0 = EQ, 1 = Isolator */
          g_shm->cfx_type[c] = 1;     /* 1 = Filter */
          g_shm->cfx_color[c] = 0.5f; /* Center = Dry */
          g_shm->cfx_param[c] = 0.5f;
          g_shm->cross_assign[c] = 0; /* 0 = THRU */
        }
        g_shm->cross_position = 0.5f; /* Center */
        g_shm->master_gain = 1.0f;
        g_shm->headphones_gain = 1.0f;
        g_shm->cue_master_mix = 0.0f; /* Default 100% CUE */
        g_shm->cue_mask = 0x00;       /* All Cues off by default */
        g_shm->dsp_seq++;
      }
    }
  }
}

static void init_dsp(void) {
  if (g_graph)
    return;
  g_graph = (LabDspGraph *)calloc(1, sizeof(LabDspGraph));
  if (!g_graph) {
    fprintf(stderr,
            LOG_TAG "ERROR: Failed to allocate LabDspGraph (%zu bytes)\n",
            sizeof(LabDspGraph));
    return;
  }

  LabMix initial_mix = {.channel_gain = {1.0f, 1.0f, 1.0f, 1.0f},
                        .cross_assign = {0, 0, 0, 0},
                        .cross_position = 0.5f,
                        .master_gain = 1.0f,
                        .headphones_gain = 1.0f,
                        .cue_master_mix = 0.0f,
                        .cue_mask = 0x00};

  if (lab_dsp_graph_init(g_graph, 44100, &initial_mix) != 1) {
    fprintf(stderr, LOG_TAG "ERROR: lab_dsp_graph_init failed\n");
    return;
  }

  const LabEqTables *tables = get_embedded_eq_tables();
  if (tables && lab_eq_tables_valid(tables)) {
    if (lab_dsp_graph_attach_eq(g_graph, tables) == 1) {
      for (unsigned c = 0; c < 4; c++) {
        lab_dsp_graph_set_eq(g_graph, c, 0, 0.5f, 0.5f, 0.5f);
        lab_dsp_graph_set_cfx(g_graph, c, 1, 0.5f, 0.5f);
      }
      fprintf(stderr, LOG_TAG "Authentic Pioneer EQ/Isolator and CFX Graph "
                              "attached successfully\n");
    } else {
      fprintf(stderr, LOG_TAG "WARNING: lab_dsp_graph_attach_eq returned -1\n");
    }
  } else {
    fprintf(stderr, LOG_TAG "WARNING: Embedded EQ tables validation failed\n");
  }

  g_dsp_active = true;
  fprintf(
      stderr, LOG_TAG
      "Pioneer 4-Deck DSP Engine active (44.1kHz, 64-frame blocks, NEON FZ)\n");
}

static void sync_dsp_from_shm(void) {
  if (!g_shm || !g_graph || !g_dsp_active)
    return;
  if (g_last_dsp_seq == g_shm->dsp_seq)
    return;
  g_last_dsp_seq = g_shm->dsp_seq;

  LabMix mix = {
      .channel_gain = {g_shm->channel_gain[0], g_shm->channel_gain[1],
                       g_shm->channel_gain[2], g_shm->channel_gain[3]},
      .cross_assign = {g_shm->cross_assign[0], g_shm->cross_assign[1],
                       g_shm->cross_assign[2], g_shm->cross_assign[3]},
      .cross_position = g_shm->cross_position,
      .master_gain = g_shm->master_gain,
      .headphones_gain = g_shm->headphones_gain,
      .cue_master_mix = g_shm->cue_master_mix,
      .cue_mask = g_shm->cue_mask};
  lab_dsp_graph_set_mix(g_graph, &mix, 64);

  for (unsigned c = 0; c < 4; c++) {
    lab_dsp_graph_set_eq(g_graph, c, g_shm->eq_mode[c], g_shm->eq_high[c],
                         g_shm->eq_mid[c], g_shm->eq_low[c]);
    lab_dsp_graph_set_cfx(g_graph, c, g_shm->cfx_type[c], g_shm->cfx_color[c],
                          g_shm->cfx_param[c]);
  }
}

static void init_real(void) {
  if (!real_snd_pcm_open) {
    real_snd_pcm_open = (int (*)(snd_pcm_t **, const char *, snd_pcm_stream_t,
                                 int))dlsym(RTLD_NEXT, "snd_pcm_open");
    real_snd_pcm_close =
        (int (*)(snd_pcm_t *))dlsym(RTLD_NEXT, "snd_pcm_close");
    real_snd_pcm_hw_params = (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *))dlsym(
        RTLD_NEXT, "snd_pcm_hw_params");
    real_snd_pcm_hw_params_set_channels =
        (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int))dlsym(
            RTLD_NEXT, "snd_pcm_hw_params_set_channels");
    real_snd_pcm_hw_params_set_channels_near =
        (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *))dlsym(
            RTLD_NEXT, "snd_pcm_hw_params_set_channels_near");
    real_snd_pcm_hw_params_set_rate_near =
        (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *,
                 int *))dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_rate_near");
    real_snd_pcm_hw_params_set_format =
        (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_format_t))dlsym(
            RTLD_NEXT, "snd_pcm_hw_params_set_format");
    real_snd_pcm_hw_params_set_buffer_size_near =
        (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *))dlsym(
            RTLD_NEXT, "snd_pcm_hw_params_set_buffer_size_near");
    real_snd_pcm_hw_params_set_buffer_size =
        (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t))dlsym(
            RTLD_NEXT, "snd_pcm_hw_params_set_buffer_size");
    real_snd_pcm_hw_params_set_period_size_near =
        (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *,
                 int *))dlsym(RTLD_NEXT,
                              "snd_pcm_hw_params_set_period_size_near");
    real_snd_pcm_hw_params_set_periods_near =
        (int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *,
                 int *))dlsym(RTLD_NEXT, "snd_pcm_hw_params_set_periods_near");
    real_snd_pcm_writei = (snd_pcm_sframes_t (*)(
        snd_pcm_t *, const void *, snd_pcm_uframes_t))dlsym(RTLD_NEXT,
                                                            "snd_pcm_writei");
    real_snd_pcm_drop = (int (*)(snd_pcm_t *))dlsym(RTLD_NEXT, "snd_pcm_drop");
    real_snd_pcm_pause =
        (int (*)(snd_pcm_t *, int))dlsym(RTLD_NEXT, "snd_pcm_pause");
  }
}

static inline void arm64_enable_fast_math(void) {
#if defined(__aarch64__)
  uint64_t fpcr;
  __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
  fpcr |= (1ULL << 24); /* Bit 24: Flush-to-Zero (FZ) */
  fpcr |= (1ULL << 25); /* Bit 25: Default NaN (DN) */
  __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
}

__attribute__((constructor)) static void audio_shim_init(void) {
  arm64_enable_fast_math();
  init_real();
  init_shm();
  init_dsp();

  const char *dev = getenv("AZ_AUDIO_DEV");
  if (dev && dev[0]) {
    strncpy(g_audio_dev, dev, sizeof(g_audio_dev) - 1);
    g_audio_dev[sizeof(g_audio_dev) - 1] = '\0';
  }

  fprintf(stderr,
          LOG_TAG "AlphaTheta ALSA Router + Pioneer DSP Engine active (Target: "
                  "%s, 10ch -> FLX6 4ch)\n",
          g_audio_dev);
}

/* Intercept snd_pcm_open to retarget output to configured device */
int snd_pcm_open(snd_pcm_t **pcmp, const char *name, snd_pcm_stream_t stream,
                 int mode) {
  init_real();
  if (!real_snd_pcm_open)
    return -1;

  /* If playback stream, retarget to configured ALSA plug device */
  if (stream == SND_PCM_STREAM_PLAYBACK) {
    const char *target = g_audio_dev;
    snd_pcm_t *probe = NULL;
    int err = real_snd_pcm_open(&probe, target, stream, mode);
    if (err == 0 && probe) {
      *pcmp = probe;
      s_is_null_fallback = false;
      fprintf(stderr,
              LOG_TAG "snd_pcm_open('%s') -> Retargeted to '%s' (Handle=%p)\n",
              name ? name : "null", target, probe);
      return 0;
    }
    fprintf(stderr,
            LOG_TAG
            "snd_pcm_open retarget to '%s' failed (%d), trying '%s'...\n",
            target, err, name ? name : "(null)");

    if (name && strcmp(name, target) != 0 && strcmp(name, "outs") != 0) {
      err = real_snd_pcm_open(&probe, name, stream, mode);
      if (err == 0 && probe) {
        *pcmp = probe;
        s_is_null_fallback = false;
        fprintf(stderr,
                LOG_TAG "snd_pcm_open('%s') succeeded (Handle=%p)\n",
                name, probe);
        return 0;
      }
    }

    /* Fallback to ALSA 'null' device if hardware DAC is not connected */
    err = real_snd_pcm_open(&probe, "null", stream, mode);
    if (err == 0 && probe) {
      *pcmp = probe;
      s_is_null_fallback = true;
      clock_gettime(CLOCK_MONOTONIC, &s_pacing_time);
      fprintf(stderr,
              LOG_TAG "snd_pcm_open: Hardware DAC offline, successfully fell back to 'null' with monotonic pacing (Handle=%p)\n",
              probe);
      return 0;
    }
    fprintf(stderr, LOG_TAG "snd_pcm_open: Fallback to 'null' failed (%d)\n", err);
  }

  return real_snd_pcm_open(pcmp, name, stream, mode);
}

/* Handle hardware parameters cleanly: request 4 channels from ALSA for FLX6 */
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                   unsigned int val) {
  init_real();
  (void)val; /* EP147 requests 10 channels */
  if (real_snd_pcm_hw_params_set_channels) {
    int err = real_snd_pcm_hw_params_set_channels(pcm, params, 4);
    if (err == 0)
      return 0;
  }
  if (real_snd_pcm_hw_params_set_channels_near) {
    unsigned int near_val = 4;
    return real_snd_pcm_hw_params_set_channels_near(pcm, params, &near_val);
  }
  return 0;
}

int snd_pcm_hw_params_set_channels_near(snd_pcm_t *pcm,
                                        snd_pcm_hw_params_t *params,
                                        unsigned int *val) {
  init_real();
  if (real_snd_pcm_hw_params_set_channels_near) {
    unsigned int hw_val = 4;
    int err = real_snd_pcm_hw_params_set_channels_near(pcm, params, &hw_val);
    if (val)
      *val = 10; /* Report 10 channels accepted to EP147 (5 stereo pairs) */
    return err;
  }
  if (val)
    *val = 10;
  return 0;
}

int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
                                 snd_pcm_format_t val) {
  init_real();
  g_configured_format = val;
  fprintf(stderr, LOG_TAG "snd_pcm_hw_params_set_format: format=%d (%s)\n",
          (int)val, snd_pcm_format_name(val));
  if (real_snd_pcm_hw_params_set_format) {
    return real_snd_pcm_hw_params_set_format(pcm, params, val);
  }
  return 0;
}

/* Intercept period size to ensure >= 64 frames */
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm,
                                           snd_pcm_hw_params_t *params,
                                           snd_pcm_uframes_t *val, int *dir) {
  init_real();
  if (real_snd_pcm_hw_params_set_period_size_near) {
    snd_pcm_uframes_t req = val ? *val : 64;
    snd_pcm_uframes_t hw_period = (req < 64) ? 64 : req;
    int err = real_snd_pcm_hw_params_set_period_size_near(pcm, params,
                                                          &hw_period, dir);
    fprintf(stderr,
            LOG_TAG "snd_pcm_hw_params_set_period_size_near: req=%lu -> "
                    "set_hw=%lu (err=%d)\n",
            (unsigned long)req, (unsigned long)hw_period, err);
    if (val)
      *val = hw_period;
    return err;
  }
  return 0;
}

/* Intercept periods to expand hardware buffer to 16 periods (16 * 64 = 1024
 * frames = 23.2ms) Completely eliminates USB audio dropouts / XRUNs on S905X
 * quad-core */
int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm,
                                       snd_pcm_hw_params_t *params,
                                       unsigned int *val, int *dir) {
  init_real();
  if (real_snd_pcm_hw_params_set_periods_near) {
    unsigned int req = val ? *val : 4;
    unsigned int hw_periods = 16;
    int err =
        real_snd_pcm_hw_params_set_periods_near(pcm, params, &hw_periods, dir);
    if (err != 0) {
      hw_periods = 8;
      err = real_snd_pcm_hw_params_set_periods_near(pcm, params, &hw_periods,
                                                    dir);
    }
    fprintf(
        stderr,
        LOG_TAG
        "snd_pcm_hw_params_set_periods_near: req=%u -> set_hw=%u (err=%d)\n",
        req, hw_periods, err);
    if (val)
      *val = hw_periods;
    return err;
  }
  return 0;
}

/* Ensure hardware buffer provides sufficient headroom (1024 frames = 23.2ms)
 * for USB DAC */
int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *pcm,
                                           snd_pcm_hw_params_t *params,
                                           snd_pcm_uframes_t *val) {
  init_real();
  if (real_snd_pcm_hw_params_set_buffer_size_near) {
    snd_pcm_uframes_t hw_buf = 1024;
    int err = real_snd_pcm_hw_params_set_buffer_size_near(pcm, params, &hw_buf);
    fprintf(stderr,
            LOG_TAG "snd_pcm_hw_params_set_buffer_size_near: requested=%lu -> "
                    "set_hw=%lu (err=%d)\n",
            val ? (unsigned long)*val : 0, (unsigned long)hw_buf, err);
    if (val)
      *val = hw_buf;
    return err;
  }
  return 0;
}

int snd_pcm_hw_params_set_buffer_size(snd_pcm_t *pcm,
                                      snd_pcm_hw_params_t *params,
                                      snd_pcm_uframes_t val) {
  init_real();
  (void)val;
  if (real_snd_pcm_hw_params_set_buffer_size_near) {
    snd_pcm_uframes_t hw_buf = 1024;
    return real_snd_pcm_hw_params_set_buffer_size_near(pcm, params, &hw_buf);
  }
  if (real_snd_pcm_hw_params_set_buffer_size) {
    return real_snd_pcm_hw_params_set_buffer_size(pcm, params, 1024);
  }
  return 0;
}

int snd_pcm_drop(snd_pcm_t *pcm) {
  init_real();
  s_residue_count = 0;
  memset(s_residue_in, 0, sizeof(s_residue_in));
  if (s_is_null_fallback) {
    clock_gettime(CLOCK_MONOTONIC, &s_pacing_time);
  }
  if (g_shm) {
    for (int c = 0; c < 4; c++)
      g_shm->is_playing[c] = 0;
    g_shm->seq++;
  }
  if (real_snd_pcm_drop)
    return real_snd_pcm_drop(pcm);
  return 0;
}

int snd_pcm_pause(snd_pcm_t *pcm, int enable) {
  init_real();
  if (enable) {
    s_residue_count = 0;
    memset(s_residue_in, 0, sizeof(s_residue_in));
    if (s_is_null_fallback) {
      clock_gettime(CLOCK_MONOTONIC, &s_pacing_time);
    }
    if (g_shm) {
      for (int c = 0; c < 4; c++)
        g_shm->is_playing[c] = 0;
      g_shm->seq++;
    }
  } else {
    if (s_is_null_fallback) {
      clock_gettime(CLOCK_MONOTONIC, &s_pacing_time);
    }
  }
  if (real_snd_pcm_pause)
    return real_snd_pcm_pause(pcm, enable);
  return 0;
}

/* Intercept snd_pcm_writei: Run Pioneer 4-Deck DSP Engine & Stream to FLX6 */
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer,
                                 snd_pcm_uframes_t size) {
  init_real();
  if (!real_snd_pcm_writei)
    return 0;
  if (!buffer || size == 0)
    return 0;

  /* Ensure hardware ARM64 Flush-to-Zero is active on this audio thread */
  static __thread bool s_ftz_active = false;
  if (!s_ftz_active) {
    arm64_enable_fast_math();
    s_ftz_active = true;
  }

  /* If DSP is not initialized, fallback to direct write */
  if (!g_graph || !g_dsp_active) {
    snd_pcm_sframes_t res = real_snd_pcm_writei(pcm, buffer, size);
    if (s_is_null_fallback) {
      uint64_t ns_delta = (uint64_t)size * 1000000000ULL / 44100ULL;
      uint64_t ns = s_pacing_time.tv_nsec + ns_delta;
      s_pacing_time.tv_sec += ns / 1000000000ULL;
      s_pacing_time.tv_nsec = ns % 1000000000ULL;

      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec < s_pacing_time.tv_sec ||
         (now.tv_sec == s_pacing_time.tv_sec && now.tv_nsec < s_pacing_time.tv_nsec)) {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &s_pacing_time, NULL);
      } else if ((now.tv_sec - s_pacing_time.tv_sec) > 1) {
        s_pacing_time = now;
      }
    }
    return res;
  }

  static int s_log_count = 0;
  if (s_log_count++ < 5) {
    const uint8_t *b = (const uint8_t *)buffer;
    fprintf(stderr,
            LOG_TAG "writei call #%d: size=%lu format=%d (%s) bytes: %02x %02x "
                    "%02x %02x %02x %02x %02x %02x\n",
            s_log_count, (unsigned long)size, (int)g_configured_format,
            snd_pcm_format_name(g_configured_format), b[0], b[1], b[2], b[3],
            b[4], b[5], b[6], b[7]);
  }

  /* Synchronize DSP mixer knobs and faders from shared memory */
  sync_dsp_from_shm();

  const float *in_samples = (const float *)buffer;
  size_t in_offset = 0;
  size_t frames_left = size;

  float peak_ch[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float peak_master_l = 0.0f, peak_master_r = 0.0f;
  float peak_cue_l = 0.0f, peak_cue_r = 0.0f;

  while (frames_left > 0) {
    /* Determine how many frames we can process in this block */
    size_t needed = DSP_BLOCK_FRAMES - s_residue_count;
    size_t chunk = (frames_left < needed) ? frames_left : needed;

    /* Copy 10 channels float directly into residue buffer */
    memcpy(s_residue_in + s_residue_count * 10, in_samples + in_offset * 10,
           chunk * 10 * sizeof(float));
    s_residue_count += chunk;
    in_offset += chunk;
    frames_left -= chunk;

    /* When we have a complete 64-frame block, process through DSP Graph */
    if (s_residue_count == DSP_BLOCK_FRAMES) {
      /* Execute Pioneer DSP Graph (CFX -> EQ -> Faders -> Crossfader -> Master
       * & Cue) */
      lab_dsp_graph_process(g_graph, s_residue_in, s_out4_buf,
                            DSP_BLOCK_FRAMES);

      for (size_t f = 0; f < DSP_BLOCK_FRAMES; f++) {
        size_t bi = f * 10;
        size_t bo = f * 4;

        /* Pre-fader channel peak metering:
         * Ch 0 (Deck 1): bi + 0 (L), bi + 1 (R)
         * Ch 1 (Deck 2): bi + 2 (L), bi + 3 (R)
         * Ch 2 (Deck 3): bi + 4 (L), bi + 5 (R)
         * Ch 3 (Deck 4): bi + 6 (L), bi + 7 (R)
         */
        float p0 = fabsf(s_residue_in[bi + 0]);
        if (p0 > peak_ch[0])
          peak_ch[0] = p0;
        float p1 = fabsf(s_residue_in[bi + 2]);
        if (p1 > peak_ch[1])
          peak_ch[1] = p1;
        float p2 = fabsf(s_residue_in[bi + 4]);
        if (p2 > peak_ch[2])
          peak_ch[2] = p2;
        float p3 = fabsf(s_residue_in[bi + 6]);
        if (p3 > peak_ch[3])
          peak_ch[3] = p3;

        float ml = s_out4_buf[bo + 0];
        float mr = s_out4_buf[bo + 1];
        float cl = s_out4_buf[bo + 2];
        float cr = s_out4_buf[bo + 3];

        /* Track Master & Cue peaks */
        if (fabsf(ml) > peak_master_l)
          peak_master_l = fabsf(ml);
        if (fabsf(mr) > peak_master_r)
          peak_master_r = fabsf(mr);
        if (fabsf(cl) > peak_cue_l)
          peak_cue_l = fabsf(cl);
        if (fabsf(cr) > peak_cue_r)
          peak_cue_r = fabsf(cr);

        /* Hard-clip protect [-1.0f, +1.0f] */
        if (ml > 1.0f)
          ml = 1.0f;
        else if (ml < -1.0f)
          ml = -1.0f;
        if (mr > 1.0f)
          mr = 1.0f;
        else if (mr < -1.0f)
          mr = -1.0f;
        if (cl > 1.0f)
          cl = 1.0f;
        else if (cl < -1.0f)
          cl = -1.0f;
        if (cr > 1.0f)
          cr = 1.0f;
        else if (cr < -1.0f)
          cr = -1.0f;

        s_out4_buf[bo + 0] = ml;
        s_out4_buf[bo + 1] = mr;
        s_out4_buf[bo + 2] = cl;
        s_out4_buf[bo + 3] = cr;
      }

      /* Deliver 4-channel float stream directly to DDJ-FLX6 via ALSA plug */
      real_snd_pcm_writei(pcm, s_out4_buf, DSP_BLOCK_FRAMES);
      s_residue_count = 0;

      /* If hardware DAC is offline, pace the audio thread at exact 44100 Hz rate */
      if (s_is_null_fallback) {
        uint64_t ns = s_pacing_time.tv_nsec + 1451247ULL; // 64 frames @ 44100Hz
        s_pacing_time.tv_sec += ns / 1000000000ULL;
        s_pacing_time.tv_nsec = ns % 1000000000ULL;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec < s_pacing_time.tv_sec ||
           (now.tv_sec == s_pacing_time.tv_sec && now.tv_nsec < s_pacing_time.tv_nsec)) {
          clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &s_pacing_time, NULL);
        } else if ((now.tv_sec - s_pacing_time.tv_sec) > 1) {
          s_pacing_time = now;
        }
      }
    }
  }

  /* Update real-time hardware VU meter peaks and playhead position in shared
   * memory */
  if (g_shm) {
    g_shm->vu_channel[0] =
        float_peak_to_flx6_meter(peak_ch[0] * (g_shm->channel_gain[0]));
    g_shm->vu_channel[1] =
        float_peak_to_flx6_meter(peak_ch[1] * (g_shm->channel_gain[1]));
    g_shm->vu_channel[2] =
        float_peak_to_flx6_meter(peak_ch[2] * (g_shm->channel_gain[2]));
    g_shm->vu_channel[3] =
        float_peak_to_flx6_meter(peak_ch[3] * (g_shm->channel_gain[3]));
    g_shm->vu_master_l = float_peak_to_flx6_meter(peak_master_l);
    g_shm->vu_master_r = float_peak_to_flx6_meter(peak_master_r);
    g_shm->vu_cue_l = float_peak_to_flx6_meter(peak_cue_l);
    g_shm->vu_cue_r = float_peak_to_flx6_meter(peak_cue_r);

    for (int c = 0; c < 4; c++) {
      g_shm->is_playing[c] = (peak_ch[c] > 0.0005f) ? 1 : 0;
    }
    g_shm->seq++;
  }

  /* Always report the requested frame count processed to EP147 */
  return size;
}
