/*
 * crashcatch.c — 64-bit AArch64 Crash Diagnostics & Fault Recovery Shim
 * Target: Amlogic S905X (ARM64 AArch64)
 *
 * Intercepts fatal signals (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) and
 * deliberate termination paths (exit, _exit), recording:
 *   - Program Counter (PC), Stack Pointer (SP), and general-purpose registers (X0..X30)
 *   - Faulting memory address (si_addr)
 *   - Full execution stack backtrace via libunwind / execinfo
 *
 * Output is logged to /opt/xdjaz/log/crash.log using async-signal-safe I/O.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#pragma GCC diagnostic ignored "-Wunused-result"
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <ucontext.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <execinfo.h>
#include <sys/time.h>

#define CRASH_LOG_PATH "/opt/xdjaz/log/crash.log"
#define CRASH_LOG_FALLBACK "/tmp/xdjaz_crash.log"

static volatile sig_atomic_t g_in_handler = 0;

static void safe_write_str(int fd, const char *str) {
    if (fd < 0 || !str) return;
    size_t len = 0;
    while (str[len]) len++;
    if (write(fd, str, len) > 0) {}
}

static void safe_write_hex(int fd, uint64_t val) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0xF;
        buf[17 - i] = (nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10);
    }
    buf[18] = '\0';
    safe_write_str(fd, buf);
}

static void crash_handler(int sig, siginfo_t *info, void *uctx_raw) {
    if (g_in_handler) {
        _exit(128 + sig);
    }
    g_in_handler = 1;

    ucontext_t *uc = (ucontext_t*)uctx_raw;

    int fd = open(CRASH_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) {
        fd = open(CRASH_LOG_FALLBACK, O_WRONLY | O_CREAT | O_APPEND, 0666);
    }
    if (fd < 0) fd = STDERR_FILENO;

    safe_write_str(fd, "\n=======================================================\n");
    safe_write_str(fd, "  ALPHATHETA XDJ-AZ CRASH DETECTED (AArch64)\n");
    safe_write_str(fd, "=======================================================\n");

    const char *sig_name = "UNKNOWN";
    if (sig == SIGSEGV) sig_name = "SIGSEGV (Segmentation Fault)";
    else if (sig == SIGBUS)  sig_name = "SIGBUS (Bus Error)";
    else if (sig == SIGILL)  sig_name = "SIGILL (Illegal Instruction)";
    else if (sig == SIGFPE)  sig_name = "SIGFPE (Floating Point Exception)";
    else if (sig == SIGABRT) sig_name = "SIGABRT (Abort)";

    safe_write_str(fd, "Signal: ");
    safe_write_str(fd, sig_name);
    safe_write_str(fd, "\nFault Address: ");
    if (info) {
        safe_write_hex(fd, (uint64_t)(uintptr_t)info->si_addr);
    }
    safe_write_str(fd, "\n");

#if defined(__aarch64__)
    if (uc) {
        safe_write_str(fd, "Program Counter (PC): ");
        safe_write_hex(fd, uc->uc_mcontext.pc);
        safe_write_str(fd, "\nStack Pointer   (SP): ");
        safe_write_hex(fd, uc->uc_mcontext.sp);
        safe_write_str(fd, "\nLink Register   (LR): ");
        safe_write_hex(fd, uc->uc_mcontext.regs[30]);
        safe_write_str(fd, "\n\nGeneral Registers:\n");

        for (int i = 0; i < 30; i += 2) {
            safe_write_str(fd, "  X");
            if (i < 10) { if (write(fd, " ", 1) > 0) {} }
            char num[4];
            snprintf(num, sizeof(num), "%d", i);
            safe_write_str(fd, num);
            safe_write_str(fd, ": ");
            safe_write_hex(fd, uc->uc_mcontext.regs[i]);

            safe_write_str(fd, "   X");
            if (i + 1 < 10) { if (write(fd, " ", 1) > 0) {} }
            snprintf(num, sizeof(num), "%d", i + 1);
            safe_write_str(fd, num);
            safe_write_str(fd, ": ");
            safe_write_hex(fd, uc->uc_mcontext.regs[i + 1]);
            safe_write_str(fd, "\n");
        }
    }
#endif

    safe_write_str(fd, "\nCall Stack Backtrace:\n");
    void *callstack[32];
    int frames = backtrace(callstack, 32);
    backtrace_symbols_fd(callstack, frames, fd);

    safe_write_str(fd, "=======================================================\n\n");
    if (fd != STDERR_FILENO) close(fd);

    /* Restore default signal handler and re-raise */
    signal(sig, SIG_DFL);
    raise(sig);
}

__attribute__((constructor))
static void crashcatch_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    fprintf(stderr, "[crashcatch] AlphaTheta AArch64 CrashGuard initialized\n");
}
