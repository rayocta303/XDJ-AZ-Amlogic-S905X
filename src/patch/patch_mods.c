/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file patch_mods.c
 * @brief 60 FPS waveform mod, QR login crash guard, and signal crash reporter for EP147.
 */

#include "patch_mods.h"
#include "az_patch_core.h"

/* ============================================================================
 * 1. 60 FPS Waveform & UI Mod
 * ============================================================================
 */
int patch_waveform_60fps_init(void) {
    patch_log("Applying 60 FPS waveform repaint mod...");
    uint32_t want = EP147_UI60_INSN_60HZ; /* 0x52800201: mov w1, #16 */
    
    uint32_t *site1 = (uint32_t *)EP147_UI60_SITE;
    uint32_t *site2 = (uint32_t *)EP147_UI60_SITE2;

    int r1 = 0, r2 = 0;
    if (*site1 == EP147_UI60_INSN_30HZ || *site1 == EP147_UI60_INSN_60HZ) {
        r1 = az_patch_code(EP147_UI60_SITE, &want, sizeof(want));
    }
    if (*site2 == EP147_UI60_INSN_30HZ || *site2 == EP147_UI60_INSN_60HZ) {
        r2 = az_patch_code(EP147_UI60_SITE2, &want, sizeof(want));
    }

    if (r1 > 0 && r2 > 0) {
        patch_log("SUCCESS: 60 FPS waveform timer enabled (period=16ms at 0x%lx & 0x%lx)",
                  EP147_UI60_SITE, EP147_UI60_SITE2);
        return 0;
    }
    patch_log("WARNING: 60 FPS patch sites partially skipped (site1=%d, site2=%d)", r1, r2);
    return -1;
}

/* ============================================================================
 * 2. Deviceless / QR Login Crash Guard
 * ============================================================================
 */
static void (*g_orig_qrtick)(void *) = NULL;

struct qr_sp {
    void *p, *c, *pad;
};

static void hook_qrlogin_tick(void *self) {
    uintptr_t s = (uintptr_t)self;
    if (s && !(s & 7)) {
        uintptr_t st = *(uintptr_t *)(s + 0x98);
        uintptr_t m  = *(uintptr_t *)(s + 0x80);
        if ((int)(st & 0xffffffff) != 1) {
            int is_null = !m;
            if (m && !(m & 7)) {
                uintptr_t vt = *(uintptr_t *)m;
                if (vt && !(vt & 7)) {
                    uintptr_t fn = *(uintptr_t *)(vt + 0x18);
                    if (fn) {
                        struct qr_sp r = ((struct qr_sp (*)(void *))fn)((void *)m);
                        is_null = !r.p;
                        if (r.c) {
                            ((void (*)(void *))EP147_SP_M_RELEASE)(r.c);
                        }
                    }
                }
            }
            if (is_null) {
                static int logs = 5;
                if (logs-- > 0) {
                    patch_log("QR login tick skipped: null session pointer (crash guarded)");
                }
                return;
            }
        }
    }
    if (g_orig_qrtick) {
        g_orig_qrtick(self);
    }
}

int patch_qrlogin_guard_init(void) {
    patch_log("Installing Deviceless QR login crash guard...");
    int rc = az_install_hook(EP147_QRLOGIN_TICK, (void *)hook_qrlogin_tick, (void **)&g_orig_qrtick);
    if (rc > 0) {
        patch_log("SUCCESS: QR login crash guard installed at 0x%lx", EP147_QRLOGIN_TICK);
        return 0;
    }
    patch_log("WARNING: Failed to install QR login crash guard at 0x%lx", EP147_QRLOGIN_TICK);
    return -1;
}

/* ============================================================================
 * 3. Signal Crash Reporter
 * ============================================================================
 */
static struct sigaction g_prev_sigsegv;
static struct sigaction g_prev_sigbus;
static struct sigaction g_prev_sigill;
static struct sigaction g_prev_sigfpe;
static struct sigaction g_prev_sigabrt;

static void hex64(char *out, unsigned long v) {
    static const char d[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; i++) {
        out[2 + i] = d[(v >> ((15 - i) * 4)) & 0xf];
    }
    out[18] = 0;
}

static void sig_write(int fd, const char *s) {
    if (fd < 0 || !s) return;
    size_t len = 0;
    while (s[len]) len++;
    ssize_t w = write(fd, s, len);
    (void)w;
}

static void patch_signal_handler(int sig, siginfo_t *si, void *uc) {
    int fd = open("/tmp/azmod-crash.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
        char b[64];
        ucontext_t *u = (ucontext_t *)uc;
        unsigned long pc = u ? u->uc_mcontext.pc : 0;
        unsigned long lr = u ? u->uc_mcontext.regs[30] : 0;
        unsigned long sp = u ? u->uc_mcontext.sp : 0;

        sig_write(fd, "\n==================== AZ_PATCH CRASH REPORT ====================\n");
        sig_write(fd, "Signal: ");
        if (sig == SIGSEGV) sig_write(fd, "SIGSEGV (Segmentation Fault)\n");
        else if (sig == SIGBUS)  sig_write(fd, "SIGBUS (Bus Error)\n");
        else if (sig == SIGILL)  sig_write(fd, "SIGILL (Illegal Instruction)\n");
        else if (sig == SIGFPE)  sig_write(fd, "SIGFPE (Floating Point Exception)\n");
        else if (sig == SIGABRT) sig_write(fd, "SIGABRT (Aborted)\n");
        else sig_write(fd, "Unknown Signal\n");

        sig_write(fd, "Fault Addr: ");
        hex64(b, (unsigned long)si->si_addr);
        sig_write(fd, b);
        sig_write(fd, "\nPC:         ");
        hex64(b, pc);
        sig_write(fd, b);
        sig_write(fd, "\nLR:         ");
        hex64(b, lr);
        sig_write(fd, b);
        sig_write(fd, "\nSP:         ");
        hex64(b, sp);
        sig_write(fd, b);
        sig_write(fd, "\n=================================================================\n");
        close(fd);
    }

    /* Chain to previous signal action */
    if (sig == SIGSEGV && g_prev_sigsegv.sa_sigaction) g_prev_sigsegv.sa_sigaction(sig, si, uc);
    else if (sig == SIGBUS && g_prev_sigbus.sa_sigaction) g_prev_sigbus.sa_sigaction(sig, si, uc);
    else if (sig == SIGILL && g_prev_sigill.sa_sigaction) g_prev_sigill.sa_sigaction(sig, si, uc);
    else if (sig == SIGFPE && g_prev_sigfpe.sa_sigaction) g_prev_sigfpe.sa_sigaction(sig, si, uc);
    else if (sig == SIGABRT && g_prev_sigabrt.sa_sigaction) g_prev_sigabrt.sa_sigaction(sig, si, uc);
    else _exit(128 + sig);
}

int patch_crash_reporter_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = patch_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, &g_prev_sigsegv);
    sigaction(SIGBUS, &sa, &g_prev_sigbus);
    sigaction(SIGILL, &sa, &g_prev_sigill);
    sigaction(SIGFPE, &sa, &g_prev_sigfpe);
    sigaction(SIGABRT, &sa, &g_prev_sigabrt);

    patch_log("SUCCESS: Signal crash reporter installed (SIGSEGV/BUS/ILL/FPE/ABRT)");
    return 0;
}

/* ============================================================================
 * 4. Bluetooth Stack Bypass & Activation Mod
 * ============================================================================
 */
#define EP147_BT_INIT_CHECK_SITE 0x2121248
#define EP147_BT_INIT_INSN_CBNZ  0x35002ec0u /* cbnz w0, 0x2121820 (jump to startFailed) */
#define EP147_BT_INIT_INSN_NOP   0xd503201fu /* nop */

int patch_bluetooth_init(void) {
    patch_log("Applying Bluetooth stack error bypass & auto-start mod...");
    uintptr_t site = EP147_BT_INIT_CHECK_SITE;
    uint32_t current = *(volatile uint32_t *)site;

    if (current == EP147_BT_INIT_INSN_CBNZ) {
        uint32_t nop = EP147_BT_INIT_INSN_NOP;
        int r = az_patch_code(site, &nop, sizeof(nop));
        if (r > 0) {
            patch_log("SUCCESS: Bluetooth error check bypassed at 0x%lx (NOP installed)", site);
            return 0;
        } else {
            patch_log("ERROR: Failed to patch Bluetooth check at 0x%lx", site);
            return -1;
        }
    } else if (current == EP147_BT_INIT_INSN_NOP) {
        patch_log("Bluetooth patch already applied at 0x%lx", site);
        return 0;
    } else {
        patch_log("WARNING: Unexpected instruction at 0x%lx: 0x%08x (expected 0x%08x)",
                  site, current, EP147_BT_INIT_INSN_CBNZ);
        return -1;
    }
}
