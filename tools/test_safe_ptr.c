#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static int s_pipe[2] = {-1, -1};

static inline bool is_safe_ptr(uintptr_t p, size_t sz) {
    if (p < 0x400000 || (p & 7) != 0) return false;
    if (s_pipe[0] < 0) {
        if (pipe2(s_pipe, O_NONBLOCK | O_CLOEXEC) < 0) return false;
    }
    ssize_t ret = write(s_pipe[1], (const void*)p, sz);
    if (ret == (ssize_t)sz) {
        char dummy[16];
        if (read(s_pipe[0], dummy, sizeof(dummy)) > 0) {}
        return true;
    }
    return false;
}

int main(void) {
    uint64_t valid_val __attribute__((aligned(8))) = 12345;
    uintptr_t valid_ptr = (uintptr_t)&valid_val;
    uintptr_t invalid_ptr = 0x7f12345670ULL; /* unmapped */
    uintptr_t null_ptr = 0;

    printf("valid_ptr: %s\n", is_safe_ptr(valid_ptr, 8) ? "SAFE" : "UNSAFE");
    printf("invalid_ptr: %s (errno=%d)\n", is_safe_ptr(invalid_ptr, 8) ? "SAFE" : "UNSAFE", errno);
    printf("null_ptr: %s\n", is_safe_ptr(null_ptr, 8) ? "SAFE" : "UNSAFE");
    printf("All tests passed without SIGSEGV!\n");
    return 0;
}
