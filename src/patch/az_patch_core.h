/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file az_patch_core.h
 * @brief Memory protection, inline hook trampoline and vtable patching engine for XDJ-AZ EP147.
 *
 * Implements POSIX-compliant page unlocking/relocking via /proc/self/maps and mprotect,
 * allowing safe runtime interception of JUCE components, vtables, and machine instructions.
 */

#ifndef AZ_PATCH_CORE_H
#define AZ_PATCH_CORE_H

#include "az_linux_compat.h"

#define PATCH_LOG_TAG "[AZ_PATCH] "

static inline void az_raw_log(const char *msg) {
    if (!msg) return;
    size_t len = 0;
    while (msg[len]) len++;
    /* Write to stderr (fd 2) */
    ssize_t w2 = write(2, msg, len);
    (void)w2;
    /* Append to /tmp/az_patch_mod.log */
    int fd = open("/tmp/az_patch_mod.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
        ssize_t w = write(fd, msg, len);
        (void)w;
        close(fd);
    }
}

#define patch_log(fmt, ...) do { \
    char _log_buf[256]; \
    snprintf(_log_buf, sizeof(_log_buf), PATCH_LOG_TAG fmt "\n", ##__VA_ARGS__); \
    az_raw_log(_log_buf); \
} while (0)

#ifndef AZPROT_MAPS
#define AZPROT_MAPS "/proc/self/maps"
#endif

typedef struct {
    uintptr_t base;
    size_t len;
    int prot;
} azprot_t;

static pthread_mutex_t g_azprot_mu = PTHREAD_MUTEX_INITIALIZER;

static int azprot_parse_line(const char *s, uintptr_t *lo, uintptr_t *hi, int *prot) {
    uintptr_t v[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
        int n = 0;
        for (;; s++, n++) {
            int c = *s, d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else break;
            if (n >= 16) return 0;
            v[k] = (v[k] << 4) | (uintptr_t) d;
        }
        if (!n || *s != (k ? ' ' : '-')) return 0;
        s++;
    }
    if (v[1] <= v[0]) return 0;
    if ((s[0] != 'r' && s[0] != '-') || (s[1] != 'w' && s[1] != '-') || (s[2] != 'x' && s[2] != '-')) return 0;
    *lo = v[0];
    *hi = v[1];
    *prot = (s[0] == 'r' ? PROT_READ : 0) | (s[1] == 'w' ? PROT_WRITE : 0) | (s[2] == 'x' ? PROT_EXEC : 0);
    return 1;
}

static int azprot_query_fd(int fd, uintptr_t a, uintptr_t b) {
    if (fd < 0 || b <= a) return -1;
    char buf[2048], line[256];
    int ln = 0, prot = -1;
    uintptr_t cur = a;
    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t k = 0; k < r; k++) {
            char c = buf[k];
            if (c != '\n') {
                if (ln < (int)sizeof(line) - 1) line[ln++] = c;
                continue;
            }
            line[ln] = 0;
            ln = 0;
            uintptr_t lo, hi;
            int p;
            if (!azprot_parse_line(line, &lo, &hi, &p) || hi <= cur) continue;
            if (lo > cur) return -1; /* unmapped hole */
            if (prot >= 0 && p != prot) return -1; /* spans multiple protections */
            prot = p;
            cur = hi;
            if (cur >= b) return prot;
        }
    }
    return -1;
}

static int azprot_query(uintptr_t a, uintptr_t b) {
    int fd = open(AZPROT_MAPS, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int p = azprot_query_fd(fd, a, b);
    close(fd);
    return p;
}

static inline int azprot_unlock(uintptr_t addr, size_t len, azprot_t *t) {
    long pg = sysconf(_SC_PAGESIZE);
    if (!t || !addr || !len || pg <= 0 || addr + len < addr) return -1;
    uintptr_t base = addr & ~(uintptr_t)(pg - 1);
    uintptr_t end = (addr + len + (uintptr_t)pg - 1) & ~(uintptr_t)(pg - 1);
    pthread_mutex_lock(&g_azprot_mu);
    int prot = azprot_query(base, end);
    if (prot < 0) prot = PROT_READ | PROT_EXEC; /* fallback standard */
    if (mprotect((void *)base, end - base, prot | PROT_READ | PROT_WRITE) != 0) {
        pthread_mutex_unlock(&g_azprot_mu);
        return -1;
    }
    t->base = base;
    t->len = end - base;
    t->prot = prot;
    return 0;
}

static inline int azprot_relock(const azprot_t *t) {
    if (!t || !t->base || !t->len) return -1;
    int rc = mprotect((void *)t->base, t->len, t->prot);
    pthread_mutex_unlock(&g_azprot_mu);
    return rc == 0 ? 0 : -1;
}

/**
 * @brief Atomically swap a single virtual table slot pointer.
 */
static inline int az_patch_vtable(uintptr_t slot_addr, void *hook, void **orig) {
    if (!slot_addr || (slot_addr & 7)) return -1;
    void **slot = (void **)slot_addr;
    azprot_t pt;
    if (azprot_unlock(slot_addr, sizeof(void *), &pt) != 0) {
        patch_log("Failed to unlock memory for vtable slot: 0x%lx", (unsigned long)slot_addr);
        return -1;
    }
    if (orig) {
        __atomic_store_n(orig, *slot, __ATOMIC_RELAXED);
    }
    __atomic_store_n(slot, hook, __ATOMIC_RELEASE);
    if (azprot_relock(&pt) != 0) {
        patch_log("Warning: vtable slot page left writable: 0x%lx", (unsigned long)slot_addr);
    }
    return 1;
}

/**
 * @brief Atomically patch executable machine code with instruction cache sync.
 */
static inline int az_patch_code(uintptr_t addr, const void *code, size_t len) {
    if (!addr || !code || len == 0) return -1;
    azprot_t pt;
    if (azprot_unlock(addr, len, &pt) != 0) {
        patch_log("Failed to unlock memory for code patch: 0x%lx", (unsigned long)addr);
        return -1;
    }
    memcpy((void *)addr, code, len);
    __builtin___clear_cache((char *)addr, (char *)addr + len);
    if (azprot_relock(&pt) != 0) {
        patch_log("Warning: code page left writable: 0x%lx", (unsigned long)addr);
    }
    return 1;
}

/**
 * @brief Install an AArch64 inline hook with a 64-bit absolute jump trampoline.
 *
 * Trampoline layout (32 bytes):
 *   [0..15] : 4 relocated original instructions
 *   [16..19]: 0x58000051 (ldr x17, #8)
 *   [20..23]: 0xd61f0220 (br x17)
 *   [24..31]: uint64_t target + 16 (return address)
 *
 * Target site overwrite (16 bytes):
 *   [0..3]  : 0x58000051 (ldr x17, #8)
 *   [4..7]  : 0xd61f0220 (br x17)
 *   [8..15] : uint64_t hook (destination address)
 */
static inline int az_install_hook(uintptr_t target, void *hook, void **orig) {
    if (!target || (target & 3) || !hook) return -1;

    void *tramp = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        patch_log("Failed to allocate trampoline memory for hook at 0x%lx", (unsigned long)target);
        return -1;
    }

    uint32_t *tcode = (uint32_t *)tramp;
    int prot = azprot_query(target, target + 16);
    if (prot < 0) {
        patch_log("Target address 0x%lx is not mapped or readable", (unsigned long)target);
        munmap(tramp, 4096);
        return -1;
    }
    /* Copy first 4 instructions (16 bytes) */
    memcpy(tcode, (const void *)target, 16);

    /* Append jump back to target + 16 */
    tcode[4] = 0x58000051u; /* ldr x17, #8 */
    tcode[5] = 0xd61f0220u; /* br x17 */
    uint64_t ret_addr = (uint64_t)(target + 16);
    memcpy(&tcode[6], &ret_addr, sizeof(uint64_t));

    __builtin___clear_cache((char *)tcode, (char *)tcode + 32);

    if (orig) {
        *orig = tramp;
    }

    /* Prepare target overwrite payload (16 bytes) */
    uint32_t payload[4];
    payload[0] = 0x58000051u; /* ldr x17, #8 */
    payload[1] = 0xd61f0220u; /* br x17 */
    uint64_t hook_addr = (uint64_t)(uintptr_t)hook;
    memcpy(&payload[2], &hook_addr, sizeof(uint64_t));

    int rc = az_patch_code(target, payload, sizeof(payload));
    if (rc <= 0) {
        munmap(tramp, 4096);
        return -1;
    }

    patch_log("Inline hook installed at: 0x%lx -> %p", (unsigned long)target, hook);
    return 1;
}

/* ============================================================================
 * Known EP147 v1.30 Addresses
 * ============================================================================
 */
#define EP147_V130_SIZE         57874328L
#define EP147_V130_MD5          "3518275e8b4d5d63636d8d7b4468d7ba"

/* Beat FX Components */
#define EP147_BFX_PARAM_VT      0x2d6c498UL /* gui::waveform::BeatFxParamWidget vtable */
#define EP147_BFX_PARAM_CTOR    0x1de01d8UL /* BeatFxParamWidget constructor */
#define EP147_BFX_LAYOUT        0x1dde9e8UL /* BeatFxParamWidget::layout(this, type, sub) */

#define EP147_BFX_BODY_VT       0x2d79490UL /* gui::waveform::FxBodyWidget vtable */
#define EP147_BFX_BODY_CTOR     0x1de6000UL /* FxBodyWidget constructor */

#define EP147_BFX_DELAY_VT      0x2d45ef0UL /* gui::waveform::EffectValueBarWidgetDelay vtable */
#define EP147_BFX_DELAY_PAINT_SLOT 0x2d45fc0UL /* vtable + 0xd0 */
#define EP147_BFX_DELAY_PAINT   0x1dc0bc8UL /* EffectValueBarWidgetDelay::paint(Graphics&) */

#define EP147_BFX_REVERB_VT     0x2d6cac0UL /* gui::waveform::EffectReverbWidget vtable */
#define EP147_BFX_REVERB_PAINT_SLOT 0x2d6cb90UL /* vtable + 0xd0 */
#define EP147_BFX_REVERB_PAINT  0x1de1550UL /* EffectReverbWidget::paint(Graphics&) */

#define EP147_IMAGESCOMP_VT     0x29e9540UL /* gui::ImagesComponent vtable */
#define EP147_IMAGESCOMP_PAINT_SLOT 0x29e9610UL /* vtable + 0xd0 */
#define EP147_IMAGESCOMP_PAINT  0x19e2d68UL /* ImagesComponent::paint(Graphics&) */

#define EP147_JUCE_SETBOUNDS    0x249efe0UL /* juce::Component::setBounds(x, y, w, h) */
#define EP147_JUCE_SETSIZE      0x249f200UL /* juce::Component::setSize(w, h) */
#define EP147_JUCE_REPAINT      0x2488570UL /* juce::Component::repaint() */

/* 60 FPS Waveform Repaint Sites */
#define EP147_UI60_SITE         0x24fac4cUL /* mov w1, #33 -> mov w1, #16 */
#define EP147_UI60_SITE2        0x24f6b34UL /* mov w1, #33 -> mov w1, #16 */
#define EP147_UI60_INSN_30HZ    0x52800421u /* mov w1, #33 */
#define EP147_UI60_INSN_60HZ    0x52800201u /* mov w1, #16 */

/* Crash Guards */
#define EP147_QRLOGIN_TICK      0x1a79ea0UL /* DevicelessLoginWidgetController timer callback */
#define EP147_SP_M_RELEASE      0x6742d8UL  /* std::_Sp_counted_base::_M_release */

#endif /* AZ_PATCH_CORE_H */
