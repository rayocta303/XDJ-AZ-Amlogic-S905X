/*
 * touch_shim.c — Hardware Touchscreen & Pointing Emulation Shim for AlphaTheta XDJ-AZ
 * Target: Amlogic S905X (AArch64 ARM64) & Linux Targets
 *
 * Implements:
 * 1. Interception of /dev/input/by-id/gt928 (AlphaTheta EP147 KernelInputSubsystem)
 * 2. Device capability and ioctl emulation (EVIOCGNAME, EVIOCGABS, EVIOCGBIT, EVIOCGPROP)
 * 3. Dynamic discovery of physical touchscreens & pointing devices (/dev/input/event*)
 * 4. Coordinate transformation (scaling, swap XY, invert X/Y, optional rotation)
 * 5. Seeding of initial ABS coordinates via EVIOCGABS to avoid (0,0) first-touch drop
 * 6. Multi-Touch Type B protocol synthesis (ABS_MT_SLOT, ABS_MT_TRACKING_ID, etc.)
 * 7. Out-of-band touch injection via /tmp/xdj_touch_inject.sock
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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <linux/input.h>

#define TARGET_DEV_NODE      "/dev/input/by-id/gt928"
#define LOGICAL_WIDTH        1280
#define LOGICAL_HEIGHT       800
#define INJECT_SOCKET_PATH   "/tmp/xdj_touch_inject.sock"
#define LOG_TAG              "[touch_shim] "
#define MAX_TOUCH_FDS        16

/* Real libc functions */
static int (*real_open)(const char *pathname, int flags, ...) = NULL;
static int (*real_open64)(const char *pathname, int flags, ...) = NULL;
static int (*real_openat)(int dirfd, const char *pathname, int flags, ...) = NULL;
static int (*real_openat64)(int dirfd, const char *pathname, int flags, ...) = NULL;
static int (*real_close)(int fd) = NULL;
static ssize_t (*real_read)(int fd, void *buf, size_t count) = NULL;
static int (*real_ioctl)(int fd, unsigned long request, ...) = NULL;

static void init_real_functions(void) {
    if (!real_open) {
        real_open     = dlsym(RTLD_NEXT, "open");
        real_open64   = dlsym(RTLD_NEXT, "open64");
        real_openat   = dlsym(RTLD_NEXT, "openat");
        real_openat64 = dlsym(RTLD_NEXT, "openat64");
        real_close    = dlsym(RTLD_NEXT, "close");
        real_read     = dlsym(RTLD_NEXT, "read");
        real_ioctl    = dlsym(RTLD_NEXT, "ioctl");
    }
}

/* Configuration flags */
static int g_point_kind = 0;       /* 0: auto, 1: abs only, 2: rel only, 3: none */
static char g_point_dev[256] = ""; /* explicit /dev/input/eventN if set */
static int g_swap_xy = 0;
static int g_invert_x = 0;
static int g_invert_y = 0;
static int g_rotate_left = 0;      /* Default 0 for normal landscape 1280x800 display */
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
    if (fd < 0) return;
    pthread_mutex_lock(&g_fds_lock);
    if (g_num_touch_fds < MAX_TOUCH_FDS) {
        g_touch_fds[g_num_touch_fds++] = fd;
    }
    pthread_mutex_unlock(&g_fds_lock);
}

static void unregister_touch_fd(int fd) {
    if (fd < 0) return;
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
    if (fd < 0) return false;
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
        if (strcasecmp(kind, "abs") == 0) g_point_kind = 1;
        else if (strcasecmp(kind, "rel") == 0) g_point_kind = 2;
        else if (strcasecmp(kind, "none") == 0) g_point_kind = 3;
        else g_point_kind = 0;
    }
    const char *pdev = getenv("POINT_DEV");
    if (pdev && strlen(pdev) > 0) {
        strncpy(g_point_dev, pdev, sizeof(g_point_dev) - 1);
    }
    if (getenv("POINT_SWAP_XY")) g_swap_xy = atoi(getenv("POINT_SWAP_XY"));
    if (getenv("POINT_INVERT_X")) g_invert_x = atoi(getenv("POINT_INVERT_X"));
    if (getenv("POINT_INVERT_Y")) g_invert_y = atoi(getenv("POINT_INVERT_Y"));
    if (getenv("POINT_ROTATE_LEFT")) g_rotate_left = atoi(getenv("POINT_ROTATE_LEFT"));
    if (getenv("POINT_DEBUG")) g_debug = atoi(getenv("POINT_DEBUG"));
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

static void emit_touch_frame(int fd, int x, int y, int touched) {
    if (fd < 0) return;

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
    if (g_invert_x) lx = LOGICAL_WIDTH - 1 - lx;
    if (g_invert_y) ly = LOGICAL_HEIGHT - 1 - ly;

    if (lx < 0) lx = 0;
    if (lx >= LOGICAL_WIDTH) lx = LOGICAL_WIDTH - 1;
    if (ly < 0) ly = 0;
    if (ly >= LOGICAL_HEIGHT) ly = LOGICAL_HEIGHT - 1;

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
static int discover_pointing_device(char *out_path, size_t max_len, bool *out_is_abs,
                                   int *out_abs_min_x, int *out_abs_max_x,
                                   int *out_abs_min_y, int *out_abs_max_y) {
    if (g_point_kind == 3) return -1; /* Pointing disabled */

    /* If explicit device configured */
    if (strlen(g_point_dev) > 0) {
        int fd = real_open(g_point_dev, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            strncpy(out_path, g_point_dev, max_len);
            struct input_absinfo abs_x, abs_y;
            if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0 && ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0) {
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
        int fd = real_open(candidate_path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

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

        bool has_touch_key = (key_bits[BTN_TOUCH / (sizeof(long)*8)] & (1UL << (BTN_TOUCH % (sizeof(long)*8))));
        bool has_mouse_btn = (key_bits[BTN_LEFT / (sizeof(long)*8)] & (1UL << (BTN_LEFT % (sizeof(long)*8))));
        bool has_abs_xy = (abs_bits[ABS_X / (sizeof(long)*8)] & (1UL << (ABS_X % (sizeof(long)*8)))) &&
                          (abs_bits[ABS_Y / (sizeof(long)*8)] & (1UL << (ABS_Y % (sizeof(long)*8))));
        bool has_rel_xy = (rel_bits[REL_X / (sizeof(long)*8)] & (1UL << (REL_X % (sizeof(long)*8)))) &&
                          (rel_bits[REL_Y / (sizeof(long)*8)] & (1UL << (REL_Y % (sizeof(long)*8))));

        /* Prefer absolute touchscreen (matches GT928, WCH CTP, Waveshare, Ilitek) */
        if (g_point_kind != 2 && (has_touch_key || (ev_bits[0] & (1 << EV_ABS))) && has_abs_xy) {
            strncpy(out_path, candidate_path, max_len);
            *out_is_abs = true;
            struct input_absinfo abs_x, abs_y;
            ioctl(fd, EVIOCGABS(ABS_X), &abs_x);
            ioctl(fd, EVIOCGABS(ABS_Y), &abs_y);
            *out_abs_min_x = abs_x.minimum;
            *out_abs_max_x = abs_x.maximum ? abs_x.maximum : LOGICAL_WIDTH;
            *out_abs_min_y = abs_y.minimum;
            *out_abs_max_y = abs_y.maximum ? abs_y.maximum : LOGICAL_HEIGHT;
            fprintf(stderr, LOG_TAG "Discovered Absolute Touchscreen: %s (%s) [%d..%d, %d..%d]\n",
                    candidate_path, name, *out_abs_min_x, *out_abs_max_x, *out_abs_min_y, *out_abs_max_y);
            return fd;
        }

        /* Fallback to relative mouse if allowed */
        if (g_point_kind != 1 && has_mouse_btn && has_rel_xy) {
            strncpy(out_path, candidate_path, max_len);
            *out_is_abs = false;
            fprintf(stderr, LOG_TAG "Discovered Relative Pointer: %s (%s)\n", candidate_path, name);
            return fd;
        }

        real_close(fd);
    }
    return -1;
}

/* Background thread reading physical device and writing to EP147 pipe */
static void *touch_reader_thread(void *arg) {
    (void)arg;
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
            dev_fd = discover_pointing_device(dev_path, sizeof(dev_path), &is_abs, &min_x, &max_x, &min_y, &max_y);
            if (dev_fd < 0) {
                usleep(500000); /* 500 ms retry interval for USB hotplug */
                continue;
            }
            /* Default to 2048 if max reported is zero or invalid */
            if (max_x <= min_x) max_x = 2048;
            if (max_y <= min_y) max_y = 2048;

            /* Seed initial coordinates */
            if (is_abs) {
                struct input_absinfo seed_x, seed_y;
                if (ioctl(dev_fd, EVIOCGABS(ABS_X), &seed_x) == 0 && seed_x.value > 0) cur_rx = seed_x.value;
                if (ioctl(dev_fd, EVIOCGABS(ABS_Y), &seed_y) == 0 && seed_y.value > 0) cur_ry = seed_y.value;
            }
        }

        struct input_event ev;
        ssize_t n = real_read(dev_fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                usleep(5000);
                continue;
            }
            /* Device disconnected */
            fprintf(stderr, LOG_TAG "Device %s disconnected. Re-scanning...\n", dev_path);
            real_close(dev_fd);
            dev_fd = -1;
            continue;
        }

        if (is_abs) {
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_MT_SLOT) {
                    if (ev.value >= 0 && ev.value < 16) cur_slot = ev.value;
                } else if (ev.code == ABS_MT_TRACKING_ID) {
                    if (cur_slot >= 0 && cur_slot < 16) slot_tracking[cur_slot] = ev.value;
                } else if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) {
                    if (cur_slot >= 0 && cur_slot < 16) slot_x[cur_slot] = ev.value;
                    cur_rx = ev.value;
                } else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) {
                    if (cur_slot >= 0 && cur_slot < 16) slot_y[cur_slot] = ev.value;
                    cur_ry = ev.value;
                }
            } else if (ev.type == EV_KEY && (ev.code == BTN_TOUCH || ev.code == BTN_LEFT)) {
                cur_btn = ev.value ? 1 : 0;
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                /* Check if any multi-touch slot is currently active */
                bool any_slot_down = false;
                int primary_slot = -1;
                for (int s = 0; s < 16; s++) {
                    if (slot_tracking[s] >= 0) {
                        any_slot_down = true;
                        if (primary_slot < 0) primary_slot = s;
                    }
                }

                /* Master touch state is pressed if BTN_TOUCH is set or any slot is tracked */
                int touched = (cur_btn || any_slot_down) ? 1 : 0;

                /* Use active slot coordinate if slot is tracked */
                if (primary_slot >= 0) {
                    cur_rx = slot_x[primary_slot];
                    cur_ry = slot_y[primary_slot];
                }

                /* Scale from raw physical range [min..max] to logical [0..1280, 0..800] */
                long rx = (long)cur_rx - min_x;
                long ry = (long)cur_ry - min_y;
                long range_x = (long)max_x - min_x + 1;
                long range_y = (long)max_y - min_y + 1;
                if (range_x < 1) range_x = 1;
                if (range_y < 1) range_y = 1;

                int scaled_x = (int)(rx * LOGICAL_WIDTH / range_x);
                int scaled_y = (int)(ry * LOGICAL_HEIGHT / range_y);

                if (g_debug) {
                    fprintf(stderr, LOG_TAG "Event: slot=%d raw=(%d,%d) -> scaled=(%d,%d) touched=%d\n",
                            primary_slot, cur_rx, cur_ry, scaled_x, scaled_y, touched);
                }

                emit_touch_frame(g_touch_pipe[1], scaled_x, scaled_y, touched);
            }
        } else {
            /* Relative Mouse */
            if (ev.type == EV_REL) {
                if (ev.code == REL_X) cur_rx += ev.value * 2;
                else if (ev.code == REL_Y) cur_ry += ev.value * 2;
                if (cur_rx < 0) cur_rx = 0;
                if (cur_rx >= LOGICAL_WIDTH) cur_rx = LOGICAL_WIDTH - 1;
                if (cur_ry < 0) cur_ry = 0;
                if (cur_ry >= LOGICAL_HEIGHT) cur_ry = LOGICAL_HEIGHT - 1;
            } else if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
                cur_btn = ev.value ? 1 : 0;
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                emit_touch_frame(g_touch_pipe[1], cur_rx, cur_ry, cur_btn);
            }
        }
    }

    if (dev_fd >= 0) real_close(dev_fd);
    return NULL;
}

/* Out-of-band touch injection socket server */
static void *touch_inject_thread(void *arg) {
    (void)arg;
    unlink(INJECT_SOCKET_PATH);
    int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sfd < 0) return NULL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, INJECT_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    bind(sfd, (struct sockaddr*)&addr, sizeof(addr));
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
            emit_touch_frame(g_touch_pipe[1], ix, iy, itouch);
        }
    }
    close(sfd);
    unlink(INJECT_SOCKET_PATH);
    return NULL;
}

static void ensure_subsystem_initialized(void) {
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
            g_threads_started = true;
            fprintf(stderr, LOG_TAG "Touch subsystem initialized with pipe [%d, %d]\n",
                    g_touch_pipe[0], g_touch_pipe[1]);
        }
    }
}

__attribute__((destructor))
static void touch_shim_fini(void) {
    g_running = false;
    if (g_touch_pipe[0] >= 0) real_close(g_touch_pipe[0]);
    if (g_touch_pipe[1] >= 0) real_close(g_touch_pipe[1]);
}

/* Intercept open / open64 for /dev/input/by-id/gt928 */
int open(const char *pathname, int flags, ...) {
    init_real_functions();
    if (pathname && strcmp(pathname, TARGET_DEV_NODE) == 0) {
        ensure_subsystem_initialized();
        int dup_fd = dup(g_touch_pipe[0]);
        register_touch_fd(dup_fd);
        fprintf(stderr, LOG_TAG "open(%s) -> Returning touch pipe fd %d\n", pathname, dup_fd);
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
        fprintf(stderr, LOG_TAG "open64(%s) -> Returning touch pipe fd %d\n", pathname, dup_fd);
        return dup_fd;
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

int openat(int dirfd, const char *pathname, int flags, ...) {
    init_real_functions();
    if (pathname && strcmp(pathname, TARGET_DEV_NODE) == 0) {
        ensure_subsystem_initialized();
        int dup_fd = dup(g_touch_pipe[0]);
        register_touch_fd(dup_fd);
        fprintf(stderr, LOG_TAG "openat(%s) -> Returning touch pipe fd %d\n", pathname, dup_fd);
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
        fprintf(stderr, LOG_TAG "openat64(%s) -> Returning touch pipe fd %d\n", pathname, dup_fd);
        return dup_fd;
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
        return real_openat64 ? real_openat64(dirfd, pathname, flags, mode) : (real_openat ? real_openat(dirfd, pathname, flags, mode) : -1);
    }
    return real_openat64 ? real_openat64(dirfd, pathname, flags) : (real_openat ? real_openat(dirfd, pathname, flags) : -1);
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
                if (argp) *(int *)argp = 0x010001;
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
                    props[INPUT_PROP_DIRECT / (sizeof(long)*8)] |= (1UL << (INPUT_PROP_DIRECT % (sizeof(long)*8)));
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
                        bits[BTN_TOUCH / (sizeof(long)*8)] |= (1UL << (BTN_TOUCH % (sizeof(long)*8)));
                    } else if (ev_type == EV_ABS) {
                        bits[ABS_X / (sizeof(long)*8)] |= (1UL << (ABS_X % (sizeof(long)*8)));
                        bits[ABS_Y / (sizeof(long)*8)] |= (1UL << (ABS_Y % (sizeof(long)*8)));
                        bits[ABS_MT_SLOT / (sizeof(long)*8)] |= (1UL << (ABS_MT_SLOT % (sizeof(long)*8)));
                        bits[ABS_MT_POSITION_X / (sizeof(long)*8)] |= (1UL << (ABS_MT_POSITION_X % (sizeof(long)*8)));
                        bits[ABS_MT_POSITION_Y / (sizeof(long)*8)] |= (1UL << (ABS_MT_POSITION_Y % (sizeof(long)*8)));
                        bits[ABS_MT_TRACKING_ID / (sizeof(long)*8)] |= (1UL << (ABS_MT_TRACKING_ID % (sizeof(long)*8)));
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
