/*
 * net_shim.c — Network Interface & Pro DJ Link Aliasing Shim for AlphaTheta XDJ-AZ
 * Target: Amlogic S905X (ARM64 AArch64)
 *
 * AlphaTheta XDJ-AZ firmware queries "mlan0" (onboard NXP Wi-Fi) by name via:
 *   - ioctl (SIOCGIFADDR, SIOCGIFNETMASK, SIOCGIFHWADDR, SIOCGIFINDEX, SIOCGIFFLAGS)
 *   - getifaddrs() / freeifaddrs()
 *   - if_nametoindex() / if_indextoname()
 *
 * This shim intercepts those queries and transparently aliases them to the active
 * host network interface (e.g., "wlan0", "eth0", or user-configured AZ_NET_IFACE),
 * allowing Pro DJ Link and Cloud Streaming to operate over any standard network.
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
#include <errno.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <ifaddrs.h>

#define LOG_TAG "[net_shim] "
#define TARGET_IFACE_ORIG "mlan0"

static int (*real_ioctl)(int fd, unsigned long request, ...) = NULL;
static unsigned int (*real_if_nametoindex)(const char *ifname) = NULL;
static char* (*real_if_indextoname)(unsigned int ifindex, char *ifname) = NULL;
static int (*real_getifaddrs)(struct ifaddrs **ifap) = NULL;

static char g_alias_target[IFNAMSIZ] = "wlan0";
static bool g_inited = false;

static void init_real(void) {
    if (!real_ioctl) {
        real_ioctl          = dlsym(RTLD_NEXT, "ioctl");
        real_if_nametoindex = dlsym(RTLD_NEXT, "if_nametoindex");
        real_if_indextoname = dlsym(RTLD_NEXT, "if_indextoname");
        real_getifaddrs     = dlsym(RTLD_NEXT, "getifaddrs");
    }
}

static void detect_active_interface(void) {
    const char *env_if = getenv("AZ_NET_IFACE");
    if (env_if && env_if[0]) {
        strncpy(g_alias_target, env_if, IFNAMSIZ - 1);
        g_alias_target[IFNAMSIZ - 1] = '\0';
        return;
    }

    /* Auto-detect active interface by probing /proc/net/dev */
    FILE *f = fopen("/proc/net/dev", "r");
    if (f) {
        char line[256];
        /* Skip 2 header lines */
        if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f)) {
            while (fgets(line, sizeof(line), f)) {
                char *colon = strchr(line, ':');
                if (!colon) continue;
                *colon = '\0';
                char *name = line;
                while (*name == ' ') name++;
                if (strcmp(name, "lo") != 0 && strcmp(name, TARGET_IFACE_ORIG) != 0) {
                    strncpy(g_alias_target, name, IFNAMSIZ - 1);
                    g_alias_target[IFNAMSIZ - 1] = '\0';
                    break;
                }
            }
        }
        fclose(f);
    }
}

__attribute__((constructor))
static void net_shim_init(void) {
    init_real();
    detect_active_interface();
    fprintf(stderr, LOG_TAG "Pro DJ Link alias active: '%s' -> '%s'\n", TARGET_IFACE_ORIG, g_alias_target);
    g_inited = true;
}

static inline bool is_target_iface(const char *name) {
    return (name && strcmp(name, TARGET_IFACE_ORIG) == 0);
}

/* Intercept ioctl for SIOCGIF* requests */
int ioctl(int fd, unsigned long request, ...) {
    init_real();
    va_list args;
    va_start(args, request);
    void *argp = va_arg(args, void*);
    va_end(args);

    if (argp && (request == SIOCGIFADDR || request == SIOCGIFNETMASK ||
                 request == SIOCGIFBRDADDR || request == SIOCGIFFLAGS ||
                 request == SIOCGIFHWADDR || request == SIOCGIFINDEX)) {
        struct ifreq *ifr = (struct ifreq*)argp;
        if (is_target_iface(ifr->ifr_name)) {
            char orig_name[IFNAMSIZ];
            strncpy(orig_name, ifr->ifr_name, IFNAMSIZ - 1);
            strncpy(ifr->ifr_name, g_alias_target, IFNAMSIZ - 1);

            int ret = real_ioctl(fd, request, ifr);
            /* Restore original name for caller */
            strncpy(ifr->ifr_name, orig_name, IFNAMSIZ - 1);
            return ret;
        }
    }

    return real_ioctl ? real_ioctl(fd, request, argp) : -1;
}

/* Intercept if_nametoindex */
unsigned int if_nametoindex(const char *ifname) {
    init_real();
    if (is_target_iface(ifname)) {
        return real_if_nametoindex ? real_if_nametoindex(g_alias_target) : 1;
    }
    return real_if_nametoindex ? real_if_nametoindex(ifname) : 0;
}

/* Intercept if_indextoname */
char* if_indextoname(unsigned int ifindex, char ifname[IF_NAMESIZE]) {
    init_real();
    char *res = real_if_indextoname ? real_if_indextoname(ifindex, ifname) : NULL;
    if (res && strcmp(res, g_alias_target) == 0) {
        strncpy(ifname, TARGET_IFACE_ORIG, IFNAMSIZ - 1);
        ifname[IFNAMSIZ - 1] = '\0';
        return ifname;
    }
    return res;
}

/* Intercept getifaddrs to inject mlan0 matching the active interface */
int getifaddrs(struct ifaddrs **ifap) {
    init_real();
    if (!real_getifaddrs) return -1;
    int ret = real_getifaddrs(ifap);
    if (ret == 0 && ifap && *ifap) {
        struct ifaddrs *ifa = *ifap;
        while (ifa) {
            if (ifa->ifa_name && strcmp(ifa->ifa_name, g_alias_target) == 0) {
                /* Duplicate node aliased as mlan0 */
                struct ifaddrs *clone = (struct ifaddrs*)malloc(sizeof(struct ifaddrs));
                if (clone) {
                    memcpy(clone, ifa, sizeof(struct ifaddrs));
                    clone->ifa_name = strdup(TARGET_IFACE_ORIG);
                    clone->ifa_next = *ifap;
                    *ifap = clone;
                }
                break;
            }
            ifa = ifa->ifa_next;
        }
    }
    return ret;
}
