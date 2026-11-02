/*
 * proc_usb_shim.c — /proc/udev_* Mount Redirection & Hardware Sysfs Shim
 * Target: Amlogic S905X (ARM64 AArch64) & Linux Targets
 *
 * Implements:
 * 1. Isolated USB1 connection & mount sequence ("connect," then "mount /media/usb/sda1 ...")
 *    preventing the "USB Error. Remove the device." modal dialog.
 * 2. Neutralization of /proc/udev_usbctn1 (discards wlanbterr / caution notifications).
 * 3. Virtualization of Rockchip vsync_time (/sys/module/rockchipdrm/parameters/vsync_time)
 *    using monotonic timestamp pipes to pace EP147 rendering loops.
 * 4. Virtualization of thermal sensor (/sys/class/thermal/thermal_zone0/temp)
 *    reporting steady 45.0°C (45000) to satisfy OverheatingWatcher.
 * 5. Stubs for USB storage bind/unbind sysfs writes and backlight writes.
 * 6. Dummy redirection for missing peripheral devices (/dev/hidg0, /dev/gpiodrv).
 * 7. Implementation of posix_fallocate / posix_fallocate64 for sparse files.
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

#define PROC_REDIRECT_DIR  "/tmp/xdj_az_proc"
#define PROC_USB1_REDIR    "/tmp/xdj_az_proc/udev_usb1"
#define PROC_USB2_REDIR    "/tmp/xdj_az_proc/udev_usb2"
#define PROC_USBG1_REDIR   "/tmp/xdj_az_proc/udev_usbg1"
#define PROC_USBCTN1_REDIR "/tmp/xdj_az_proc/udev_usbctn1"
#define PROC_THERMAL_TEMP  "/tmp/xdj_az_proc/thermal_temp"

#define LOG_TAG "[proc_usb_shim] "

static int (*real_open)(const char *pathname, int flags, ...) = NULL;
static int (*real_open64)(const char *pathname, int flags, ...) = NULL;
static FILE* (*real_fopen)(const char *pathname, const char *mode) = NULL;
static void* (*real_mmap)(void*, size_t, int, int, int, off_t) = NULL;
static void* (*real_mmap64)(void*, size_t, int, int, int, off_t) = NULL;
static FILE* (*real_fopen64)(const char *pathname, const char *mode) = NULL;
static int (*real_access)(const char *pathname, int mode) = NULL;
static int (*real_stat)(const char *pathname, struct stat *statbuf) = NULL;
static int (*real_stat64)(const char *pathname, struct stat64 *statbuf) = NULL;

static atomic_uint g_usb_event_count = 0;

static void init_real(void) {
    if (!real_open) {
        real_open    = dlsym(RTLD_NEXT, "open");
        real_open64  = dlsym(RTLD_NEXT, "open64");
        real_fopen   = dlsym(RTLD_NEXT, "fopen");
        real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
        real_mmap    = dlsym(RTLD_NEXT, "mmap");
        real_mmap64  = dlsym(RTLD_NEXT, "mmap64");
        real_access  = dlsym(RTLD_NEXT, "access");
        real_stat    = dlsym(RTLD_NEXT, "stat");
        real_stat64  = dlsym(RTLD_NEXT, "stat64");
    }
}

static const char* redirect_proc_path(const char *path) {
    if (!path) return path;
    if (strcmp(path, PROC_USB1_ORIG) == 0) return PROC_USB1_REDIR;
    if (strcmp(path, PROC_USB2_ORIG) == 0) return PROC_USB2_REDIR;
    if (strcmp(path, PROC_USBG1_ORIG) == 0) return PROC_USBG1_REDIR;
    if (strcmp(path, PROC_USBCTN1_ORIG) == 0) return PROC_USBCTN1_REDIR;
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

/* In-memory binary patch: bypass caution 43 ("USB Error. Remove the device.") at 0x25bf848 */
static void patch_caution_gate(void) {
    uintptr_t patch_addr = 0x25bf848;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    uintptr_t page_base = patch_addr & ~(page_size - 1);

    volatile uint32_t *target = (volatile uint32_t *)patch_addr;
    /* Verify original instruction: b.eq 0x25bf86c (0x54000120) */
    if (*target == 0x54000120) {
        if (mprotect((void *)page_base, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            *target = 0x14000009; /* b 0x25bf86c (unconditional branch skipping caution 43) */
            __builtin___clear_cache((char *)patch_addr, (char *)(patch_addr + 4));
            mprotect((void *)page_base, page_size, PROT_READ | PROT_EXEC);
            fprintf(stderr, LOG_TAG "Caution 43 bypass patch applied at 0x%lx (0x54000120 -> 0x14000009)\n", (unsigned long)patch_addr);
        } else {
            fprintf(stderr, LOG_TAG "ERROR: mprotect failed at 0x%lx: %s\n", (unsigned long)page_base, strerror(errno));
        }
    } else if (*target == 0x14000009) {
        fprintf(stderr, LOG_TAG "Caution 43 bypass patch already active at 0x%lx\n", (unsigned long)patch_addr);
    } else {
        fprintf(stderr, LOG_TAG "Notice: opcode at 0x%lx is 0x%08x (expected 0x54000120)\n", (unsigned long)patch_addr, *target);
    }
}

/* Dummy UNIX Domain Socket server for /tmp/edb_daemon.sock to satisfy UDSCommunicator */
static void *edb_dummy_server_thread(void *arg) {
    (void)arg;
    const char *sock_path = "/tmp/edb_daemon.sock";
    unlink(sock_path);

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) return NULL;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sfd);
        return NULL;
    }

    chmod(sock_path, 0666);
    if (listen(sfd, 5) < 0) {
        close(sfd);
        return NULL;
    }

    fprintf(stderr, LOG_TAG "Dummy edb_daemon UDS socket listening at %s\n", sock_path);

    while (1) {
        int client_fd = accept(sfd, NULL, NULL);
        if (client_fd >= 0) {
            char buf[64];
            while (read(client_fd, buf, sizeof(buf)) > 0) {}
            close(client_fd);
        }
    }
    close(sfd);
    return NULL;
}

static void start_edb_dummy_server(void) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &attr, edb_dummy_server_thread, NULL);
    pthread_attr_destroy(&attr);
}

__attribute__((constructor))
static void proc_usb_shim_init(void) {
    init_real();
    if (!is_target_process()) return;
    unsetenv("LD_PRELOAD");

    /* 1. Apply in-memory patch to bypass caution 43 modal dialog */
    patch_caution_gate();

    /* 2. Start dummy edb_daemon socket listener */
    start_edb_dummy_server();

    mkdir(PROC_REDIRECT_DIR, 0777);
    mkfifo(PROC_USB1_REDIR, 0666);
    mkfifo(PROC_USB2_REDIR, 0666);
    mkfifo(PROC_USBG1_REDIR, 0666);
    mkfifo(PROC_USBCTN1_REDIR, 0666);

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

    /* 5. Missing /dev peripheral nodes */
    if (strcmp(pathname, "/dev/hidg0") == 0 ||
        strcmp(pathname, "/dev/gpiodrv") == 0) {
        return real_open("/dev/null", O_RDWR);
    }

    /* 6. Isolated USB1 mount event (mount event alone, NO "connect," which triggers USB Error) */
    if (strcmp(pathname, PROC_USB1_ORIG) == 0 || strcmp(pathname, PROC_USB1_REDIR) == 0) {
        unsigned event = atomic_fetch_add(&g_usb_event_count, 1);
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
            const char *msg = (event == 0) ? "mount /media/usb/sda1 vfat protect:0" : "";
            if (*msg) {
                if (write(fds[1], msg, strlen(msg)) > 0) {}
                fprintf(stderr, LOG_TAG "USB1 event %u synthesized: %s\n", event, msg);
            }
            close(fds[1]);
            return fds[0];
        }
    }

    /* 7. Drain & neutralize other USB proc endpoints */
    if (strcmp(pathname, PROC_USB2_ORIG) == 0 || strcmp(pathname, PROC_USB2_REDIR) == 0 ||
        strcmp(pathname, PROC_USBG1_ORIG) == 0 || strcmp(pathname, PROC_USBG1_REDIR) == 0 ||
        strcmp(pathname, PROC_USBCTN1_ORIG) == 0 || strcmp(pathname, PROC_USBCTN1_REDIR) == 0) {
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
    if (final_path && (strstr(final_path, "udev_usbctn1"))) {
        return real_open("/dev/null", O_WRONLY);
    }
    if (final_path && (strstr(final_path, "udev_usb") || strstr(final_path, "xdj_az_proc"))) {
        flags = (flags & ~O_ACCMODE) | O_RDWR;
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
    if (final_path && (strstr(final_path, "udev_usbctn1"))) {
        return real_open64 ? real_open64("/dev/null", O_WRONLY) : real_open("/dev/null", O_WRONLY);
    }
    if (final_path && (strstr(final_path, "udev_usb") || strstr(final_path, "xdj_az_proc"))) {
        flags = (flags & ~O_ACCMODE) | O_RDWR;
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
        if (strstr(pathname, "vsync_time") ||
            (strstr(pathname, "thermal_zone") && strstr(pathname, "/temp")) ||
            strcmp(pathname, "/dev/hidg0") == 0 ||
            strcmp(pathname, "/dev/gpiodrv") == 0) {
            return 0;
        }
    }
    return real_access(redirect_proc_path(pathname), mode);
}

int stat(const char *pathname, struct stat *statbuf) {
    init_real();
    if (pathname) {
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

int posix_fallocate(int fd, off_t offset, off_t len) {
    if (ftruncate(fd, offset + len) < 0) return errno;
    return 0;
}

int posix_fallocate64(int fd, off64_t offset, off64_t len) {
    if (ftruncate64(fd, offset + len) < 0) return errno;
    return 0;
}

/* Pioneer i.MX RTC / dev/mem register mapper failsafe:
 * When /dev/mem is blocked (EACCES), user_space_rtc_init() passes fd=-1 with MAP_SHARED.
 * Rewrite to MAP_PRIVATE | MAP_ANONYMOUS so it gets valid zeroed memory instead of MAP_FAILED.
 */
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
    if (real_mmap64) return real_mmap64(addr, length, prot, flags, fd, offset);
    return mmap(addr, length, prot, flags, fd, offset);
}
