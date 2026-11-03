#ifndef FAST_FZ_H
#define FAST_FZ_H

#include <stdint.h>
#include <string.h>

/*
 * fast_fz.h — High-Performance Zero-Cost Floating-Point Flush-to-Zero Engine
 * Target: Amlogic S905X (ARM Cortex-A53 AArch64) & Embedded Real-Time Audio
 *
 * Background:
 * The original reverse-engineered DSP algorithms used software memcpy() calls
 * per operation to emulate SH4/ARM floating-point register flush-to-zero.
 * On ARM Cortex-A53 (in-order execution), this caused 76 million memcpy() calls/sec,
 * generating severe load-hit-store pipeline stalls and audio buffer underruns (stuttering).
 *
 * When ARM64 Hardware Flush-to-Zero (FPCR.FZ bit 24) is enabled, the hardware FPU
 * handles denormal numbers in 0 cycles automatically, allowing us to use direct
 * single-cycle FPU instructions without any memory round-trips.
 */

#if defined(__aarch64__) || defined(_M_ARM64)

/* ARM64 Native Hardware FPU path (0 cycle overhead) */
static inline float fast_fz(float x) {
    return x;
}

static inline float fast_add(float a, float b) {
    return a + b;
}

static inline float fast_sub(float a, float b) {
    return a - b;
}

static inline float fast_mul(float a, float b) {
    return a * b;
}

#else

/* Portable fallback with union bit-punning (no memcpy) */
static inline float fast_fz(float x) {
    union { float f; uint32_t u; } p;
    p.f = x;
    if ((p.u & 0x7f800000u) == 0) p.u &= 0x80000000u;
    return p.f;
}

static inline float fast_add(float a, float b) {
    return fast_fz(fast_fz(a) + fast_fz(b));
}

static inline float fast_sub(float a, float b) {
    return fast_fz(fast_fz(a) - fast_fz(b));
}

static inline float fast_mul(float a, float b) {
    return fast_fz(fast_fz(a) * fast_fz(b));
}

#endif

#endif /* FAST_FZ_H */
