/*
 * touch_shim.c — Hardware Touchscreen & Pointing Emulation Shim for AlphaTheta
 * XDJ-AZ Target: Amlogic S905X (AArch64 ARM64) & Linux Targets
 *
 * Implements:
 * 1. Interception of /dev/input/by-id/gt928 (AlphaTheta EP147
 * KernelInputSubsystem)
 * 2. Device capability and ioctl emulation (EVIOCGNAME, EVIOCGABS, EVIOCGBIT,
 * EVIOCGPROP)
 * 3. Dynamic discovery of physical touchscreens & pointing devices
 * (/dev/input/event*)
 * 4. Coordinate transformation (scaling, swap XY, invert X/Y, optional
 * rotation)
 * 5. Seeding of initial ABS coordinates via EVIOCGABS to avoid (0,0)
 * first-touch drop
 * 6. Multi-Touch Type B protocol synthesis (ABS_MT_SLOT, ABS_MT_TRACKING_ID,
 * etc.)
 * 7. Out-of-band touch injection via /tmp/xdj_touch_inject.sock
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#pragma GCC diagnostic ignored "-Wnonnull-compare"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>


#include "fx_paint.h"
#include "fx_zone.h"
#include "menu_paint.h"


#define TARGET_DEV_NODE "/dev/input/by-id/gt928"
#define LOGICAL_WIDTH 1280
#define LOGICAL_HEIGHT 800
#define INJECT_SOCKET_PATH "/tmp/xdj_touch_inject.sock"
#define LOG_TAG "[touch_shim] "
#define MAX_TOUCH_FDS 16

/* Real libc functions */
static int (*real_open)(const char *pathname, int flags, ...) = NULL;
static int (*real_open64)(const char *pathname, int flags, ...) = NULL;
static int (*real_openat)(int dirfd, const char *pathname, int flags,
                          ...) = NULL;
static int (*real_openat64)(int dirfd, const char *pathname, int flags,
                            ...) = NULL;
static int (*real_close)(int fd) = NULL;
static ssize_t (*real_read)(int fd, void *buf, size_t count) = NULL;
static int (*real_ioctl)(int fd, unsigned long request, ...) = NULL;

static void init_real_functions(void) {
  if (!real_open) {
    real_open = dlsym(RTLD_NEXT, "open");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_openat = dlsym(RTLD_NEXT, "openat");
    real_openat64 = dlsym(RTLD_NEXT, "openat64");
    real_close = dlsym(RTLD_NEXT, "close");
    real_read = dlsym(RTLD_NEXT, "read");
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
  }
}

static int safe_real_open(const char *pathname, int flags) {
  init_real_functions();
  return real_open ? real_open(pathname, flags) : open(pathname, flags);
}

static int safe_real_close(int fd) {
  init_real_functions();
  return real_close ? real_close(fd) : close(fd);
}

static ssize_t safe_real_read(int fd, void *buf, size_t count) {
  init_real_functions();
  return real_read ? real_read(fd, buf, count) : read(fd, buf, count);
}

/* Configuration flags */
static int g_point_kind = 0; /* 0: auto, 1: abs only, 2: rel only, 3: none */
static char g_point_dev[256] = ""; /* explicit /dev/input/eventN if set */
static int g_swap_xy = 0;
static int g_invert_x = 0;
static int g_invert_y = 0;
static int g_rotate_left =
    0; /* Default 0 for normal landscape 1280x800 display */
static int g_debug = 0;

/* Pipe between our reader thread and EP147 */
static int g_touch_pipe[2] = {-1, -1};
static pthread_t g_reader_thread;
static pthread_t g_inject_thread;
static volatile bool g_running = true;
static bool g_threads_started = false;

/* Tracked touch FDs returned to EP147 */
static int g_touch_fds[MAX_TOUCH_FDS] = {0};
static int g_num_touch_fds = 0;
static pthread_mutex_t g_fds_lock = PTHREAD_MUTEX_INITIALIZER;

static void register_touch_fd(int fd) {
  if (fd < 0)
    return;
  pthread_mutex_lock(&g_fds_lock);
  if (g_num_touch_fds < MAX_TOUCH_FDS) {
    g_touch_fds[g_num_touch_fds++] = fd;
  }
  pthread_mutex_unlock(&g_fds_lock);
}

static void unregister_touch_fd(int fd) {
  if (fd < 0)
    return;
  pthread_mutex_lock(&g_fds_lock);
  for (int i = 0; i < g_num_touch_fds; i++) {
    if (g_touch_fds[i] == fd) {
      g_touch_fds[i] = g_touch_fds[g_num_touch_fds - 1];
      g_num_touch_fds--;
      break;
    }
  }
  pthread_mutex_unlock(&g_fds_lock);
}

static bool is_touch_fd(int fd) {
  if (fd < 0)
    return false;
  bool found = false;
  pthread_mutex_lock(&g_fds_lock);
  for (int i = 0; i < g_num_touch_fds; i++) {
    if (g_touch_fds[i] == fd) {
      found = true;
      break;
    }
  }
  pthread_mutex_unlock(&g_fds_lock);
  return found;
}

/* Active pointing state */
static int g_cur_x = LOGICAL_WIDTH / 2;
static int g_cur_y = LOGICAL_HEIGHT / 2;
static int g_cur_touch = 0;
static int g_tracking_id = 0;

static void load_config(void) {
  const char *kind = getenv("POINT_KIND");
  if (kind) {
    if (strcasecmp(kind, "abs") == 0)
      g_point_kind = 1;
    else if (strcasecmp(kind, "rel") == 0)
      g_point_kind = 2;
    else if (strcasecmp(kind, "none") == 0)
      g_point_kind = 3;
    else
      g_point_kind = 0;
  }
  const char *pdev = getenv("POINT_DEV");
  if (pdev && strlen(pdev) > 0) {
    strncpy(g_point_dev, pdev, sizeof(g_point_dev) - 1);
  }
  if (getenv("POINT_SWAP_XY"))
    g_swap_xy = atoi(getenv("POINT_SWAP_XY"));
  if (getenv("POINT_INVERT_X"))
    g_invert_x = atoi(getenv("POINT_INVERT_X"));
  if (getenv("POINT_INVERT_Y"))
    g_invert_y = atoi(getenv("POINT_INVERT_Y"));
  if (getenv("POINT_ROTATE_LEFT"))
    g_rotate_left = atoi(getenv("POINT_ROTATE_LEFT"));
  if (getenv("POINT_DEBUG"))
    g_debug = atoi(getenv("POINT_DEBUG"));
}

/* Emit a complete Linux input_event to the pipe */
static void emit_event(int fd, uint16_t type, uint16_t code, int32_t val) {
  struct input_event ev;
  memset(&ev, 0, sizeof(ev));
  struct timeval tv;
  gettimeofday(&tv, NULL);
  ev.time.tv_sec = tv.tv_sec;
  ev.time.tv_usec = tv.tv_usec;
  ev.type = type;
  ev.code = code;
  ev.value = val;
  write(fd, &ev, sizeof(ev));
}

/* ============================================================================
 * Pioneer XDJ-AZ / RX3 Beat FX Touch & Picker Compositor
 * ============================================================================
 */
static Display *s_x11_dpy = NULL;
static Window s_x11_win = None; /* 14-effect popup picker window */
static GC s_x11_gc = None;
static bool s_picker_open = false;
static unsigned long long s_picker_expire_ms = 0;
static int s_pressed_row = -1;
int s_cur_fx = 0;      /* 0: DELAY */
int s_cur_div_idx = 5; /* 5: "1" Beat */
bool s_fx_power = false;

/* Debounce timing thresholds (milliseconds) */
#define BFX_PICKER_GUARD_MS                                                    \
  200ULL /* Ignore touches immediately after picker opens */
#define BFX_SELECT_DEBOUNCE_MS 250ULL /* Debounce between selecting effects */
#define BFX_ROW_MIN_HOLD_MS                                                    \
  40ULL /* Minimum hold duration to register row press */
#define BFX_HEADER_DEBOUNCE_MS 350ULL /* Debounce for opening picker */
#define BFX_CLOSE_REOPEN_DEBOUNCE_MS                                           \
  300ULL /* Debounce after closing picker before header can re-open */
#define BFX_POWER_DEBOUNCE_MS 250ULL  /* Debounce for toggling FX power */
#define BFX_BEAT_DEBOUNCE_MS 150ULL   /* Debounce for stepping beat division */
#define BFX_QUANTIZE_DEBOUNCE_MS 250ULL /* Debounce for toggling quantize */

/* Debounce state timestamps */
static unsigned long long s_picker_open_time_ms = 0;
static unsigned long long s_picker_close_time_ms = 0;
static unsigned long long s_row_press_start_ms = 0;
static unsigned long long s_last_header_open_ms = 0;
static unsigned long long s_last_select_time_ms = 0;
static unsigned long long s_last_quantize_ms = 0;
static unsigned long long s_last_beat_ms = 0;

static int s_anchor_row = -1;
static unsigned long long s_picker_trigger_start_ms = 0;

static bool s_overlay_visible = true;
static XFontStruct *s_font_bold = NULL;
static pthread_mutex_t s_x11_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *const s_fx_names[14] = {
    "DELAY",  "ECHO",      "PING PONG", "SPIRAL",     "HELIX",
    "REVERB", "FLANGER",   "PHASER",    "FILTER",     "TRANS",
    "ROLL",   "SLIP ROLL", "PITCH",     "VINYL BRAKE"};

static const uint8_t s_bfx_presets[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
#define NUM_BFX_PRESETS 12
static int s_cur_preset_idx = 7; /* Default: preset 10 = 1 beat */

static void send_bfx_cmd(uint8_t cmd, uint8_t val) {
  typedef void (*set_type_fn)(int);
  typedef void (*toggle_power_fn)(void);
  typedef void (*set_ch_fn)(int);
  typedef void (*step_beat_fn)(int);
  typedef void (*toggle_quantize_fn)(void);
  typedef void (*set_fraction_fn)(int);
  static set_type_fn s_set_type = NULL;
  static toggle_power_fn s_toggle_power = NULL;
  static set_ch_fn s_set_ch = NULL;
  static step_beat_fn s_step_beat = NULL;
  static toggle_quantize_fn s_toggle_quantize = NULL;
  static set_fraction_fn s_set_fraction = NULL;
  static bool s_dlsym_done = false;

  if (!s_dlsym_done) {
    s_set_type = (set_type_fn)dlsym(RTLD_DEFAULT, "subucom_set_bfx_type");
    s_toggle_power =
        (toggle_power_fn)dlsym(RTLD_DEFAULT, "subucom_toggle_bfx_power");
    s_set_ch = (set_ch_fn)dlsym(RTLD_DEFAULT, "subucom_set_bfx_channel");
    s_step_beat = (step_beat_fn)dlsym(RTLD_DEFAULT, "subucom_step_beat");
    s_toggle_quantize = (toggle_quantize_fn)dlsym(RTLD_DEFAULT, "subucom_toggle_bfx_quantize");
    s_set_fraction = (set_fraction_fn)dlsym(RTLD_DEFAULT, "subucom_set_bfx_fraction");
    s_dlsym_done = true;
  }

  if (cmd == 1 && s_set_type) {
    s_set_type(val);
    return;
  }
  if (cmd == 2 && s_toggle_power) {
    s_toggle_power();
    return;
  }
  if (cmd == 3 && s_set_ch) {
    s_set_ch(val);
    return;
  }
  if (cmd == 4 && s_step_beat) {
    s_step_beat(1);
    return;
  }
  if (cmd == 5 && s_step_beat) {
    s_step_beat(-1);
    return;
  }
  if (cmd == 6 && s_toggle_quantize) {
    s_toggle_quantize();
    return;
  }
  if (cmd == 8 && s_set_fraction) {
    s_set_fraction(val);
    return;
  }

  /* UDP fallback to 127.0.0.1:9999 */
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s >= 0) {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(9999);
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    uint8_t pkt[3] = {0xBF, cmd, val};
    sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&sin, sizeof(sin));
    close(s);
  }
}

static void send_xpad_cmd(uint8_t region, uint8_t val) {
  typedef void (*set_xpad_fn)(int, int);
  static set_xpad_fn s_set_xpad = NULL;
  static bool s_xpad_done = false;
  if (!s_xpad_done) {
    s_set_xpad = (set_xpad_fn)dlsym(RTLD_DEFAULT, "subucom_set_xpad");
    s_xpad_done = true;
  }
  if (s_set_xpad) {
    s_set_xpad(region, val);
    return;
  }
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s >= 0) {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(9999);
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    uint8_t pkt[4] = {0xBF, 7, region, val};
    sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&sin, sizeof(sin));
    close(s);
  }
}

static void init_x11_picker(void) {
  if (!s_x11_dpy) {
    s_x11_dpy = XOpenDisplay(":0");
    if (!s_x11_dpy)
      return;
  }
  if (s_x11_win != None)
    return;

  int scr = DefaultScreen(s_x11_dpy);
  Window root = RootWindow(s_x11_dpy, scr);
  XSetWindowAttributes attr;
  attr.override_redirect = True;

  /* 14-Effect Popup Picker Window: x=1090, y=85, w=180, h=406 */
  s_x11_win =
      XCreateWindow(s_x11_dpy, root, 1090, 85, 180, 406, 0, CopyFromParent,
                    InputOutput, CopyFromParent, CWOverrideRedirect, &attr);
  s_x11_gc = XCreateGC(s_x11_dpy, s_x11_win, 0, NULL);

  if (!s_font_bold) {
    s_font_bold = XLoadQueryFont(
        s_x11_dpy, "-adobe-helvetica-bold-r-normal--18-*-*-*-*-*-*-*");
    if (!s_font_bold)
      s_font_bold = XLoadQueryFont(s_x11_dpy, "fixed");
  }
}

static void x11_paint_picker(int pressed_row) {
  if (!s_x11_dpy || s_x11_win == None)
    return;

  if (s_font_bold)
    XSetFont(s_x11_dpy, s_x11_gc, s_font_bold->fid);

  XSetForeground(s_x11_dpy, s_x11_gc, 0x202020);
  XFillRectangle(s_x11_dpy, s_x11_win, s_x11_gc, 0, 0, 180, 406);

  for (int i = 0; i < 14; i++) {
    int ry = i * 29;
    unsigned long bg = 0x141414;
    if (i == pressed_row) {
      bg = 0x0070B0; /* Pressed: cyan/blue */
    } else if (i == s_cur_fx) {
      bg = 0x333333; /* Current: active dark grey */
    }
    XSetForeground(s_x11_dpy, s_x11_gc, bg);
    XFillRectangle(s_x11_dpy, s_x11_win, s_x11_gc, 2, ry + 2, 176, 26);

    XSetForeground(s_x11_dpy, s_x11_gc, 0x2C2C2C);
    XDrawRectangle(s_x11_dpy, s_x11_win, s_x11_gc, 2, ry + 2, 175, 25);

    XSetForeground(s_x11_dpy, s_x11_gc, 0xFFFFFF);
    int text_w = s_font_bold ? XTextWidth(s_font_bold, s_fx_names[i],
                                          (int)strlen(s_fx_names[i]))
                             : (int)strlen(s_fx_names[i]) * 8;
    int tx = (180 - text_w) / 2;
    if (tx < 10)
      tx = 10;
    XDrawString(s_x11_dpy, s_x11_win, s_x11_gc, tx, ry + 20, s_fx_names[i],
                (int)strlen(s_fx_names[i]));
  }
  XFlush(s_x11_dpy);
}

static bool is_waveform_screen(Display *dpy, Window root) {
  if (!dpy || root == None)
    return false;

  /* On WAVEFORM screen:
   * Top-left header displays 'WAVEFORM' title.
   * 'WAVEFORM' is 8 letters long:
   * - Letter 'A' at x=50, y=35 has bright white ink (>= 150).
   * - Letter 'M' at x=116..128, y=32..42 has bright white ink (>= 150).
   * On SOURCE, BROWSE, SEARCH, MENU/UTILITY:
   *   The titles are shorter ('SOURCE', 'BROWSE', 'UTILITY'), so region
   *   x=116..128, y=32..42 is dark background (lum < 60).
   */
  XImage *chk = XGetImage(dpy, root, 116, 32, 12, 10, AllPlanes, ZPixmap);
  if (!chk)
    return false;

  int max_m = 0;
  for (int y = 0; y < 10; y++) {
    for (int x = 0; x < 12; x++) {
      unsigned long p = XGetPixel(chk, x, y);
      int r = (p >> 16) & 0xFF;
      int g = (p >> 8) & 0xFF;
      int b = p & 0xFF;
      int lum = (r + g + b) / 3;
      if (lum > max_m)
        max_m = lum;
    }
  }
  XDestroyImage(chk);

  if (max_m < 150)
    return false;

  /* Verify letter 'A' at (50, 35) */
  XImage *chk_a = XGetImage(dpy, root, 50, 35, 1, 1, AllPlanes, ZPixmap);
  if (!chk_a)
    return false;
  unsigned long pa = XGetPixel(chk_a, 0, 0);
  XDestroyImage(chk_a);
  int lum_a = (((pa >> 16) & 0xFF) + ((pa >> 8) & 0xFF) + (pa & 0xFF)) / 3;

  return lum_a >= 150;
}

static void bfx_open_picker(unsigned long long now_ms) {
  pthread_mutex_lock(&s_x11_lock);
  init_x11_picker();
  if (!s_x11_dpy || s_x11_win == None) {
    pthread_mutex_unlock(&s_x11_lock);
    return;
  }

  int scr = DefaultScreen(s_x11_dpy);
  Window root = RootWindow(s_x11_dpy, scr);

  /* Guard: Only open picker when strictly on Waveform screen */
  if (!is_waveform_screen(s_x11_dpy, root)) {
    pthread_mutex_unlock(&s_x11_lock);
    return;
  }

  s_picker_open = true;
  s_picker_open_time_ms = now_ms;
  s_picker_expire_ms = now_ms + 8000ULL; /* 8 second auto dismiss */
  s_pressed_row = -1;
  s_anchor_row = -1;
  s_row_press_start_ms = 0;

  XMapRaised(s_x11_dpy, s_x11_win);
  x11_paint_picker(-1);
  XFlush(s_x11_dpy);
  pthread_mutex_unlock(&s_x11_lock);
  fprintf(stderr, LOG_TAG "Beat FX Selector opened on Waveform screen at t=%llu ms\n", now_ms);
}

static void bfx_close_picker(void) {
  pthread_mutex_lock(&s_x11_lock);
  if (!s_picker_open) {
    pthread_mutex_unlock(&s_x11_lock);
    return;
  }
  s_picker_open = false;
  s_pressed_row = -1;
  s_anchor_row = -1;
  if (s_x11_dpy && s_x11_win != None) {
    XUnmapWindow(s_x11_dpy, s_x11_win);
    XFlush(s_x11_dpy);
  }
  pthread_mutex_unlock(&s_x11_lock);
  fprintf(stderr, LOG_TAG "Beat FX Selector closed\n");
}

static void check_bfx_visibility(void) {
  pthread_mutex_lock(&s_x11_lock);
  if (!s_x11_dpy) {
    s_x11_dpy = XOpenDisplay(":0");
    if (!s_x11_dpy) {
      pthread_mutex_unlock(&s_x11_lock);
      return;
    }
  }

  int scr = DefaultScreen(s_x11_dpy);
  Window root = RootWindow(s_x11_dpy, scr);

  bool on_wf = is_waveform_screen(s_x11_dpy, root);
  if (on_wf != s_overlay_visible) {
    fprintf(stderr, LOG_TAG "View changed: Waveform screen = %d\n", on_wf);
  }
  s_overlay_visible = on_wf;

  /* If user navigated away from Waveform while picker was open, dismiss it */
  if (s_picker_open && !on_wf) {
    s_picker_open = false;
    s_pressed_row = -1;
    s_anchor_row = -1;
    if (s_x11_win != None) {
      XUnmapWindow(s_x11_dpy, s_x11_win);
      XFlush(s_x11_dpy);
    }
  }
  pthread_mutex_unlock(&s_x11_lock);
}

static void *bfx_compositor_thread(void *arg) {
  (void)arg;
  for (int i = 0; i < 20 && g_running; i++) {
    usleep(100000);
  }
  while (g_running) {
    check_bfx_visibility();
    usleep(150000);
  }
  return NULL;
}

static bool bfx_handle_touch(int touched, int lx, int ly) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  unsigned long long now_ms =
      (unsigned long long)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;

  if (s_picker_open) {
    /* Auto-timeout check */
    if (now_ms >= s_picker_expire_ms) {
      bfx_close_picker();
      s_picker_close_time_ms = now_ms;
      return false;
    }

    /* Debounce: guard against touches immediately following picker open */
    if (now_ms < s_picker_open_time_ms + BFX_PICKER_GUARD_MS) {
      return true; /* Swallow touch during guard interval */
    }

    if (touched) {
      s_picker_expire_ms = now_ms + 8000ULL; /* Extend timeout on touch */
      if (lx >= 1090 && lx <= 1270 && ly >= 85 && ly < (85 + 14 * 29)) {
        int row = (ly - 85) / 29;
        if (row >= 0 && row < 14) {
          if (s_anchor_row < 0) {
            /* Initial touch down on a row */
            s_anchor_row = row;
            s_pressed_row = row;
            s_row_press_start_ms = now_ms;
            pthread_mutex_lock(&s_x11_lock);
            x11_paint_picker(s_pressed_row);
            pthread_mutex_unlock(&s_x11_lock);
          } else if (row != s_pressed_row) {
            /* Finger sliding across rows */
            s_pressed_row = row;
            pthread_mutex_lock(&s_x11_lock);
            x11_paint_picker(s_pressed_row);
            pthread_mutex_unlock(&s_x11_lock);
          }
        }
      } else {
        /* Touched outside picker -> dismiss picker with debounce */
        bfx_close_picker();
        s_picker_close_time_ms = now_ms;
        return true;
      }
    } else {
      /* Release event */
      if (s_anchor_row >= 0) {
        int release_row = -1;
        if (lx >= 1090 && lx <= 1270 && ly >= 85 && ly < (85 + 14 * 29)) {
          release_row = (ly - 85) / 29;
        }

        unsigned long long hold_duration = now_ms - s_row_press_start_ms;
        unsigned long long since_last_select = now_ms - s_last_select_time_ms;

        /* Comprehensive Selector Debounce:
         * 1. Minimum hold time passed (filters noise glitches)
         * 2. Debounce interval from previous effect selection
         * 3. Release matches anchor row or current highlighted row
         */
        if (hold_duration >= BFX_ROW_MIN_HOLD_MS &&
            since_last_select >= BFX_SELECT_DEBOUNCE_MS &&
            (release_row == s_anchor_row || release_row == s_pressed_row) &&
            s_pressed_row >= 0 && s_pressed_row < 14) {
          
          s_cur_fx = s_pressed_row;
          s_last_select_time_ms = now_ms;
          fprintf(stderr, LOG_TAG "Beat FX Selected: [%d] %s (hold=%llu ms, debounce ok)\n",
                  s_cur_fx, s_fx_names[s_cur_fx], hold_duration);
          send_bfx_cmd(1, (uint8_t)s_cur_fx);
        } else {
          fprintf(stderr, LOG_TAG "Beat FX Selection ignored/debounced (hold=%llu ms, anchor=%d rel=%d)\n",
                  hold_duration, s_anchor_row, release_row);
        }
      }
      bfx_close_picker();
      s_picker_close_time_ms = now_ms;
    }
    return true; /* Swallow touch while picker is up */
  }

  /* Screen Guard: only intercept when Beat FX panel is visible */
  if (!s_overlay_visible) {
    return false;
  }

  static bool s_picker_trigger_down = false;
  static bool s_quantize_down = false;
  static bool s_beat_left_down = false;
  static bool s_beat_mid_down = false;
  static bool s_beat_right_down = false;
  static bool s_xpad_down = false;

  if (touched) {
    /* 1. Picker Trigger: Header (y=85..118) or Effect Name (y=170..214) */
    if (lx >= 1090 && lx <= 1270 &&
        ((ly >= 85 && ly <= 118) || (ly >= 170 && ly <= 214))) {
      /* Debounce check for opening picker */
      if (now_ms - s_last_header_open_ms >= BFX_HEADER_DEBOUNCE_MS &&
          now_ms - s_picker_close_time_ms >= BFX_CLOSE_REOPEN_DEBOUNCE_MS) {
        s_picker_trigger_down = true;
        s_picker_trigger_start_ms = now_ms;
      }
      return true;
    }

    /* 2. BPM & Quantize box: y=119..169 */
    if (lx >= 1090 && lx <= 1270 && ly >= 119 && ly <= 169) {
      if (now_ms - s_last_quantize_ms >= BFX_QUANTIZE_DEBOUNCE_MS) {
        s_quantize_down = true;
      }
      return true;
    }

    /* 3. Beat Division: y=215..268 */
    if (lx >= 1090 && lx <= 1270 && ly >= 215 && ly <= 268) {
      if (now_ms - s_last_beat_ms >= BFX_BEAT_DEBOUNCE_MS) {
        if (lx <= 1150) {
          s_beat_left_down = true;
        } else if (lx <= 1208) {
          s_beat_mid_down = true;
        } else {
          s_beat_right_down = true;
        }
      }
      return true;
    }

    /* 4. X-PAD Strip: y=373..420 (or while dragging) */
    if ((lx >= 1090 && lx <= 1270 && ly >= 373 && ly <= 420) || s_xpad_down) {
      s_xpad_down = true;
      int off_x = lx - 1090;
      if (off_x < 0) off_x = 0;
      if (off_x > 179) off_x = 179;
      int slot = off_x / 30; /* 0..5 */
      if (slot > 5) slot = 5;
      int region = slot + 1; /* 1..6 */
      int val = (off_x * 127) / 179;
      send_xpad_cmd((uint8_t)region, (uint8_t)val);
      return true;
    }
  } else {
    /* Release events */
    if (s_picker_trigger_down) {
      s_picker_trigger_down = false;
      if (lx >= 1090 && lx <= 1270 &&
          ((ly >= 85 && ly <= 118) || (ly >= 170 && ly <= 214))) {
        unsigned long long hold_ms = now_ms - s_picker_trigger_start_ms;
        if (hold_ms >= 30ULL &&
            now_ms - s_last_header_open_ms >= BFX_HEADER_DEBOUNCE_MS &&
            now_ms - s_picker_close_time_ms >= BFX_CLOSE_REOPEN_DEBOUNCE_MS) {
          s_last_header_open_ms = now_ms;
          bfx_open_picker(now_ms);
        }
      }
      return true;
    }

    if (s_quantize_down) {
      s_quantize_down = false;
      if (lx >= 1090 && lx <= 1270 && ly >= 119 && ly <= 169) {
        s_last_quantize_ms = now_ms;
        send_bfx_cmd(6, 0);
      }
      return true;
    }

    if (s_beat_left_down) {
      s_beat_left_down = false;
      if (lx >= 1090 && lx <= 1150 && ly >= 215 && ly <= 268) {
        s_last_beat_ms = now_ms;
        if (s_cur_preset_idx > 0)
          s_cur_preset_idx--;
        send_bfx_cmd(8, s_bfx_presets[s_cur_preset_idx]);
        send_bfx_cmd(5, 0);
      }
      return true;
    }

    if (s_beat_mid_down) {
      s_beat_mid_down = false;
      if (lx >= 1151 && lx <= 1208 && ly >= 215 && ly <= 268) {
        s_last_beat_ms = now_ms;
        s_cur_preset_idx = 7; /* Preset 10: 1 beat */
        send_bfx_cmd(8, s_bfx_presets[s_cur_preset_idx]);
      }
      return true;
    }

    if (s_beat_right_down) {
      s_beat_right_down = false;
      if (lx >= 1209 && lx <= 1270 && ly >= 215 && ly <= 268) {
        s_last_beat_ms = now_ms;
        if (s_cur_preset_idx < NUM_BFX_PRESETS - 1)
          s_cur_preset_idx++;
        send_bfx_cmd(8, s_bfx_presets[s_cur_preset_idx]);
        send_bfx_cmd(4, 0);
      }
      return true;
    }

    if (s_xpad_down) {
      s_xpad_down = false;
      send_xpad_cmd(0, 0);
      return true;
    }
  }

  return false;
}

static void emit_touch_frame(int fd, int x, int y, int touched) {
  if (fd < 0)
    return;

  int lx = x;
  int ly = y;

  if (g_rotate_left) {
    /* 90-degree left rotation on 1280x800 display */
    int temp = lx;
    lx = LOGICAL_WIDTH - 1 - ly;
    ly = temp;
  }

  if (g_swap_xy) {
    int temp = lx;
    lx = ly;
    ly = temp;
  }
  if (g_invert_x)
    lx = LOGICAL_WIDTH - 1 - lx;
  if (g_invert_y)
    ly = LOGICAL_HEIGHT - 1 - ly;

  if (lx < 0)
    lx = 0;
  if (lx >= LOGICAL_WIDTH)
    lx = LOGICAL_WIDTH - 1;
  if (ly < 0)
    ly = 0;
  if (ly >= LOGICAL_HEIGHT)
    ly = LOGICAL_HEIGHT - 1;

  /* Intercept and handle Beat FX interaction */
  if (bfx_handle_touch(touched, lx, ly)) {
    return;
  }

  emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
  if (touched) {
    if (!g_cur_touch) {
      g_tracking_id++;
      emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, g_tracking_id);
      emit_event(fd, EV_KEY, BTN_TOUCH, 1);
    }
    emit_event(fd, EV_ABS, ABS_MT_POSITION_X, lx);
    emit_event(fd, EV_ABS, ABS_MT_POSITION_Y, ly);
    emit_event(fd, EV_ABS, ABS_X, lx);
    emit_event(fd, EV_ABS, ABS_Y, ly);
  } else {
    if (g_cur_touch) {
      emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
      emit_event(fd, EV_KEY, BTN_TOUCH, 0);
    }
  }
  emit_event(fd, EV_SYN, SYN_REPORT, 0);

  if (g_debug) {
    fprintf(stderr, LOG_TAG "Frame: lx=%d ly=%d touched=%d\n", lx, ly, touched);
  }

  g_cur_x = lx;
  g_cur_y = ly;
  g_cur_touch = touched;
}

/* Find pointing device candidate */
static int discover_pointing_device(char *out_path, size_t max_len,
                                    bool *out_is_abs, int *out_abs_min_x,
                                    int *out_abs_max_x, int *out_abs_min_y,
                                    int *out_abs_max_y) {
  if (g_point_kind == 3)
    return -1; /* Pointing disabled */

  /* If explicit device configured */
  if (strlen(g_point_dev) > 0) {
    int fd = safe_real_open(g_point_dev, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
      strncpy(out_path, g_point_dev, max_len);
      struct input_absinfo abs_x, abs_y;
      if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0 &&
          ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0) {
        *out_is_abs = true;
        *out_abs_min_x = abs_x.minimum;
        *out_abs_max_x = abs_x.maximum ? abs_x.maximum : LOGICAL_WIDTH;
        *out_abs_min_y = abs_y.minimum;
        *out_abs_max_y = abs_y.maximum ? abs_y.maximum : LOGICAL_HEIGHT;
      } else {
        *out_is_abs = false;
      }
      return fd;
    }
  }

  char candidate_path[64];
  for (int i = 0; i < 32; i++) {
    snprintf(candidate_path, sizeof(candidate_path), "/dev/input/event%d", i);
    int fd = safe_real_open(candidate_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      continue;

    char name[128] = "Unknown";
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);

    unsigned long ev_bits[1] = {0};
    unsigned long key_bits[64] = {0};
    unsigned long abs_bits[64] = {0};
    unsigned long rel_bits[64] = {0};

    ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits);

    bool has_touch_key = (key_bits[BTN_TOUCH / (sizeof(long) * 8)] &
                          (1UL << (BTN_TOUCH % (sizeof(long) * 8))));
    bool has_mouse_btn = (key_bits[BTN_LEFT / (sizeof(long) * 8)] &
                          (1UL << (BTN_LEFT % (sizeof(long) * 8))));
    bool has_abs_xy = (abs_bits[ABS_X / (sizeof(long) * 8)] &
                       (1UL << (ABS_X % (sizeof(long) * 8)))) &&
                      (abs_bits[ABS_Y / (sizeof(long) * 8)] &
                       (1UL << (ABS_Y % (sizeof(long) * 8))));
    bool has_rel_xy = (rel_bits[REL_X / (sizeof(long) * 8)] &
                       (1UL << (REL_X % (sizeof(long) * 8)))) &&
                      (rel_bits[REL_Y / (sizeof(long) * 8)] &
                       (1UL << (REL_Y % (sizeof(long) * 8))));

    /* Prefer absolute touchscreen (matches GT928, WCH CTP, Waveshare, Ilitek)
     */
    if (g_point_kind != 2 && (has_touch_key || (ev_bits[0] & (1 << EV_ABS))) &&
        has_abs_xy) {
      strncpy(out_path, candidate_path, max_len);
      *out_is_abs = true;
      struct input_absinfo abs_x, abs_y;
      ioctl(fd, EVIOCGABS(ABS_X), &abs_x);
      ioctl(fd, EVIOCGABS(ABS_Y), &abs_y);
      *out_abs_min_x = abs_x.minimum;
      *out_abs_max_x = abs_x.maximum ? abs_x.maximum : LOGICAL_WIDTH;
      *out_abs_min_y = abs_y.minimum;
      *out_abs_max_y = abs_y.maximum ? abs_y.maximum : LOGICAL_HEIGHT;
      fprintf(stderr,
              LOG_TAG
              "Discovered Absolute Touchscreen: %s (%s) [%d..%d, %d..%d]\n",
              candidate_path, name, *out_abs_min_x, *out_abs_max_x,
              *out_abs_min_y, *out_abs_max_y);
      return fd;
    }

    /* Fallback to relative mouse if allowed */
    if (g_point_kind != 1 && has_mouse_btn && has_rel_xy) {
      strncpy(out_path, candidate_path, max_len);
      *out_is_abs = false;
      fprintf(stderr, LOG_TAG "Discovered Relative Pointer: %s (%s)\n",
              candidate_path, name);
      return fd;
    }

    safe_real_close(fd);
  }
  return -1;
}

/* Background thread reading physical device and writing to EP147 pipe */
static void *touch_reader_thread(void *arg) {
  (void)arg;
  init_real_functions();
  char dev_path[64] = "";
  bool is_abs = true;
  int min_x = 0, max_x = LOGICAL_WIDTH;
  int min_y = 0, max_y = LOGICAL_HEIGHT;
  int dev_fd = -1;

  int cur_rx = LOGICAL_WIDTH / 2;
  int cur_ry = LOGICAL_HEIGHT / 2;
  int cur_btn = 0;
  int cur_slot = 0;
  int slot_tracking[16];
  int slot_x[16];
  int slot_y[16];
  for (int i = 0; i < 16; i++) {
    slot_tracking[i] = -1;
    slot_x[i] = LOGICAL_WIDTH / 2;
    slot_y[i] = LOGICAL_HEIGHT / 2;
  }

  while (g_running) {
    if (dev_fd < 0) {
      dev_fd = discover_pointing_device(dev_path, sizeof(dev_path), &is_abs,
                                        &min_x, &max_x, &min_y, &max_y);
      if (dev_fd < 0) {
        usleep(500000); /* 500 ms retry interval for USB hotplug */
        continue;
      }
      /* Default to 2048 if max reported is zero or invalid */
      if (max_x <= min_x)
        max_x = 2048;
      if (max_y <= min_y)
        max_y = 2048;

      /* Seed initial coordinates */
      if (is_abs) {
        struct input_absinfo seed_x, seed_y;
        if (ioctl(dev_fd, EVIOCGABS(ABS_X), &seed_x) == 0 && seed_x.value > 0)
          cur_rx = seed_x.value;
        if (ioctl(dev_fd, EVIOCGABS(ABS_Y), &seed_y) == 0 && seed_y.value > 0)
          cur_ry = seed_y.value;
      }
    }

    struct input_event ev;
    ssize_t n = safe_real_read(dev_fd, &ev, sizeof(ev));
    if (n != (ssize_t)sizeof(ev)) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
        usleep(5000);
        continue;
      }
      /* Device disconnected */
      fprintf(stderr, LOG_TAG "Device %s disconnected. Re-scanning...\n",
              dev_path);
      safe_real_close(dev_fd);
      dev_fd = -1;
      continue;
    }

    if (is_abs) {
      if (ev.type == EV_ABS) {
        if (ev.code == ABS_MT_SLOT) {
          if (ev.value >= 0 && ev.value < 16)
            cur_slot = ev.value;
        } else if (ev.code == ABS_MT_TRACKING_ID) {
          if (cur_slot >= 0 && cur_slot < 16)
            slot_tracking[cur_slot] = ev.value;
        } else if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) {
          if (cur_slot >= 0 && cur_slot < 16)
            slot_x[cur_slot] = ev.value;
          cur_rx = ev.value;
        } else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) {
          if (cur_slot >= 0 && cur_slot < 16)
            slot_y[cur_slot] = ev.value;
          cur_ry = ev.value;
        }
      } else if (ev.type == EV_KEY &&
                 (ev.code == BTN_TOUCH || ev.code == BTN_LEFT)) {
        cur_btn = ev.value ? 1 : 0;
      } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        /* Check if any multi-touch slot is currently active */
        bool any_slot_down = false;
        int primary_slot = -1;
        for (int s = 0; s < 16; s++) {
          if (slot_tracking[s] >= 0) {
            any_slot_down = true;
            if (primary_slot < 0)
              primary_slot = s;
          }
        }

        /* Master touch state is pressed if BTN_TOUCH is set or any slot is
         * tracked */
        int touched = (cur_btn || any_slot_down) ? 1 : 0;

        /* Use active slot coordinate if slot is tracked */
        if (primary_slot >= 0) {
          cur_rx = slot_x[primary_slot];
          cur_ry = slot_y[primary_slot];
        }

        /* Scale from raw physical range [min..max] to logical [0..1280, 0..800]
         */
        long rx = (long)cur_rx - min_x;
        long ry = (long)cur_ry - min_y;
        long range_x = (long)max_x - min_x + 1;
        long range_y = (long)max_y - min_y + 1;
        if (range_x < 1)
          range_x = 1;
        if (range_y < 1)
          range_y = 1;

        int scaled_x = (int)(rx * LOGICAL_WIDTH / range_x);
        int scaled_y = (int)(ry * LOGICAL_HEIGHT / range_y);

        if (g_debug) {
          fprintf(stderr,
                  LOG_TAG
                  "Event: slot=%d raw=(%d,%d) -> scaled=(%d,%d) touched=%d\n",
                  primary_slot, cur_rx, cur_ry, scaled_x, scaled_y, touched);
        }

        emit_touch_frame(g_touch_pipe[1], scaled_x, scaled_y, touched);
      }
    } else {
      /* Relative Mouse */
      if (ev.type == EV_REL) {
        if (ev.code == REL_X)
          cur_rx += ev.value * 2;
        else if (ev.code == REL_Y)
          cur_ry += ev.value * 2;
        if (cur_rx < 0)
          cur_rx = 0;
        if (cur_rx >= LOGICAL_WIDTH)
          cur_rx = LOGICAL_WIDTH - 1;
        if (cur_ry < 0)
          cur_ry = 0;
        if (cur_ry >= LOGICAL_HEIGHT)
          cur_ry = LOGICAL_HEIGHT - 1;
      } else if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
        cur_btn = ev.value ? 1 : 0;
      } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        emit_touch_frame(g_touch_pipe[1], cur_rx, cur_ry, cur_btn);
      }
    }
  }

  if (dev_fd >= 0)
    safe_real_close(dev_fd);
  return NULL;
}

/* Out-of-band touch injection socket server */
static void *touch_inject_thread(void *arg) {
  (void)arg;
  unlink(INJECT_SOCKET_PATH);
  int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sfd < 0)
    return NULL;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, INJECT_SOCKET_PATH, sizeof(addr.sun_path) - 1);
  bind(sfd, (struct sockaddr *)&addr, sizeof(addr));
  chmod(INJECT_SOCKET_PATH, 0666);

  char buf[64];
  while (g_running) {
    ssize_t n = recv(sfd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
      usleep(10000);
      continue;
    }
    buf[n] = '\0';
    /* Format: "X,Y,TOUCHED\n" e.g. "640,400,1" */
    int ix, iy, itouch;
    if (sscanf(buf, "%d,%d,%d", &ix, &iy, &itouch) == 3) {
      fprintf(stderr, LOG_TAG "Inject touch: x=%d y=%d touched=%d\n", ix, iy, itouch);
      emit_touch_frame(g_touch_pipe[1], ix, iy, itouch);
    }
  }
  close(sfd);
  unlink(INJECT_SOCKET_PATH);
  return NULL;
}

static void ensure_subsystem_initialized(void) {
  init_real_functions();
  if (g_touch_pipe[0] < 0) {
    load_config();
    if (pipe2(g_touch_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
      pipe(g_touch_pipe);
      fcntl(g_touch_pipe[0], F_SETFL, O_NONBLOCK);
      fcntl(g_touch_pipe[1], F_SETFL, O_NONBLOCK);
    }
    if (!g_threads_started) {
      pthread_create(&g_reader_thread, NULL, touch_reader_thread, NULL);
      pthread_create(&g_inject_thread, NULL, touch_inject_thread, NULL);
      pthread_t bfx_th;
      pthread_create(&bfx_th, NULL, bfx_compositor_thread, NULL);
      pthread_detach(bfx_th);
      g_threads_started = true;
      fprintf(
          stderr,
          LOG_TAG
          "Touch subsystem initialized with pipe [%d, %d] and BFX compositor\n",
          g_touch_pipe[0], g_touch_pipe[1]);
    }
  }
}

__attribute__((constructor)) static void touch_shim_ctor(void) {
  XInitThreads();
  init_real_functions();
  ensure_subsystem_initialized();
}

__attribute__((destructor)) static void touch_shim_fini(void) {
  g_running = false;
  if (g_touch_pipe[0] >= 0)
    safe_real_close(g_touch_pipe[0]);
  if (g_touch_pipe[1] >= 0)
    safe_real_close(g_touch_pipe[1]);
}

/* Intercept open / open64 for /dev/input/by-id/gt928 */
int open(const char *pathname, int flags, ...) {
  init_real_functions();
  if (pathname && strcmp(pathname, TARGET_DEV_NODE) == 0) {
    ensure_subsystem_initialized();
    int dup_fd = dup(g_touch_pipe[0]);
    register_touch_fd(dup_fd);
    fprintf(stderr, LOG_TAG "open(%s) -> Returning touch pipe fd %d\n",
            pathname, dup_fd);
    return dup_fd;
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
  if (pathname && strcmp(pathname, TARGET_DEV_NODE) == 0) {
    ensure_subsystem_initialized();
    int dup_fd = dup(g_touch_pipe[0]);
    register_touch_fd(dup_fd);
    fprintf(stderr, LOG_TAG "open64(%s) -> Returning touch pipe fd %d\n",
            pathname, dup_fd);
    return dup_fd;
  }
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
    return real_open64 ? real_open64(pathname, flags, mode)
                       : real_open(pathname, flags, mode);
  }
  return real_open64 ? real_open64(pathname, flags)
                     : real_open(pathname, flags);
}

int openat(int dirfd, const char *pathname, int flags, ...) {
  init_real_functions();
  if (pathname && strcmp(pathname, TARGET_DEV_NODE) == 0) {
    ensure_subsystem_initialized();
    int dup_fd = dup(g_touch_pipe[0]);
    register_touch_fd(dup_fd);
    fprintf(stderr, LOG_TAG "openat(%s) -> Returning touch pipe fd %d\n",
            pathname, dup_fd);
    return dup_fd;
  }
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
    return real_openat ? real_openat(dirfd, pathname, flags, mode) : -1;
  }
  return real_openat ? real_openat(dirfd, pathname, flags) : -1;
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
  init_real_functions();
  if (pathname && strcmp(pathname, TARGET_DEV_NODE) == 0) {
    ensure_subsystem_initialized();
    int dup_fd = dup(g_touch_pipe[0]);
    register_touch_fd(dup_fd);
    fprintf(stderr, LOG_TAG "openat64(%s) -> Returning touch pipe fd %d\n",
            pathname, dup_fd);
    return dup_fd;
  }
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
    return real_openat64
               ? real_openat64(dirfd, pathname, flags, mode)
               : (real_openat ? real_openat(dirfd, pathname, flags, mode) : -1);
  }
  return real_openat64
             ? real_openat64(dirfd, pathname, flags)
             : (real_openat ? real_openat(dirfd, pathname, flags) : -1);
}

int close(int fd) {
  init_real_functions();
  if (is_touch_fd(fd)) {
    unregister_touch_fd(fd);
  }
  return real_close ? real_close(fd) : -1;
}

int ioctl(int fd, unsigned long request, ...) {
  init_real_functions();
  va_list args;
  va_start(args, request);
  void *argp = va_arg(args, void *);
  va_end(args);

  if (is_touch_fd(fd)) {
    if (_IOC_TYPE(request) == 'E') {
      int nr = _IOC_NR(request);
      size_t sz = _IOC_SIZE(request);

      /* EVIOCGVERSION */
      if (request == EVIOCGVERSION) {
        if (argp)
          *(int *)argp = 0x010001;
        return 0;
      }
      /* EVIOCGID */
      if (request == EVIOCGID) {
        struct input_id *id = (struct input_id *)argp;
        if (id) {
          id->bustype = BUS_I2C;
          id->vendor = 0x0416;
          id->product = 0x0928;
          id->version = 0x0100;
        }
        return 0;
      }
      /* EVIOCGNAME */
      if (nr == 0x06) {
        const char *name = "Goodix Capacitive TouchScreen";
        if (argp && sz > 0) {
          strncpy((char *)argp, name, sz - 1);
          ((char *)argp)[sz - 1] = '\0';
          return strlen((char *)argp);
        }
        return 0;
      }
      /* EVIOCGPROP */
      if (nr == 0x09) {
        if (argp && sz > 0) {
          memset(argp, 0, sz);
          unsigned long *props = (unsigned long *)argp;
          props[INPUT_PROP_DIRECT / (sizeof(long) * 8)] |=
              (1UL << (INPUT_PROP_DIRECT % (sizeof(long) * 8)));
        }
        return sz;
      }
      /* EVIOCGBIT */
      if (nr >= 0x20 && nr <= 0x3f) {
        int ev_type = nr - 0x20;
        if (argp && sz > 0) {
          memset(argp, 0, sz);
          unsigned long *bits = (unsigned long *)argp;
          if (ev_type == 0) {
            bits[0] |= (1UL << EV_SYN) | (1UL << EV_KEY) | (1UL << EV_ABS);
          } else if (ev_type == EV_KEY) {
            bits[BTN_TOUCH / (sizeof(long) * 8)] |=
                (1UL << (BTN_TOUCH % (sizeof(long) * 8)));
          } else if (ev_type == EV_ABS) {
            bits[ABS_X / (sizeof(long) * 8)] |=
                (1UL << (ABS_X % (sizeof(long) * 8)));
            bits[ABS_Y / (sizeof(long) * 8)] |=
                (1UL << (ABS_Y % (sizeof(long) * 8)));
            bits[ABS_MT_SLOT / (sizeof(long) * 8)] |=
                (1UL << (ABS_MT_SLOT % (sizeof(long) * 8)));
            bits[ABS_MT_POSITION_X / (sizeof(long) * 8)] |=
                (1UL << (ABS_MT_POSITION_X % (sizeof(long) * 8)));
            bits[ABS_MT_POSITION_Y / (sizeof(long) * 8)] |=
                (1UL << (ABS_MT_POSITION_Y % (sizeof(long) * 8)));
            bits[ABS_MT_TRACKING_ID / (sizeof(long) * 8)] |=
                (1UL << (ABS_MT_TRACKING_ID % (sizeof(long) * 8)));
          }
        }
        return sz;
      }
      /* EVIOCGABS */
      if (nr >= 0x40 && nr <= 0x7f) {
        int axis = nr - 0x40;
        struct input_absinfo *ai = (struct input_absinfo *)argp;
        if (ai) {
          memset(ai, 0, sizeof(*ai));
          if (axis == ABS_X || axis == ABS_MT_POSITION_X) {
            ai->minimum = 0;
            ai->maximum = LOGICAL_WIDTH - 1;
            ai->resolution = 10;
          } else if (axis == ABS_Y || axis == ABS_MT_POSITION_Y) {
            ai->minimum = 0;
            ai->maximum = LOGICAL_HEIGHT - 1;
            ai->resolution = 10;
          } else if (axis == ABS_MT_SLOT) {
            ai->minimum = 0;
            ai->maximum = 9;
          } else if (axis == ABS_MT_TRACKING_ID) {
            ai->minimum = 0;
            ai->maximum = 65535;
          }
        }
        return 0;
      }
    }
    return 0;
  }
  return real_ioctl ? real_ioctl(fd, request, argp) : -1;
}
