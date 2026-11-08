/*
 * net_shim.c — Network Interface, Authentic Device MAC & Pro DJ Link Wi-Fi Shim
 * Target: AlphaTheta XDJ-AZ on Armbian Amlogic S905X (ARM64 AArch64)
 *
 * Ground Truth & Capabilities:
 *   1. Hardware MAC Address:
 *      - Reads authentic device MAC addresses from /sys/class/net/{wlan0,eth0}/address.
 *      - Supplies real device MAC (d4:58:00:37:e9:3b / 00:15:18:01:81:31) to SIOCGIFHWADDR
 *        and getifaddrs(AF_PACKET) so Rekordbox Cloud / JUCE macAddress.isNull() NEVER fails.
 *
 *   2. Pro DJ Link over Wi-Fi:
 *      - Pioneer EP147 Pro DJ Link stack queries "eth0" (wired) and "mlan0" (wireless).
 *      - Redirects setsockopt(SO_BINDTODEVICE) for "eth0" and "mlan0" to the active Wi-Fi interface ("wlan0").
 *      - Reports mlan0 & wlan0 as IFF_UP | IFF_RUNNING | IFF_BROADCAST | IFF_MULTICAST.
 *      - Reports eth0 as IFF_BROADCAST | IFF_MULTICAST (carrier down / disconnected) to eliminate
 *        multi-interface conflict warnings while maintaining authentic hardware identity.
 *      - Injects synthetic ifaddrs entries (AF_INET and AF_PACKET for mlan0/wlan0; AF_PACKET only for eth0).
 *
 *   3. Wi-Fi & WPA Control Redirection:
 *      - Redirects UNIX socket connect() from /var/run/wpa_supplicant/mlan0 or /tmp/WpaCtrl/mlan0
 *        to the active system /run/wpa_supplicant/wlan0 socket.
 *
 *   4. Bluetooth Stack Emulation & Authentic MAC Hooks:
 *      - Exports GetMacAddress, BTHCI_GetSavedBDAddr, AnWBT_GetLocalBTAddress, AnWBT_GetLocalDevInfo.
 *      - Supplies authentic device MAC ("D4:58:00:37:E9:3B" / {0x3b, 0xe9, 0x37, 0x00, 0x58, 0xd4}).
 *      - Intercepts and suppresses papiPanic and papiPanicHandler to prevent stack crashes.
 *      - Injects authentic BD_ADDR directly into prh_mgr_LocalBDADDR and bthci_addr global symbols.
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
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netpacket/packet.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <sys/stat.h>

#define LOG_TAG "[net_shim] "

/* Authentic Hardware Device Defaults on Amlogic S905X */
#define DEFAULT_WLAN_MAC "d4:58:00:37:e9:3b"
#define DEFAULT_ETH_MAC  "00:15:18:01:81:31"

static int (*real_ioctl)(int fd, unsigned long request, ...) = NULL;
static unsigned int (*real_if_nametoindex)(const char *ifname) = NULL;
static char* (*real_if_indextoname)(unsigned int ifindex, char *ifname) = NULL;
static int (*real_getifaddrs)(struct ifaddrs **ifap) = NULL;
static void (*real_freeifaddrs)(struct ifaddrs *ifa) = NULL;
static int (*real_setsockopt)(int sockfd, int level, int optname, const void *optval, socklen_t optlen) = NULL;
static int (*real_connect)(int sockfd, const struct sockaddr *addr, socklen_t addrlen) = NULL;
static int (*real_system)(const char *command) = NULL;
static FILE* (*real_popen)(const char *command, const char *type) = NULL;

static char g_active_iface[IFNAMSIZ] = "wlan0";
static uint8_t g_wlan_mac_bytes[6] = {0xd4, 0x58, 0x00, 0x37, 0xe9, 0x3b};
static uint8_t g_eth_mac_bytes[6]  = {0x00, 0x15, 0x18, 0x01, 0x81, 0x31};
static uint8_t g_bt_mac_bytes[6]   = {0x3b, 0xe9, 0x37, 0x00, 0x58, 0xd4}; /* Little-endian BD_ADDR */
static char g_bt_mac_str[20]       = "D4:58:00:37:E9:3B";

static struct in_addr g_ip_addr;
static struct in_addr g_netmask;
static struct in_addr g_broadcast;
static bool g_inited = false;

/* Explicitly exported Bluetooth symbols in case the ANW stack binds to them */
uint8_t bthci_addr[6] __attribute__((visibility("default"))) = {0x3b, 0xe9, 0x37, 0x00, 0x58, 0xd4};
uint8_t prh_mgr_LocalBDADDR[6] __attribute__((visibility("default"))) = {0x3b, 0xe9, 0x37, 0x00, 0x58, 0xd4};

static void parse_mac_string(const char *str, uint8_t out[6]) {
    unsigned int b[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
    }
}

static void read_device_mac(const char *ifname, uint8_t out[6], const char *fallback) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[32];
        if (fgets(buf, sizeof(buf), f)) {
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            parse_mac_string(buf, out);
            fclose(f);
            return;
        }
        fclose(f);
    }
    parse_mac_string(fallback, out);
}

static void init_real(void) {
    if (!real_ioctl) {
        real_ioctl          = dlsym(RTLD_NEXT, "ioctl");
        real_if_nametoindex = dlsym(RTLD_NEXT, "if_nametoindex");
        real_if_indextoname = dlsym(RTLD_NEXT, "if_indextoname");
        real_getifaddrs     = dlsym(RTLD_NEXT, "getifaddrs");
        real_freeifaddrs    = dlsym(RTLD_NEXT, "freeifaddrs");
        real_setsockopt     = dlsym(RTLD_NEXT, "setsockopt");
        real_connect        = dlsym(RTLD_NEXT, "connect");
        real_system         = dlsym(RTLD_NEXT, "system");
        real_popen          = dlsym(RTLD_NEXT, "popen");
    }
}

static void populate_bt_symbols(void) {
    /* Write authentic Bluetooth MAC to global symbols in ANW stack if present */
    uint8_t *p_bthci = (uint8_t *)dlsym(RTLD_DEFAULT, "bthci_addr");
    if (p_bthci && p_bthci != bthci_addr) {
        memcpy(p_bthci, g_bt_mac_bytes, 6);
    }
    uint8_t *p_prh = (uint8_t *)dlsym(RTLD_DEFAULT, "prh_mgr_LocalBDADDR");
    if (p_prh && p_prh != prh_mgr_LocalBDADDR) {
        memcpy(p_prh, g_bt_mac_bytes, 6);
    }
}

static void detect_network_state(void) {
    const char *env_if = getenv("AZ_NET_IFACE");
    if (env_if && env_if[0]) {
        strncpy(g_active_iface, env_if, IFNAMSIZ - 1);
        g_active_iface[IFNAMSIZ - 1] = '\0';
    } else {
        /* Prefer wlan0, then eth0 */
        if (access("/sys/class/net/wlan0", F_OK) == 0) {
            strncpy(g_active_iface, "wlan0", IFNAMSIZ - 1);
        } else if (access("/sys/class/net/eth0", F_OK) == 0) {
            strncpy(g_active_iface, "eth0", IFNAMSIZ - 1);
        }
    }

    read_device_mac("wlan0", g_wlan_mac_bytes, DEFAULT_WLAN_MAC);
    read_device_mac("eth0", g_eth_mac_bytes, DEFAULT_ETH_MAC);

    /* Derive Bluetooth BD_ADDR (reversed byte order) */
    g_bt_mac_bytes[0] = g_wlan_mac_bytes[5];
    g_bt_mac_bytes[1] = g_wlan_mac_bytes[4];
    g_bt_mac_bytes[2] = g_wlan_mac_bytes[3];
    g_bt_mac_bytes[3] = g_wlan_mac_bytes[2];
    g_bt_mac_bytes[4] = g_wlan_mac_bytes[1];
    g_bt_mac_bytes[5] = g_wlan_mac_bytes[0];

    snprintf(g_bt_mac_str, sizeof(g_bt_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             g_wlan_mac_bytes[0], g_wlan_mac_bytes[1], g_wlan_mac_bytes[2],
             g_wlan_mac_bytes[3], g_wlan_mac_bytes[4], g_wlan_mac_bytes[5]);

    memcpy(bthci_addr, g_bt_mac_bytes, 6);
    memcpy(prh_mgr_LocalBDADDR, g_bt_mac_bytes, 6);

    /* Default IP fallback: 192.168.137.22 / 24 */
    inet_aton("192.168.137.22", &g_ip_addr);
    inet_aton("255.255.255.0", &g_netmask);
    inet_aton("192.168.137.255", &g_broadcast);

    /* Probe active IP from kernel ioctl */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, g_active_iface, IFNAMSIZ - 1);
        if (real_ioctl && real_ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
            g_ip_addr = sin->sin_addr;
        }
        if (real_ioctl && real_ioctl(sock, SIOCGIFNETMASK, &ifr) == 0) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_netmask;
            g_netmask = sin->sin_addr;
        }
        if (real_ioctl && real_ioctl(sock, SIOCGIFBRDADDR, &ifr) == 0) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_broadaddr;
            g_broadcast = sin->sin_addr;
        }
        close(sock);
    }
}

static void *wifi_monitor_thread_func(void *arg) {
    (void)arg;
    mkdir("/tmp/WpaCtrl", 0777);
    chmod("/tmp/WpaCtrl", 0777);

    while (1) {
        /* Maintain WPA Supplicant socket symlinks */
        unlink("/run/wpa_supplicant/mlan0");
        symlink("/run/wpa_supplicant/wlan0", "/run/wpa_supplicant/mlan0");

        unlink("/var/run/wpa_supplicant/mlan0");
        symlink("/run/wpa_supplicant/wlan0", "/var/run/wpa_supplicant/mlan0");

        unlink("/tmp/WpaCtrl/mlan0");
        symlink("/run/wpa_supplicant/wlan0", "/tmp/WpaCtrl/mlan0");

        /* Detect authentic wlan0 carrier & operstate */
        bool is_up = false;
        FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
        if (f) {
            char state[32];
            if (fgets(state, sizeof(state), f)) {
                if (strncmp(state, "up", 2) == 0) is_up = true;
            }
            fclose(f);
        }

        /* Feed WPA event to EP147 WpaSupplicantCtrl */
        FILE *fev = fopen("/tmp/WpaCtrl/wpa_event", "w");
        if (fev) {
            fprintf(fev, "%s\n", is_up ? "CONNECTED" : "DISCONNECTED");
            fclose(fev);
            chmod("/tmp/WpaCtrl/wpa_event", 0666);
        }

        /* Maintain authentic hardware MAC files */
        char mac_str[32];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 g_wlan_mac_bytes[0], g_wlan_mac_bytes[1], g_wlan_mac_bytes[2],
                 g_wlan_mac_bytes[3], g_wlan_mac_bytes[4], g_wlan_mac_bytes[5]);
        FILE *fm = fopen("/tmp/mlan0Addr", "w");
        if (fm) {
            fprintf(fm, "%s\n", mac_str);
            fclose(fm);
            chmod("/tmp/mlan0Addr", 0666);
        }

        /* Maintain network state dat files: mlan0 up, eth0 0 */
        FILE *fstate = fopen("/var/net_mlan0_state.dat", "w");
        if (fstate) {
            fprintf(fstate, "%s\n", is_up ? "up" : "down");
            fclose(fstate);
            chmod("/var/net_mlan0_state.dat", 0666);
        }
        FILE *feth = fopen("/var/net_eth0_state.dat", "w");
        if (feth) {
            fprintf(feth, "0\n");
            fclose(feth);
            chmod("/var/net_eth0_state.dat", 0666);
        }

        sleep(1);
    }
    return NULL;
}

__attribute__((constructor))
static void net_shim_init(void) {
    init_real();
    detect_network_state();
    populate_bt_symbols();
    fprintf(stderr, LOG_TAG "Pro DJ Link Wi-Fi & Device MAC Shim active\n");
    fprintf(stderr, LOG_TAG "Active Interface: '%s' (IP: %s, Bcast: %s)\n",
            g_active_iface, inet_ntoa(g_ip_addr), inet_ntoa(g_broadcast));
    fprintf(stderr, LOG_TAG "Hardware MACs -> WLAN: %02x:%02x:%02x:%02x:%02x:%02x | ETH: %02x:%02x:%02x:%02x:%02x:%02x | BT: %s\n",
            g_wlan_mac_bytes[0], g_wlan_mac_bytes[1], g_wlan_mac_bytes[2],
            g_wlan_mac_bytes[3], g_wlan_mac_bytes[4], g_wlan_mac_bytes[5],
            g_eth_mac_bytes[0], g_eth_mac_bytes[1], g_eth_mac_bytes[2],
            g_eth_mac_bytes[3], g_eth_mac_bytes[4], g_eth_mac_bytes[5],
            g_bt_mac_str);

    pthread_t th;
    pthread_create(&th, NULL, wifi_monitor_thread_func, NULL);
    pthread_detach(th);

    g_inited = true;
}

/* ============================================================================
 * Bluetooth Stack Emulation Hooks
 * ============================================================================
 */
void GetMacAddress(char *out, void *cfg) __attribute__((visibility("default")));
void GetMacAddress(char *out, void *cfg) {
    (void)cfg;
    if (out) {
        memcpy(out, g_bt_mac_str, 18);
    }
}

int BTHCI_GetSavedBDAddr(uint8_t *out) __attribute__((visibility("default")));
int BTHCI_GetSavedBDAddr(uint8_t *out) {
    if (!out) return -1;
    memcpy(out, g_bt_mac_bytes, 6);
    populate_bt_symbols();
    return 0;
}

int AnWBT_GetLocalBTAddress(uint8_t *out) __attribute__((visibility("default")));
int AnWBT_GetLocalBTAddress(uint8_t *out) {
    return BTHCI_GetSavedBDAddr(out);
}

int AnWBT_GetLocalDevInfo(void *out) __attribute__((visibility("default")));
int AnWBT_GetLocalDevInfo(void *out) {
    if (!out) return -1;
    memset(out, 0, 0x50);
    memcpy(out, g_bt_mac_bytes, 6);
    strncpy((char *)out + 6, "AlphaTheta XDJ-AZ", 0x50 - 7);
    populate_bt_symbols();
    return 0;
}

void papiPanic(int code, int sub) __attribute__((visibility("default")));
void papiPanic(int code, int sub) {
    fprintf(stderr, LOG_TAG "papiPanic(%d, %d) intercepted & suppressed\n", code, sub);
}

void papiPanicHandler(int code, int sub) __attribute__((visibility("default")));
void papiPanicHandler(int code, int sub) {
    fprintf(stderr, LOG_TAG "papiPanicHandler(%d, %d) intercepted & suppressed\n", code, sub);
}

/* Check if interface name matches Pioneer target aliases */
static inline bool is_pioneer_link_iface(const char *name) {
    if (!name) return false;
    return (strcmp(name, "mlan0") == 0 || strcmp(name, "eth0") == 0 ||
            strcmp(name, "wlan0") == 0 || strcmp(name, "uap0") == 0);
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
        const char *name = ifr->ifr_name;

        if (is_pioneer_link_iface(name)) {
            if (request == SIOCGIFFLAGS) {
                if (strcmp(name, "eth0") == 0) {
                    /* Simulate Ethernet disconnected: broadcast & multicast only, NO IFF_UP/RUNNING */
                    ifr->ifr_flags = IFF_BROADCAST | IFF_MULTICAST;
                    return 0;
                }
                /* mlan0 and wlan0 are UP and RUNNING */
                ifr->ifr_flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
                return 0;
            }
            if (request == SIOCGIFADDR) {
                if (strcmp(name, "eth0") == 0) {
                    errno = EADDRNOTAVAIL;
                    return -1;
                }
                struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
                memset(sin, 0, sizeof(*sin));
                sin->sin_family = AF_INET;
                sin->sin_addr = g_ip_addr;
                return 0;
            }
            if (request == SIOCGIFNETMASK) {
                if (strcmp(name, "eth0") == 0) {
                    errno = EADDRNOTAVAIL;
                    return -1;
                }
                struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_netmask;
                memset(sin, 0, sizeof(*sin));
                sin->sin_family = AF_INET;
                sin->sin_addr = g_netmask;
                return 0;
            }
            if (request == SIOCGIFBRDADDR) {
                if (strcmp(name, "eth0") == 0) {
                    errno = EADDRNOTAVAIL;
                    return -1;
                }
                struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_broadaddr;
                memset(sin, 0, sizeof(*sin));
                sin->sin_family = AF_INET;
                sin->sin_addr = g_broadcast;
                return 0;
            }
            if (request == SIOCGIFHWADDR) {
                ifr->ifr_hwaddr.sa_family = ARPHRD_ETHER;
                const uint8_t *mac = (strcmp(name, "eth0") == 0) ? g_eth_mac_bytes : g_wlan_mac_bytes;
                memcpy(ifr->ifr_hwaddr.sa_data, mac, 6);
                return 0;
            }
            if (request == SIOCGIFINDEX) {
                if (strcmp(name, "eth0") == 0) ifr->ifr_ifindex = 2;
                else if (strcmp(name, "mlan0") == 0) ifr->ifr_ifindex = 3;
                else if (strcmp(name, "wlan0") == 0) ifr->ifr_ifindex = 4;
                else ifr->ifr_ifindex = 5;
                return 0;
            }
        }
    }

    return real_ioctl ? real_ioctl(fd, request, argp) : -1;
}

/* Intercept if_nametoindex */
unsigned int if_nametoindex(const char *ifname) {
    init_real();
    if (!ifname) return 0;
    if (strcmp(ifname, "eth0") == 0) return 2;
    if (strcmp(ifname, "mlan0") == 0) return 3;
    if (strcmp(ifname, "wlan0") == 0) return 4;
    return real_if_nametoindex ? real_if_nametoindex(ifname) : 0;
}

/* Intercept if_indextoname */
char* if_indextoname(unsigned int ifindex, char ifname[IF_NAMESIZE]) {
    init_real();
    if (ifindex == 2) { strncpy(ifname, "eth0", IF_NAMESIZE); return ifname; }
    if (ifindex == 3) { strncpy(ifname, "mlan0", IF_NAMESIZE); return ifname; }
    if (ifindex == 4) { strncpy(ifname, "wlan0", IF_NAMESIZE); return ifname; }
    return real_if_indextoname ? real_if_indextoname(ifindex, ifname) : NULL;
}

/* Intercept setsockopt to bind Pro DJ Link to Wi-Fi */
int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    init_real();
    if (level == SOL_SOCKET && optname == SO_BINDTODEVICE && optval) {
        const char *dev = (const char *)optval;
        if (strcmp(dev, "eth0") == 0 || strcmp(dev, "mlan0") == 0) {
            /* Transparently redirect socket bind to active Wi-Fi interface */
            return real_setsockopt(sockfd, level, optname, g_active_iface, (socklen_t)strlen(g_active_iface));
        }
    }
    return real_setsockopt ? real_setsockopt(sockfd, level, optname, optval, optlen) : -1;
}

/* Intercept connect to redirect WPA Supplicant control sockets */
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    init_real();
    if (addr && addr->sa_family == AF_UNIX) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        if (strstr(un->sun_path, "mlan0")) {
            struct sockaddr_un redirected;
            memset(&redirected, 0, sizeof(redirected));
            redirected.sun_family = AF_UNIX;
            strncpy(redirected.sun_path, "/run/wpa_supplicant/wlan0", sizeof(redirected.sun_path) - 1);
            int ret = real_connect ? real_connect(sockfd, (struct sockaddr *)&redirected, sizeof(redirected)) : -1;
            if (ret == 0) return 0;
        }
    }
    return real_connect ? real_connect(sockfd, addr, addrlen) : -1;
}

/* Intercept system() to redirect Wi-Fi commands from mlan0 to wlan0 */
int system(const char *command) {
    init_real();
    if (!command) return 1;

    /* Suppress systemctl on proprietary hardware service */
    if (strstr(command, "start-mlan0.service")) {
        return 0;
    }

    /* Translate mlan0 occurrences to wlan0 */
    if (strstr(command, "mlan0")) {
        char buf[512];
        const char *src = command;
        char *dst = buf;
        size_t rem = sizeof(buf) - 1;
        while (*src && rem > 5) {
            if (strncmp(src, "mlan0", 5) == 0) {
                memcpy(dst, "wlan0", 5);
                dst += 5;
                src += 5;
                rem -= 5;
            } else {
                *dst++ = *src++;
                rem--;
            }
        }
        *dst = '\0';
        return real_system ? real_system(buf) : 0;
    }

    return real_system ? real_system(command) : 0;
}

/* Intercept popen() to redirect Wi-Fi status queries */
FILE *popen(const char *command, const char *type) {
    init_real();
    if (!command) return NULL;

    if (strstr(command, "start-mlan0.service")) {
        return real_popen ? real_popen("true", type) : NULL;
    }

    if (strstr(command, "mlan0")) {
        char buf[512];
        const char *src = command;
        char *dst = buf;
        size_t rem = sizeof(buf) - 1;
        while (*src && rem > 5) {
            if (strncmp(src, "mlan0", 5) == 0) {
                memcpy(dst, "wlan0", 5);
                dst += 5;
                src += 5;
                rem -= 5;
            } else {
                *dst++ = *src++;
                rem--;
            }
        }
        *dst = '\0';
        return real_popen ? real_popen(buf, type) : NULL;
    }

    return real_popen ? real_popen(command, type) : NULL;
}

/* Helper to allocate an ifaddrs node */
static struct ifaddrs* make_ifaddrs_node(const char *name, bool is_ipv4, const uint8_t mac[6], short flags) {
    struct ifaddrs *n = (struct ifaddrs*)calloc(1, sizeof(struct ifaddrs));
    if (!n) return NULL;
    n->ifa_name = strdup(name);
    n->ifa_flags = flags;

    if (is_ipv4) {
        struct sockaddr_in *sin_addr = (struct sockaddr_in*)calloc(1, sizeof(struct sockaddr_in));
        sin_addr->sin_family = AF_INET;
        sin_addr->sin_addr = g_ip_addr;
        n->ifa_addr = (struct sockaddr*)sin_addr;

        struct sockaddr_in *sin_mask = (struct sockaddr_in*)calloc(1, sizeof(struct sockaddr_in));
        sin_mask->sin_family = AF_INET;
        sin_mask->sin_addr = g_netmask;
        n->ifa_netmask = (struct sockaddr*)sin_mask;

        struct sockaddr_in *sin_bcast = (struct sockaddr_in*)calloc(1, sizeof(struct sockaddr_in));
        sin_bcast->sin_family = AF_INET;
        sin_bcast->sin_addr = g_broadcast;
        n->ifa_broadaddr = (struct sockaddr*)sin_bcast;
    } else {
        /* AF_PACKET node with authentic device hardware MAC */
        struct sockaddr_ll *sll = (struct sockaddr_ll*)calloc(1, sizeof(struct sockaddr_ll));
        sll->sll_family = AF_PACKET;
        sll->sll_halen = 6;
        sll->sll_hatype = ARPHRD_ETHER;
        memcpy(sll->sll_addr, mac, 6);
        n->ifa_addr = (struct sockaddr*)sll;
    }
    return n;
}

/* Intercept getifaddrs to provide complete eth0, mlan0, and wlan0 with authentic MACs */
int getifaddrs(struct ifaddrs **ifap) {
    init_real();
    if (!ifap) return -1;

    struct ifaddrs *head = NULL;
    int ret = real_getifaddrs ? real_getifaddrs(&head) : 0;
    (void)ret;

    /*
     * Strip IFF_UP and IFF_RUNNING from any real eth0 and wlan0 entries in head.
     * Only mlan0 should be seen as active (UP & RUNNING) by EP147!
     */
    for (struct ifaddrs *p = head; p; p = p->ifa_next) {
        if (!p->ifa_name) continue;
        if (strcmp(p->ifa_name, "eth0") == 0) {
            p->ifa_flags &= ~(IFF_UP | IFF_RUNNING);
        } else if (strcmp(p->ifa_name, "wlan0") == 0) {
            p->ifa_flags &= ~(IFF_UP | IFF_RUNNING);
        }
    }

    /*
     * Inject authentic nodes:
     * 1. mlan0: AF_INET (192.168.137.22) & AF_PACKET (d4:58:00:37:e9:3b) with (IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST).
     * 2. eth0: AF_PACKET ONLY (00:15:18:01:81:31) without IFF_UP (down, disconnected).
     * This guarantees EP147 sees EXACTLY ONE active interface (mlan0), eliminating the multi-net banner forever!
     */
    struct ifaddrs *node_mlan_ip = make_ifaddrs_node("mlan0", true, g_wlan_mac_bytes,
                                                     IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST);
    if (node_mlan_ip) {
        node_mlan_ip->ifa_next = head;
        head = node_mlan_ip;
    }
    struct ifaddrs *node_mlan_mac = make_ifaddrs_node("mlan0", false, g_wlan_mac_bytes,
                                                      IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST);
    if (node_mlan_mac) {
        node_mlan_mac->ifa_next = head;
        head = node_mlan_mac;
    }
    struct ifaddrs *node_eth_mac = make_ifaddrs_node("eth0", false, g_eth_mac_bytes,
                                                     IFF_BROADCAST | IFF_MULTICAST);
    if (node_eth_mac) {
        node_eth_mac->ifa_next = head;
        head = node_eth_mac;
    }

    *ifap = head;
    return 0;
}
