/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file az_linux_compat.h
 * @brief Self-contained POSIX / Linux AArch64 compatibility declarations.
 *
 * Automatically includes system headers when available (e.g. GCC/Clang on Linux),
 * or provides direct standard definitions when cross-compiling in bare environments.
 */

#ifndef AZ_LINUX_COMPAT_H
#define AZ_LINUX_COMPAT_H

#if defined(__has_include) && __has_include(<stdio.h>)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>

#else

/* Direct POSIX / Linux AArch64 definitions */
typedef unsigned long  size_t;
typedef long           ssize_t;
typedef unsigned long  uintptr_t;
typedef unsigned int   uint32_t;
typedef unsigned long  uint64_t;
typedef int            int32_t;
typedef long           int64_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef bool
typedef _Bool bool;
#define true 1
#define false 0
#endif

#define _SC_PAGESIZE    30

#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define MAP_FAILED      ((void *)-1)

#define O_RDONLY        0
#define O_WRONLY        1
#define O_CREAT         0x40
#define O_APPEND        0x400
#define O_CLOEXEC       0x80000

#define S_IRUSR         0400
#define S_IWUSR         0200
#define S_IRGRP         0040
#define S_IROTH         0004

#define SIGILL          4
#define SIGABRT         6
#define SIGBUS          7
#define SIGFPE          8
#define SIGSEGV         11

#define SA_SIGINFO      4
#define SA_ONSTACK      0x08000000

/* POSIX basic types */
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0

typedef struct {
    unsigned long sig[16];
} sigset_t;

typedef struct {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int _pad;
    void *si_addr;
} siginfo_t;

typedef struct {
    unsigned long fault_address;
    unsigned long regs[31];
    unsigned long sp;
    unsigned long pc;
    unsigned long pstate;
    unsigned char _pad[4096];
} mcontext_t;

typedef struct ucontext_tag {
    unsigned long uc_flags;
    struct ucontext_tag *uc_link;
    stack_t uc_stack;
    sigset_t uc_sigmask;
    unsigned char _pad[128];
    mcontext_t uc_mcontext;
} ucontext_t;

struct sigaction {
    void (*sa_sigaction)(int, siginfo_t *, void *);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

/* Standard libc dynamic function prototypes */
int open(const char *pathname, int flags, ...);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
int mprotect(void *addr, size_t len, int prot);
void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap(void *addr, size_t length);
long sysconf(int name);
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int snprintf(char *str, size_t size, const char *format, ...);
void _exit(int status);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int sigemptyset(sigset_t *set);

#endif /* __has_include */

#endif /* AZ_LINUX_COMPAT_H */
