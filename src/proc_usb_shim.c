/*
 * proc_usb_shim.c — /proc/udev_* Mount Redirection & Hardware Sysfs Shim
 * Target: Amlogic S905X (ARM64 AArch64) & Linux Targets
 *
 * Implements:
 * 1. Isolated USB1 connection & mount sequence ("connect," then "mount /media/usb/sda1 ...")
 *    preventing the "USB Error. Remove the device." modal dialog.
 * 2. Full openat, openat64, access, faccessat, stat, fstatat redirection for /proc/udev_*
 *    (udev_usb1, udev_usb2, udev_usbctn1, udev_usbg1, udev_sd1, udev_nfs).
 * 3. Neutralization of /proc/udev_usbctn1 (discards wlanbterr / caution notifications).
 * 4. Virtualization of Rockchip vsync_time (/sys/module/rockchipdrm/parameters/vsync_time)
 *    using monotonic timestamp pipes to pace EP147 rendering loops.
 * 5. Virtualization of thermal sensor (/sys/class/thermal/thermal_zone0/temp)
 *    reporting steady 45.0°C (45000) to satisfy OverheatingWatcher.
 * 6. Stubs for USB storage bind/unbind sysfs writes and backlight writes.
 * 7. Dummy redirection for missing peripheral devices (/dev/hidg0, /dev/gpiodrv).
 * 8. Implementation of posix_fallocate / posix_fallocate64 for sparse files.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#pragma GCC diagnostic ignored "-Wnonnull-compare"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

#define PROC_USB1_ORIG     "/proc/udev_usb1"
#define PROC_USB2_ORIG     "/proc/udev_usb2"
#define PROC_USBG1_ORIG    "/proc/udev_usbg1"
#define PROC_USBCTN1_ORIG  "/proc/udev_usbctn1"
#define PROC_SD1_ORIG      "/proc/udev_sd1"
#define PROC_NFS_ORIG      "/proc/udev_nfs"

#define PROC_REDIRECT_DIR  "/tmp/xdj_az_proc"
#define PROC_USB1_REDIR    "/tmp/xdj_az_proc/udev_usb1"
#define PROC_USB2_REDIR    "/tmp/xdj_az_proc/udev_usb2"
#define PROC_USBG1_REDIR   "/tmp/xdj_az_proc/udev_usbg1"
#define PROC_USBCTN1_REDIR "/tmp/xdj_az_proc/udev_usbctn1"
#define PROC_SD1_REDIR     "/tmp/xdj_az_proc/udev_sd1"
#define PROC_NFS_REDIR     "/tmp/xdj_az_proc/udev_nfs"
#define PROC_THERMAL_TEMP  "/tmp/xdj_az_proc/thermal_temp"

#define LOG_TAG "[proc_usb_shim] "

static int (*real_open)(const char *pathname, int flags, ...) = NULL;
static int (*real_open64)(const char *pathname, int flags, ...) = NULL;
static int (*real_openat)(int dirfd, const char *pathname, int flags, ...) = NULL;
static int (*real_openat64)(int dirfd, const char *pathname, int flags, ...) = NULL;
static FILE* (*real_fopen)(const char *pathname, const char *mode) = NULL;
static FILE* (*real_fopen64)(const char *pathname, const char *mode) = NULL;
static void* (*real_mmap)(void*, size_t, int, int, int, off_t) = NULL;
static void* (*real_mmap64)(void*, size_t, int, int, int, off_t) = NULL;
static int (*real_access)(const char *pathname, int mode) = NULL;
static int (*real_faccessat)(int dirfd, const char *pathname, int mode, int flags) = NULL;
static int (*real_stat)(const char *pathname, struct stat *statbuf) = NULL;
static int (*real_stat64)(const char *pathname, struct stat64 *statbuf) = NULL;
static int (*real_fstatat)(int dirfd, const char *pathname, struct stat *statbuf, int flags) = NULL;
static int (*real_fstatat64)(int dirfd, const char *pathname, struct stat64 *statbuf, int flags) = NULL;

static atomic_uint g_usb_event_count = 0;

static void init_real(void) {
    if (!real_open) {
        real_open      = dlsym(RTLD_NEXT, "open");
        real_open64    = dlsym(RTLD_NEXT, "open64");
        real_openat    = dlsym(RTLD_NEXT, "openat");
        real_openat64  = dlsym(RTLD_NEXT, "openat64");
        real_fopen     = dlsym(RTLD_NEXT, "fopen");
        real_fopen64   = dlsym(RTLD_NEXT, "fopen64");
        real_mmap      = dlsym(RTLD_NEXT, "mmap");
        real_mmap64    = dlsym(RTLD_NEXT, "mmap64");
        real_access    = dlsym(RTLD_NEXT, "access");
        real_faccessat = dlsym(RTLD_NEXT, "faccessat");
        real_stat      = dlsym(RTLD_NEXT, "stat");
        real_stat64    = dlsym(RTLD_NEXT, "stat64");
        real_fstatat   = dlsym(RTLD_NEXT, "fstatat");
        real_fstatat64 = dlsym(RTLD_NEXT, "fstatat64");
    }
}

static inline bool is_proc_udev(const char *path) {
    if (!path) return false;
    return (strstr(path, "/proc/udev_") != NULL || strstr(path, "xdj_az_proc/udev_") != NULL);
}

static const char* redirect_proc_path(const char *path) {
    if (!path) return path;
    if (strcmp(path, PROC_USB1_ORIG) == 0) return PROC_USB1_REDIR;
    if (strcmp(path, PROC_USB2_ORIG) == 0) return PROC_USB2_REDIR;
    if (strcmp(path, PROC_USBG1_ORIG) == 0) return PROC_USBG1_REDIR;
    if (strcmp(path, PROC_USBCTN1_ORIG) == 0) return PROC_USBCTN1_REDIR;
    if (strcmp(path, PROC_SD1_ORIG) == 0) return PROC_SD1_REDIR;
    if (strcmp(path, PROC_NFS_ORIG) == 0) return PROC_NFS_REDIR;
    return path;
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

/* In-memory binary patch: bypass caution 43 ("USB Error. Remove the device.") */
static void patch_single_site(uintptr_t patch_addr, uint32_t expected_inst, uint32_t new_inst, const char *label) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    uintptr_t page_base = patch_addr & ~(page_size - 1);

    volatile uint32_t *target = (volatile uint32_t *)patch_addr;
    if (*target == expected_inst) {
        if (mprotect((void *)page_base, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            *target = new_inst;
            __builtin___clear_cache((char *)patch_addr, (char *)(patch_addr + 4));
            mprotect((void *)page_base, page_size, PROT_READ | PROT_EXEC);
            fprintf(stderr, LOG_TAG "Caution 43 bypass applied at 0x%lx (%s)\n", (unsigned long)patch_addr, label);
        } else {
            fprintf(stderr, LOG_TAG "ERROR: mprotect failed at 0x%lx: %s\n", (unsigned long)page_base, strerror(errno));
        }
    } else if (*target == new_inst) {
        fprintf(stderr, LOG_TAG "Caution 43 bypass already active at 0x%lx (%s)\n", (unsigned long)patch_addr, label);
    }
}

/* Universal Caution 43 dialog hook: intercepts central JUCE caution popup dispatcher */
static void hook_caution_dispatcher_universal(void) {
    uintptr_t func_addr = 0x249efe0;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    // Allocate executable trampoline page
    uint32_t *tramp = (uint32_t *)mmap(NULL, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        fprintf(stderr, LOG_TAG "ERROR: mmap failed for caution hook trampoline: %s\n", strerror(errno));
        return;
    }

    /*
     * Trampoline layout (AArch64):
     * 0:  7100ac5f   cmp  w2, #0x2b     ; Check if caution_id == 43 (USB Error)
     * 1:  54000041   b.ne +8            ; if not equal, jump to [3] (original logic)
     * 2:  d65f03c0   ret                ; if equal (caution 43): immediately return x0!
     * 3:  a9ba7bfd   stp  x29, x30, [sp, #-96]! ; original inst 1
     * 4:  7100007f   cmp  w3, #0x0              ; original inst 2
     * 5:  910003fd   mov  x29, sp               ; original inst 3
     * 6:  a90153f3   stp  x19, x20, [sp, #16]   ; original inst 4
     * 7:  58000050   ldr  x16, [pc, #8]         ; load target address from [9..10]
     * 8:  d61f0200   br   x16                   ; jump to original function + 16
     * 9:  uint32_t   low 32-bit of 0x249eff0
     * 10: uint32_t   high 32-bit of 0x249eff0
     */
    uint64_t resume_addr = func_addr + 16; // 0x249eff0
    tramp[0] = 0x7100ac5f; // cmp w2, #0x2b
    tramp[1] = 0x54000041; // b.ne +8 (to index 3)
    tramp[2] = 0xd65f03c0; // ret
    tramp[3] = 0xa9ba7bfd; // stp x29, x30, [sp, #-96]!
    tramp[4] = 0x7100007f; // cmp w3, #0x0
    tramp[5] = 0x910003fd; // mov x29, sp
    tramp[6] = 0xa90153f3; // stp x19, x20, [sp, #16]
    tramp[7] = 0x58000050; // ldr x16, [pc, #8]
    tramp[8] = 0xd61f0200; // br x16
    tramp[9] = (uint32_t)(resume_addr & 0xffffffff);
    tramp[10] = (uint32_t)(resume_addr >> 32);

    __builtin___clear_cache((char *)tramp, (char *)(tramp + 12));

    // Hook 0x249efe0 entry point:
    uintptr_t page_base = func_addr & ~(page_size - 1);
    if (mprotect((void *)page_base, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        volatile uint32_t *target = (volatile uint32_t *)func_addr;
        uint64_t tramp_addr = (uint64_t)(uintptr_t)tramp;
        target[0] = 0x58000050; // ldr x16, [pc, #8]
        target[1] = 0xd61f0200; // br x16
        target[2] = (uint32_t)(tramp_addr & 0xffffffff);
        target[3] = (uint32_t)(tramp_addr >> 32);

        __builtin___clear_cache((char *)func_addr, (char *)(func_addr + 16));
        mprotect((void *)page_base, page_size, PROT_READ | PROT_EXEC);
        fprintf(stderr, LOG_TAG "Universal Caution 43 interceptor active at 0x%lx -> trampoline %p\n",
                (unsigned long)func_addr, (void *)tramp);
    } else {
        fprintf(stderr, LOG_TAG "ERROR: mprotect failed at 0x%lx: %s\n", (unsigned long)page_base, strerror(errno));
    }
}

static void patch_caution_gate(void) {
    /* Primary trigger sites: Error 0x10000ab branch & Caution 43 emitter call */
    patch_single_site(0x1de0848, 0x941af39a, 0xd503201f, "Source View Caution 43 addAndMakeVisible -> NOP");
    patch_single_site(0x244163c, 0x54001080, 0xd503201f, "0x10000ab branch -> NOP");
    patch_single_site(0x2441860, 0x97fb4ae8, 0x17ffff9b, "Caution 43 emitter -> skip");
    /* USB Slot 2 (0x25bf series) */
    patch_single_site(0x25bf848, 0x54000120, 0x14000009, "Slot 2A");
    patch_single_site(0x25bf8a8, 0x54000120, 0x14000009, "Slot 2B");
    patch_single_site(0x25bfb3c, 0x54000120, 0x14000009, "Slot 2C");
    /* USB Slot 1 (0x256f series) */
    patch_single_site(0x256fb3c, 0x54000120, 0x14000009, "Slot 1A");
    patch_single_site(0x256fba0, 0x54000120, 0x14000009, "Slot 1B");
    patch_single_site(0x256fe9c, 0x54000120, 0x14000009, "Slot 1C");

    /* Universal popup dispatcher interceptor */
    hook_caution_dispatcher_universal();
}

__attribute__((constructor))
static void proc_usb_shim_init(void) {
    init_real();
    if (!is_target_process()) return;
    unsetenv("LD_PRELOAD");

    /* 1. Apply in-memory patch to bypass caution 43 modal dialog */
    patch_caution_gate();

    mkdir(PROC_REDIRECT_DIR, 0777);
    mkfifo(PROC_USB1_REDIR, 0666);
    mkfifo(PROC_USB2_REDIR, 0666);
    mkfifo(PROC_USBG1_REDIR, 0666);
    mkfifo(PROC_USBCTN1_REDIR, 0666);
    mkfifo(PROC_SD1_REDIR, 0666);
    mkfifo(PROC_NFS_REDIR, 0666);

    int tf = real_open(PROC_THERMAL_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (tf >= 0) {
        if (write(tf, "45000\n", 6) > 0) {}
        close(tf);
    }

    fprintf(stderr, LOG_TAG "Pioneer USB notification & sysfs redirection initialized\n");
}

/* Helper to generate dynamic synthesized read-only files */
static int handle_synthetic_open(const char *pathname) {
    if (!pathname) return -1;

    /* 1. vsync_time: Monotonic milliseconds for render loop timing */
    if (strstr(pathname, "vsync_time") != NULL) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)ms);
        int pfd[2];
        if (pipe(pfd) == 0) {
            if (write(pfd[1], buf, len) > 0) {}
            close(pfd[1]);
            return pfd[0];
        }
    }

    /* 2. thermal_zone: Report steady 45.0°C (45000 m°C) via real regular file */
    if (strstr(pathname, "thermal_zone") && strstr(pathname, "/temp")) {
        return real_open("/tmp/xdj_az_proc/thermal_temp", O_RDONLY);
    }

    /* 3. USB storage bind / unbind and safe-eject sysfs writes */
    if (strncmp(pathname, "/sys/bus/usb/drivers/", 21) == 0 &&
        (strstr(pathname, "/bind") != NULL || strstr(pathname, "/unbind") != NULL)) {
        return real_open("/dev/null", O_WRONLY);
    }

    /* 4. Backlight brightness control */
    if (strncmp(pathname, "/sys/class/backlight/", 21) == 0 &&
        strstr(pathname, "/brightness") != NULL) {
        return real_open("/dev/null", O_WRONLY);
    }

    /* 5. i.MX hardware register block (/dev/mem) -> must return EACCES */
    if (strcmp(pathname, "/dev/mem") == 0) {
        errno = EACCES;
        return -1;
    }

    /* 6. i.MX / SoC platform stubs */
    if (strcmp(pathname, "/sys/devices/soc0/soc_id") == 0 ||
        strcmp(pathname, "/sys/devices/soc0/family") == 0 ||
        strcmp(pathname, "/sys/devices/soc0/machine") == 0) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
            if (write(fds[1], "i.MX6Q\n", 7) > 0) {}
            close(fds[1]);
            return fds[0];
        }
    }

    /* 7. Missing /dev peripheral nodes */
    if (strcmp(pathname, "/dev/hidg0") == 0 ||
        strcmp(pathname, "/dev/gpiodrv") == 0) {
        return real_open("/dev/null", O_RDWR);
    }

    /* 8. Isolated USB1 mount event: proper Pioneer connect -> mount handshake */
    if (strcmp(pathname, PROC_USB1_ORIG) == 0 || strcmp(pathname, PROC_USB1_REDIR) == 0) {
        unsigned event = atomic_fetch_add(&g_usb_event_count, 1);
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
            char msg_buf[128] = "";
            const char *msg = "";

            /* Check if external trigger file /tmp/usb1_event exists */
            int tf = real_open("/tmp/usb1_event", O_RDONLY);
            if (tf >= 0) {
                ssize_t tn = read(tf, msg_buf, sizeof(msg_buf) - 1);
                close(tf);
                if (tn > 0) {
                    msg_buf[tn] = '\0';
                    msg = msg_buf;
                    fprintf(stderr, LOG_TAG "USB1 manual trigger: %s\n", msg);
                }
            } else if (access("/media/usb/sda1/PIONEER/rekordbox/export.pdb", F_OK) == 0 ||
                       access("/media/usb/sda1/PIONEER/rekordbox/exportExt.pdb", F_OK) == 0) {
                if (event == 0) {
                    msg = "mount /media/usb/sda1 vfat protect:0";
                    fprintf(stderr, LOG_TAG "USB1 mounted: %s\n", msg);
                }
            }

            if (*msg) {
                if (write(fds[1], msg, strlen(msg)) > 0) {}
            }
            close(fds[1]);
            return fds[0];
        }
    }

    /* 9. Neutralize other udev proc endpoints (sd1, nfs, usb2, usbg1, usbctn1) */
    if (strcmp(pathname, PROC_USB2_ORIG) == 0 || strcmp(pathname, PROC_USB2_REDIR) == 0 ||
        strcmp(pathname, PROC_USBG1_ORIG) == 0 || strcmp(pathname, PROC_USBG1_REDIR) == 0 ||
        strcmp(pathname, PROC_USBCTN1_ORIG) == 0 || strcmp(pathname, PROC_USBCTN1_REDIR) == 0 ||
        strcmp(pathname, PROC_SD1_ORIG) == 0 || strcmp(pathname, PROC_SD1_REDIR) == 0 ||
        strcmp(pathname, PROC_NFS_ORIG) == 0 || strcmp(pathname, PROC_NFS_REDIR) == 0) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
            close(fds[1]);
            return fds[0];
        }
    }

    return -1;
}

int open(const char *pathname, int flags, ...) {
    init_real();
    if (pathname && strcmp(pathname, "/dev/mem") == 0) {
        errno = EACCES;
        return -1;
    }
    int synth_fd = handle_synthetic_open(pathname);
    if (synth_fd >= 0) return synth_fd;

    const char *final_path = redirect_proc_path(pathname);
    if (final_path && is_proc_udev(final_path)) {
        int synth = handle_synthetic_open(final_path);
        if (synth >= 0) return synth;
    }

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
        return real_open(final_path, flags, mode);
    }
    return real_open(final_path, flags);
}

int open64(const char *pathname, int flags, ...) {
    init_real();
    if (pathname && strcmp(pathname, "/dev/mem") == 0) {
        errno = EACCES;
        return -1;
    }
    int synth_fd = handle_synthetic_open(pathname);
    if (synth_fd >= 0) return synth_fd;

    const char *final_path = redirect_proc_path(pathname);
    if (final_path && is_proc_udev(final_path)) {
        int synth = handle_synthetic_open(final_path);
        if (synth >= 0) return synth;
    }

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
        return real_open64 ? real_open64(final_path, flags, mode) : real_open(final_path, flags, mode);
    }
    return real_open64 ? real_open64(final_path, flags) : real_open(final_path, flags);
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    init_real();
    if (pathname && strcmp(pathname, "/dev/mem") == 0) {
        errno = EACCES;
        return -1;
    }
    int synth_fd = handle_synthetic_open(pathname);
    if (synth_fd >= 0) return synth_fd;

    const char *final_path = redirect_proc_path(pathname);
    if (final_path && is_proc_udev(final_path)) {
        int synth = handle_synthetic_open(final_path);
        if (synth >= 0) return synth;
    }

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
        return real_openat ? real_openat(dirfd, final_path, flags, mode) : real_open(final_path, flags, mode);
    }
    return real_openat ? real_openat(dirfd, final_path, flags) : real_open(final_path, flags);
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
    init_real();
    if (pathname && strcmp(pathname, "/dev/mem") == 0) {
        errno = EACCES;
        return -1;
    }
    int synth_fd = handle_synthetic_open(pathname);
    if (synth_fd >= 0) return synth_fd;

    const char *final_path = redirect_proc_path(pathname);
    if (final_path && is_proc_udev(final_path)) {
        int synth = handle_synthetic_open(final_path);
        if (synth >= 0) return synth;
    }

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
        return real_openat64 ? real_openat64(dirfd, final_path, flags, mode) : real_open64(final_path, flags, mode);
    }
    return real_openat64 ? real_openat64(dirfd, final_path, flags) : real_open64(final_path, flags);
}

FILE* fopen(const char *pathname, const char *mode) {
    init_real();
    if (pathname && strstr(pathname, "thermal_zone") && strstr(pathname, "/temp")) {
        int fd = handle_synthetic_open(pathname);
        if (fd >= 0) return fdopen(fd, mode);
    }
    return real_fopen(redirect_proc_path(pathname), mode);
}

FILE* fopen64(const char *pathname, const char *mode) {
    init_real();
    if (pathname && strstr(pathname, "thermal_zone") && strstr(pathname, "/temp")) {
        int fd = handle_synthetic_open(pathname);
        if (fd >= 0) return fdopen(fd, mode);
    }
    return real_fopen64 ? real_fopen64(redirect_proc_path(pathname), mode) : real_fopen(redirect_proc_path(pathname), mode);
}

int access(const char *pathname, int mode) {
    init_real();
    if (pathname) {
        if (is_proc_udev(pathname) ||
            strstr(pathname, "vsync_time") ||
            (strstr(pathname, "thermal_zone") && strstr(pathname, "/temp")) ||
            strcmp(pathname, "/dev/hidg0") == 0 ||
            strcmp(pathname, "/dev/gpiodrv") == 0) {
            return 0;
        }
    }
    return real_access(redirect_proc_path(pathname), mode);
}

int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    init_real();
    if (pathname) {
        if (is_proc_udev(pathname) ||
            strstr(pathname, "vsync_time") ||
            (strstr(pathname, "thermal_zone") && strstr(pathname, "/temp")) ||
            strcmp(pathname, "/dev/hidg0") == 0 ||
            strcmp(pathname, "/dev/gpiodrv") == 0) {
            return 0;
        }
    }
    return real_faccessat ? real_faccessat(dirfd, redirect_proc_path(pathname), mode, flags) : real_access(redirect_proc_path(pathname), mode);
}

static inline void fake_fifo_stat(struct stat *statbuf) {
    if (statbuf) {
        memset(statbuf, 0, sizeof(*statbuf));
        statbuf->st_mode = S_IFIFO | 0666;
        statbuf->st_nlink = 1;
    }
}

static inline void fake_fifo_stat64(struct stat64 *statbuf) {
    if (statbuf) {
        memset(statbuf, 0, sizeof(*statbuf));
        statbuf->st_mode = S_IFIFO | 0666;
        statbuf->st_nlink = 1;
    }
}

int stat(const char *pathname, struct stat *statbuf) {
    init_real();
    if (pathname) {
        if (is_proc_udev(pathname)) {
            fake_fifo_stat(statbuf);
            return 0;
        }
        if (strstr(pathname, "thermal_zone") && strstr(pathname, "/temp")) {
            return real_stat(PROC_THERMAL_TEMP, statbuf);
        }
        if (strstr(pathname, "vsync_time") ||
            strcmp(pathname, "/dev/hidg0") == 0 ||
            strcmp(pathname, "/dev/gpiodrv") == 0) {
            return real_stat("/dev/null", statbuf);
        }
    }
    return real_stat(redirect_proc_path(pathname), statbuf);
}

int stat64(const char *pathname, struct stat64 *statbuf) {
    init_real();
    if (pathname) {
        if (is_proc_udev(pathname)) {
            fake_fifo_stat64(statbuf);
            return 0;
        }
        if (strstr(pathname, "thermal_zone") && strstr(pathname, "/temp")) {
            return real_stat64 ? real_stat64(PROC_THERMAL_TEMP, statbuf) : -1;
        }
        if (strstr(pathname, "vsync_time") ||
            strcmp(pathname, "/dev/hidg0") == 0 ||
            strcmp(pathname, "/dev/gpiodrv") == 0) {
            return real_stat64 ? real_stat64("/dev/null", statbuf) : -1;
        }
    }
    return real_stat64 ? real_stat64(redirect_proc_path(pathname), statbuf) : -1;
}

int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags) {
    init_real();
    if (pathname) {
        if (is_proc_udev(pathname)) {
            fake_fifo_stat(statbuf);
            return 0;
        }
    }
    return real_fstatat ? real_fstatat(dirfd, redirect_proc_path(pathname), statbuf, flags) : real_stat(redirect_proc_path(pathname), statbuf);
}

int fstatat64(int dirfd, const char *pathname, struct stat64 *statbuf, int flags) {
    init_real();
    if (pathname) {
        if (is_proc_udev(pathname)) {
            fake_fifo_stat64(statbuf);
            return 0;
        }
    }
    return real_fstatat64 ? real_fstatat64(dirfd, redirect_proc_path(pathname), statbuf, flags) : real_stat64(redirect_proc_path(pathname), statbuf);
}

int posix_fallocate(int fd, off_t offset, off_t len) {
    if (ftruncate(fd, offset + len) < 0) return errno;
    return 0;
}

int posix_fallocate64(int fd, off64_t offset, off64_t len) {
    if (ftruncate64(fd, offset + len) < 0) return errno;
    return 0;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    init_real();
    if (fd < 0 && (flags & MAP_SHARED) && !(flags & MAP_ANONYMOUS)) {
        flags = (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS;
        fd = -1;
        offset = 0;
    }
    return real_mmap ? real_mmap(addr, length, prot, flags, fd, offset) : MAP_FAILED;
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    init_real();
    if (fd < 0 && (flags & MAP_SHARED) && !(flags & MAP_ANONYMOUS)) {
        flags = (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS;
        fd = -1;
        offset = 0;
    }
    return real_mmap64 ? real_mmap64(addr, length, prot, flags, fd, offset) : (real_mmap ? real_mmap(addr, length, prot, flags, fd, offset) : MAP_FAILED);
}
