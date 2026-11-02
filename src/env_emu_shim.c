/*
 * env_emu_shim.c — Hardware Environment & Peripheral Stubs Shim
 * Target: Amlogic S905X (ARM64 AArch64)
 *
 * Provides mock responses for offline environment parameters:
 * - Mock /tmp/mlan0Addr (prevents wlan-monitor.sh from stalling startup)
 * - Intercepts fw_printenv command invocations to report model=XDJ-AZ, version=1.30
 * - Handles device node fallbacks for thermal and clock sensors
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#pragma GCC diagnostic ignored "-Wnonnull-compare"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/stat.h>

#define LOG_TAG "[env_emu_shim] "
#define MLAN_ADDR_FILE "/tmp/mlan0Addr"

static int (*real_execve)(const char *filename, char *const argv[], char *const envp[]) = NULL;
static int (*real_system)(const char *command) = NULL;

static void init_real(void) {
    if (!real_execve) {
        real_execve = dlsym(RTLD_NEXT, "execve");
        real_system = dlsym(RTLD_NEXT, "system");
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

__attribute__((constructor))
static void env_emu_init(void) {
    init_real();
    if (!is_target_process()) return;
    unsetenv("LD_PRELOAD");
    /* Ensure /tmp/mlan0Addr exists with a dummy MAC to prevent wlan-monitor stall */
    int fd = open(MLAN_ADDR_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        const char *dummy_mac = "00:00:00:00:00:00\n";
        if (write(fd, dummy_mac, strlen(dummy_mac)) > 0) {}
        close(fd);
    }
    fprintf(stderr, LOG_TAG "Environment stubs & offline fixtures initialized\n");
}

/* Intercept execve to redirect or mock fw_printenv */
int execve(const char *filename, char *const argv[], char *const envp[]) {
    init_real();
    if (filename && strstr(filename, "fw_printenv")) {
        /* If asking for model or version */
        if (argv && argv[1]) {
            if (strcmp(argv[1], "model") == 0) {
                printf("model=XDJ-AZ\n");
                exit(0);
            }
            if (strcmp(argv[1], "version") == 0) {
                printf("version=1.30\n");
                exit(0);
            }
        }
        printf("model=XDJ-AZ\nversion=1.30\nserial=ATC-D810-0001\n");
        exit(0);
    }
    return real_execve(filename, argv, envp);
}
