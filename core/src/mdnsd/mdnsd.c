/**
 *  Copyright (C) 2026  kgbook
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include "../compat.h"
#include "mdnsd.h"

#define MDNS_ADDR4 "224.0.0.251"
#define MDNS_ADDR6 "ff02::fb"
#define MDNS_PORT 5353
#define MDNS_MAX_PACKET 1500
#define MDNS_COMBINED_PACKET_MAX 1200
#define MDNS_TTL_HOST 120

#define DNS_TYPE_A 1
#define DNS_TYPE_AAAA 28
#define DNS_TYPE_PTR 12
#define DNS_TYPE_TXT 16
#define DNS_TYPE_SRV 33
#define DNS_TYPE_ANY 255
#define DNS_CLASS_IN 1
#define DNS_FLAG_RESPONSE 0x8000
#define DNS_FLAG_AUTHORITATIVE 0x0400
#define DNS_CACHE_FLUSH 0x8000
#define DNS_UNICAST_RESPONSE 0x8000

typedef struct {
    unsigned char bytes[MDNS_MAX_PACKET];
    size_t length;
} mdns_packet_t;

typedef enum {
    MDNS_FAMILY_IPV4,
    MDNS_FAMILY_IPV6
} mdns_family_t;

struct mdnsd_s {
    char host_name[MDNSD_MAX_NAME];
    mdnsd_service_t airplay;
    mdnsd_service_t raop;

    uint32_t ipv4_addr;     /* PM: A record of the interface currently being answered on */
    unsigned char ipv6_addr[16];
    unsigned int ipv6_scope_id;
    int sock_fd4;
    int sock_fd6;
    int running;
    thread_handle_t thread;
    mutex_handle_t mutex;
    /* PM: multi-interface IPv4 */
    mdnsd_iface_t ifaces[MDNSD_MAX_IFACES];
    int n_ifaces;
#ifdef _WIN32
    LPFN_WSARECVMSG recvmsg_fn;  /* WSARecvMsg + IP_PKTINFO: arrival interface of a query */
#endif
};

static int mdns_put_u8(mdns_packet_t *packet, unsigned int value)
{
    if (packet->length + 1 > sizeof(packet->bytes)) {
        return -1;
    }
    packet->bytes[packet->length++] = (unsigned char) value;
    return 0;
}

static int mdns_put_u16(mdns_packet_t *packet, unsigned int value)
{
    if (packet->length + 2 > sizeof(packet->bytes)) {
        return -1;
    }
    packet->bytes[packet->length++] = (unsigned char) ((value >> 8) & 0xff);
    packet->bytes[packet->length++] = (unsigned char) (value & 0xff);
    return 0;
}

static int mdns_put_u32(mdns_packet_t *packet, uint32_t value)
{
    if (packet->length + 4 > sizeof(packet->bytes)) {
        return -1;
    }
    packet->bytes[packet->length++] = (unsigned char) ((value >> 24) & 0xff);
    packet->bytes[packet->length++] = (unsigned char) ((value >> 16) & 0xff);
    packet->bytes[packet->length++] = (unsigned char) ((value >> 8) & 0xff);
    packet->bytes[packet->length++] = (unsigned char) (value & 0xff);
    return 0;
}

static int mdns_put_bytes(mdns_packet_t *packet, const void *bytes, size_t length)
{
    if (packet->length + length > sizeof(packet->bytes)) {
        return -1;
    }
    memcpy(packet->bytes + packet->length, bytes, length);
    packet->length += length;
    return 0;
}

static int mdns_put_name(mdns_packet_t *packet, const char *name)
{
    const char *label = name;

    while (label && *label) {
        const char *dot = strchr(label, '.');
        size_t length = dot ? (size_t) (dot - label) : strlen(label);

        if (length > 63 || mdns_put_u8(packet, (unsigned int) length)) {
            return -1;
        }
        if (mdns_put_bytes(packet, label, length)) {
            return -1;
        }
        if (!dot) {
            break;
        }
        label = dot + 1;
    }

    return mdns_put_u8(packet, 0);
}

static int mdns_begin_rr(mdns_packet_t *packet, const char *name, unsigned int type,
                         unsigned int cls, uint32_t ttl, size_t *rdlength_pos)
{
    if (mdns_put_name(packet, name) ||
        mdns_put_u16(packet, type) ||
        mdns_put_u16(packet, cls) ||
        mdns_put_u32(packet, ttl)) {
        return -1;
    }
    *rdlength_pos = packet->length;
    return mdns_put_u16(packet, 0);
}

static void mdns_end_rr(mdns_packet_t *packet, size_t rdlength_pos)
{
    size_t rdlength = packet->length - rdlength_pos - 2;
    packet->bytes[rdlength_pos] = (unsigned char) ((rdlength >> 8) & 0xff);
    packet->bytes[rdlength_pos + 1] = (unsigned char) (rdlength & 0xff);
}

static int mdns_add_ptr(mdns_packet_t *packet, const char *name, const char *ptr,
                        uint32_t ttl)
{
    size_t rdlength_pos;
    if (mdns_begin_rr(packet, name, DNS_TYPE_PTR, DNS_CLASS_IN, ttl, &rdlength_pos) ||
        mdns_put_name(packet, ptr)) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_add_srv(mdns_packet_t *packet, const char *name, unsigned short port,
                        const char *target, uint32_t ttl)
{
    size_t rdlength_pos;
    if (mdns_begin_rr(packet, name, DNS_TYPE_SRV, DNS_CLASS_IN | DNS_CACHE_FLUSH,
                      ttl, &rdlength_pos) ||
        mdns_put_u16(packet, 0) ||
        mdns_put_u16(packet, 0) ||
        mdns_put_u16(packet, port) ||
        mdns_put_name(packet, target)) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_add_txt(mdns_packet_t *packet, const char *name, const mdnsd_txt_t *txt,
                        uint32_t ttl)
{
    size_t rdlength_pos;
    if (mdns_begin_rr(packet, name, DNS_TYPE_TXT, DNS_CLASS_IN | DNS_CACHE_FLUSH,
                      ttl, &rdlength_pos) ||
        mdns_put_bytes(packet, txt->bytes, (size_t) txt->length)) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_add_a(mdns_packet_t *packet, const char *name, uint32_t addr,
                      uint32_t ttl)
{
    size_t rdlength_pos;
    if (!addr) {
        return 0;
    }
    if (mdns_begin_rr(packet, name, DNS_TYPE_A, DNS_CLASS_IN | DNS_CACHE_FLUSH,
                      ttl, &rdlength_pos) ||
        mdns_put_bytes(packet, &addr, sizeof(addr))) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_has_ipv6_addr(const unsigned char addr[16])
{
    static const unsigned char zero[16] = {0};
    return memcmp(addr, zero, sizeof(zero)) != 0;
}

static int mdns_add_aaaa(mdns_packet_t *packet, const char *name,
                         const unsigned char addr[16], uint32_t ttl)
{
    size_t rdlength_pos;

    if (!mdns_has_ipv6_addr(addr)) {
        return 0;
    }
    if (mdns_begin_rr(packet, name, DNS_TYPE_AAAA,
                      DNS_CLASS_IN | DNS_CACHE_FLUSH, ttl, &rdlength_pos) ||
        mdns_put_bytes(packet, addr, 16)) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_add_host_records(mdnsd_t *mdnsd, mdns_packet_t *packet,
                                 uint32_t ttl, unsigned int *answers,
                                 mdns_family_t family)
{
    /*
     * Treat the IPv4 and IPv6 sockets as logical mDNS interfaces. This keeps
     * AirPlay clients from seeing cross-family host records in one response
     * and opening two TCP connections for the same service.
     */
    if (family == MDNS_FAMILY_IPV4) {
        if (mdns_add_a(packet, mdnsd->host_name, mdnsd->ipv4_addr, ttl)) {
            return -1;
        }
        if (mdnsd->ipv4_addr) {
            (*answers)++;
        }
    } else {
        if (mdns_add_aaaa(packet, mdnsd->host_name, mdnsd->ipv6_addr, ttl)) {
            return -1;
        }
        if (mdns_has_ipv6_addr(mdnsd->ipv6_addr)) {
            (*answers)++;
        }
    }

    return 0;
}

static uint16_t mdns_read_u16(const unsigned char *bytes)
{
    return (uint16_t) (((uint16_t) bytes[0] << 8) | bytes[1]);
}

static int mdns_read_name(const unsigned char *packet, size_t packet_len,
                          size_t *offset, char *name, size_t name_len)
{
    size_t pos = *offset;
    size_t out = 0;
    int jumped = 0;
    int jumps = 0;

    while (pos < packet_len) {
        unsigned int length = packet[pos++];

        if (length == 0) {
            if (!jumped) {
                *offset = pos;
            }
            if (out == 0) {
                if (name_len < 2) {
                    return -1;
                }
                name[out++] = '.';
            }
            name[out] = '\0';
            return 0;
        }

        if ((length & 0xc0) == 0xc0) {
            unsigned int ptr;
            if (pos >= packet_len || ++jumps > 16) {
                return -1;
            }
            ptr = ((length & 0x3f) << 8) | packet[pos++];
            if (ptr >= packet_len) {
                return -1;
            }
            if (!jumped) {
                *offset = pos;
            }
            pos = ptr;
            jumped = 1;
            continue;
        }

        if ((length & 0xc0) || pos + length > packet_len) {
            return -1;
        }
        if (out) {
            if (out + 1 >= name_len) {
                return -1;
            }
            name[out++] = '.';
        }
        if (out + length >= name_len) {
            return -1;
        }
        memcpy(name + out, packet + pos, length);
        out += length;
        pos += length;
    }

    return -1;
}

static int mdns_name_equals(const char *left, const char *right)
{
    while (*left && *right) {
        if (tolower((unsigned char) *left) != tolower((unsigned char) *right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int mdns_query_type_matches(uint16_t query_type, uint16_t record_type)
{
    return query_type == record_type || query_type == DNS_TYPE_ANY;
}

static uint32_t mdns_get_default_ipv4(void)
{
    uint32_t addr = 0;
#ifdef _WIN32
    ULONG buflen = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = NULL;
    DWORD dwRetVal = 0;

    pAddresses = (IP_ADAPTER_ADDRESSES *) malloc(buflen);
    if (!pAddresses) {
        return 0;
    }
    dwRetVal = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, NULL, pAddresses, &buflen);
    if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
        free(pAddresses);
        pAddresses = (IP_ADAPTER_ADDRESSES *) malloc(buflen); 
        if (!pAddresses) {
            return 0;
        }
        dwRetVal = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, NULL, pAddresses, &buflen);
    }
    
    if (dwRetVal == NO_ERROR) {
        PIP_ADAPTER_ADDRESSES pCurr = pAddresses;
        while (pCurr) {
            // Precise target filters to bypass the Hyper-V virtual adapter loopback capture
            if (pCurr->OperStatus == IfOperStatusUp && 
                pCurr->IfType != IF_TYPE_SOFTWARE_LOOPBACK &&
                pCurr->FirstGatewayAddress != NULL && 
                wcsstr(pCurr->Description, L"Hyper-V") == NULL &&
                wcsstr(pCurr->Description, L"Virtual") == NULL &&
                wcsstr(pCurr->Description, L"WSL") == NULL &&
                wcsstr(pCurr->Description, L"vEthernet") == NULL) {
                
                if (pCurr->FirstUnicastAddress != NULL) {
                    struct sockaddr_in *sockaddr_ipv4 = (struct sockaddr_in *)pCurr->FirstUnicastAddress->Address.lpSockaddr;
                    addr = sockaddr_ipv4->sin_addr.s_addr;
                    break; 
                }
            }
            pCurr = pCurr->Next;
        }
    }
    free(pAddresses);
#else    
    int fd;
    struct sockaddr_in remote;
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        return 0;
    }

    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_ADDR4, &remote.sin_addr);

    if (connect(fd, (struct sockaddr *) &remote, sizeof(remote)) == 0 &&
        getsockname(fd, (struct sockaddr *) &local, &local_len) == 0) {
        addr = local.sin_addr.s_addr;
    }

    CLOSESOCKET(fd);
#endif
    //printf("local ipv4 address %d.%d.%d.%d\n",  (addr & 0xff), ((addr >> 8) & 0xff),
    //       ((addr >> 16) & 0xff),((addr >> 24) & 0xff));
    
    return addr;
}

static int mdns_get_default_ipv6(unsigned char addr[16], unsigned int *scope_id)
{
#ifdef WIN32
    (void) addr;
    (void) scope_id;
    return 0;
#else
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;
    int found = 0;

    memset(addr, 0, 16);
    *scope_id = 0;

    if (getifaddrs(&ifaddr) == -1) {
        return 0;
    }

    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        struct sockaddr_in6 *sin6;
        unsigned char *bytes;

        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET6 ||
            !(ifa->ifa_flags & IFF_UP) ||
            !(ifa->ifa_flags & IFF_MULTICAST) ||
            (ifa->ifa_flags & IFF_LOOPBACK)) {
            continue;
        }

        sin6 = (struct sockaddr_in6 *) ifa->ifa_addr;
        bytes = sin6->sin6_addr.s6_addr;
        if (IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr) ||
            IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) {
            continue;
        }

        if (!found || IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
            memcpy(addr, bytes, 16);
            *scope_id = sin6->sin6_scope_id;
            if (!*scope_id) {
                *scope_id = if_nametoindex(ifa->ifa_name);
            }
            found = 1;
            if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    if (!*scope_id) {
        memset(addr, 0, 16);
        return 0;
    }
    return found;
#endif
}

/* PM: interface selection for multi-interface advertising (see mdnsd.h) */
#ifdef _WIN32
static int mdns_wcs_contains_ci(const wchar_t *hay, const wchar_t *needle)
{
    size_t n = wcslen(needle);
    if (!hay) {
        return 0;
    }
    for (; *hay; hay++) {
        if (_wcsnicmp(hay, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static int mdns_adapter_is_virtual(const IP_ADAPTER_ADDRESSES *a)
{
    /* Mobile Hotspot ("Microsoft Wi-Fi Direct Virtual Adapter") is a real radio
     * link that phones join: keep it. */
    static const wchar_t *real_patterns[] = { L"Wi-Fi Direct", L"Hosted Network", NULL };
    static const wchar_t *virtual_patterns[] = {
        L"Hyper-V", L"vEthernet", L"WSL", L"Virtual", L"VirtualBox", L"VMware", L"VMnet",
        L"Tailscale", L"WireGuard", L"Wintun", L"Surfshark", L"TAP-", L"TAP ", L"OpenVPN",
        L"ZeroTier", L"NordLynx", L"ProtonVPN", L"Npcap", L"Loopback", L"Bluetooth",
        L"Docker", L"Kernel Debug", L"Teredo", L"isatap", NULL };
    int i;
    for (i = 0; real_patterns[i]; i++) {
        if (mdns_wcs_contains_ci(a->Description, real_patterns[i]) ||
            mdns_wcs_contains_ci(a->FriendlyName, real_patterns[i])) {
            return 0;
        }
    }
    if (a->IfType == IF_TYPE_PROP_VIRTUAL || a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PPP) {
        return 1;
    }
    for (i = 0; virtual_patterns[i]; i++) {
        if (mdns_wcs_contains_ci(a->Description, virtual_patterns[i]) ||
            mdns_wcs_contains_ci(a->FriendlyName, virtual_patterns[i])) {
            return 1;
        }
    }
    return 0;
}
#endif

int mdnsd_scan_interfaces(mdnsd_iface_t *out, int max)
{
    int n = 0;
#ifdef _WIN32
    mdnsd_iface_t all[MDNSD_MAX_IFACES * 2];
    unsigned long metric[MDNSD_MAX_IFACES * 2];
    int n_all = 0;
    int n_real = 0;
    ULONG buflen = 16 * 1024;
    IP_ADAPTER_ADDRESSES *addrs = NULL;
    DWORD rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; tries++) {
        free(addrs);
        addrs = (IP_ADAPTER_ADDRESSES *) malloc(buflen);
        if (!addrs) {
            return 0;
        }
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                  GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, addrs, &buflen);
    }
    if (rc != NO_ERROR) {
        free(addrs);
        return 0;
    }
    for (IP_ADAPTER_ADDRESSES *a = addrs; a && n_all < MDNSD_MAX_IFACES * 2; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            (a->Flags & IP_ADAPTER_NO_MULTICAST) || !(a->Flags & IP_ADAPTER_IPV4_ENABLED)) {
            continue;
        }
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            const struct sockaddr_in *sin = (const struct sockaddr_in *) u->Address.lpSockaddr;
            if (!sin || sin->sin_family != AF_INET) {
                continue;
            }
            uint32_t host = ntohl(sin->sin_addr.s_addr);
            if ((host >> 24) == 127 || (host >> 16) == 0xA9FE /* 169.254 link-local */ || host == 0 ||
                u->DadState != IpDadStatePreferred) {
                continue;
            }
            mdnsd_iface_t *f = &all[n_all];
            memset(f, 0, sizeof(*f));
            f->addr = sin->sin_addr.s_addr;
            {
                ULONG mask = 0;
                ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &mask);
                f->mask = (uint32_t) mask;
            }
            f->ifindex = a->IfIndex;
            f->has_gateway = a->FirstGatewayAddress != NULL;
            /* test hook (tools/pm_probe/net_check.py): PM_MDNS_ALLOW_VIRTUAL=1
             * treats VPN/VM adapters like real ones */
            f->is_virtual = mdns_adapter_is_virtual(a) && !getenv("PM_MDNS_ALLOW_VIRTUAL");
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, f->name, sizeof(f->name), NULL, NULL);
            metric[n_all] = a->Ipv4Metric;
            if (!f->is_virtual) {
                n_real++;
            }
            n_all++;
            break; /* one address per adapter */
        }
    }
    free(addrs);
    /* order: real before virtual, gateway before none, then route metric */
    for (int i = 1; i < n_all; i++) {
        for (int j = i; j > 0; j--) {
            mdnsd_iface_t *x = &all[j - 1], *y = &all[j];
            int swap = (x->is_virtual != y->is_virtual) ? x->is_virtual
                     : (x->has_gateway != y->has_gateway) ? y->has_gateway
                     : metric[j - 1] > metric[j];
            if (!swap) {
                break;
            }
            mdnsd_iface_t t = *x; *x = *y; *y = t;
            unsigned long tm = metric[j - 1]; metric[j - 1] = metric[j]; metric[j] = tm;
        }
    }
    for (int i = 0; i < n_all && n < max; i++) {
        /* virtual adapters only when no real one is up, and then only those
         * with a default gateway (upstream fallback behaviour) */
        if (all[i].is_virtual && (n_real > 0 || !all[i].has_gateway)) {
            continue;
        }
        out[n++] = all[i];
    }
#else
    uint32_t addr = mdns_get_default_ipv4();
    if (addr && max > 0) {
        memset(&out[0], 0, sizeof(out[0]));
        out[0].addr = addr;
        out[0].mask = 0xffffffffu;
        snprintf(out[0].name, sizeof(out[0].name), "default");
        n = 1;
    }
#endif
    return n;
}

/* PM: index of the advertised interface a query arrived on (ifindex from
 * IP_PKTINFO if known, else the subnet that contains the sender), -1 if it
 * is not one of ours. Caller holds the mutex. */
static int mdns_find_iface_locked(const mdnsd_t *mdnsd, unsigned int ifindex, uint32_t from_addr)
{
    int i;
    if (ifindex) {
        for (i = 0; i < mdnsd->n_ifaces; i++) {
            if (mdnsd->ifaces[i].ifindex == ifindex) {
                return i;
            }
        }
        return -1;
    }
    for (i = 0; i < mdnsd->n_ifaces; i++) {
        if (((from_addr ^ mdnsd->ifaces[i].addr) & mdnsd->ifaces[i].mask) == 0) {
            return i;
        }
    }
    return mdnsd->n_ifaces == 1 ? 0 : -1;
}

/* PM: following multicasts leave through interface i and carry its A record */
static void mdns_use_iface_locked(mdnsd_t *mdnsd, int i)
{
    struct in_addr iface;
    if (i < 0 || i >= mdnsd->n_ifaces) {
        return;
    }
    iface.s_addr = mdnsd->ifaces[i].addr;
    mdnsd->ipv4_addr = iface.s_addr;
    if (mdnsd->sock_fd4 != -1) {
        setsockopt(mdnsd->sock_fd4, IPPROTO_IP, IP_MULTICAST_IF, (const char *) &iface, sizeof(iface));
    }
}

/* PM: one socket bound to *:5353 that joins the mDNS group on every selected
 * interface (upstream: on one interface). Updates mdnsd->ifaces to the
 * interfaces actually joined; sets mdnsd->sock_fd4. */
static int mdns_open_socket4(mdnsd_t *mdnsd)
{
    int fd;
    int one = 1;
    unsigned char ttl = 255;
    unsigned char loop = 1;
    struct sockaddr_in addr;
    struct ip_mreq mreq;
    int joined = 0;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        return -SOCKET_GET_ERROR();
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *) &one, sizeof(one));
#endif
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *) &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *) &loop, sizeof(loop));
#ifdef _WIN32
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, (const char *) &one, sizeof(one));
#endif

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        int error = SOCKET_GET_ERROR();
        CLOSESOCKET(fd);
        return -error;
    }

    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET, MDNS_ADDR4, &mreq.imr_multiaddr);
    for (int i = 0; i < mdnsd->n_ifaces; i++) {
        mreq.imr_interface.s_addr = mdnsd->ifaces[i].addr;
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *) &mreq, sizeof(mreq)) == 0) {
            mdnsd->ifaces[joined++] = mdnsd->ifaces[i];
        }
    }
    mdnsd->n_ifaces = joined;
    if (!joined) {
        /* no usable interface (yet): default membership; if even that fails
         * (no network at all) keep the socket anyway, a later mdnsd_restart()
         * after a network change brings it up */
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *) &mreq, sizeof(mreq));
    }
    mdnsd->sock_fd4 = fd;
    mdns_use_iface_locked(mdnsd, 0);

#ifdef _WIN32
    {
        GUID guid = WSAID_WSARECVMSG;
        DWORD bytes = 0;
        mdnsd->recvmsg_fn = NULL;
        if (WSAIoctl((SOCKET) fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
                     &mdnsd->recvmsg_fn, sizeof(mdnsd->recvmsg_fn), &bytes, NULL, NULL) == SOCKET_ERROR) {
            mdnsd->recvmsg_fn = NULL;
        }
    }
#endif
    return fd;
}

static int mdns_open_socket6(unsigned int scope_id)
{
    int fd;
    int one = 1;
    int hops = 255;
    unsigned int loop = 1;
    struct sockaddr_in6 addr;
    struct ipv6_mreq mreq;

    if (!scope_id) {
        return -1;
    }

    fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd == -1) {
        return -SOCKET_GET_ERROR();
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *) &one, sizeof(one));
#endif
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &one, sizeof(one));
    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char *) &hops, sizeof(hops));
    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (const char *) &loop, sizeof(loop));
    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, (const char *) &scope_id, sizeof(scope_id));

    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(MDNS_PORT);
    addr.sin6_addr = in6addr_any;

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        int error = SOCKET_GET_ERROR();
        CLOSESOCKET(fd);
        return -error;
    }

    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET6, MDNS_ADDR6, &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = scope_id;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                   (const char *) &mreq, sizeof(mreq)) == -1) {
        int error = SOCKET_GET_ERROR();
        CLOSESOCKET(fd);
        return -error;
    }

    return fd;
}

static int mdns_begin_response(mdns_packet_t *packet, size_t *count_pos)
{
    memset(packet, 0, sizeof(*packet));
    if (mdns_put_u16(packet, 0) ||
        mdns_put_u16(packet, DNS_FLAG_RESPONSE | DNS_FLAG_AUTHORITATIVE) ||
        mdns_put_u16(packet, 0)) {
        return -1;
    }
    *count_pos = packet->length;
    return mdns_put_u16(packet, 0) ||
           mdns_put_u16(packet, 0) ||
           mdns_put_u16(packet, 0);
}

static void mdns_finish_response(mdns_packet_t *packet, size_t count_pos,
                                 unsigned int answers)
{
    packet->bytes[count_pos] = (unsigned char) ((answers >> 8) & 0xff);
    packet->bytes[count_pos + 1] = (unsigned char) (answers & 0xff);
}

static int mdns_add_service_records(mdns_packet_t *packet,
                                    const mdnsd_service_t *service,
                                    const char *host_name, uint32_t ttl)
{
    if (!service->registered) {
        return 0;
    }

    return mdns_add_ptr(packet, "_services._dns-sd._udp.local", service->type, ttl) ||
           mdns_add_ptr(packet, service->type, service->name, ttl) ||
           mdns_add_srv(packet, service->name, service->port, host_name,
                        ttl ? MDNS_TTL_HOST : 0) ||
           mdns_add_txt(packet, service->name, &service->txt, ttl);
}

static int mdns_build_response(mdnsd_t *mdnsd, mdns_packet_t *packet,
                               const mdnsd_service_t *service,
                               int include_host, uint32_t ttl,
                               mdns_family_t family)
{
    size_t count_pos;
    unsigned int answers = 0;
    size_t before;

    if (mdns_begin_response(packet, &count_pos)) {
        return -1;
    }

    if (service && service->registered) {
        before = packet->length;
        if (mdns_add_service_records(packet, service, mdnsd->host_name, ttl)) {
            return -1;
        }
        if (packet->length != before) {
            answers += 4;
        }
    }

    if (include_host &&
        mdns_add_host_records(mdnsd, packet, ttl ? MDNS_TTL_HOST : 0,
                              &answers, family)) {
        return -1;
    }

    mdns_finish_response(packet, count_pos, answers);
    return answers ? 0 : -1;
}

static int mdns_build_combined_response(mdnsd_t *mdnsd, mdns_packet_t *packet,
                                        int include_airplay, int include_raop,
                                        int include_host, uint32_t ttl,
                                        mdns_family_t family)
{
    size_t count_pos;
    unsigned int answers = 0;
    size_t before;

    if (mdns_begin_response(packet, &count_pos)) {
        return -1;
    }

    if (include_airplay && mdnsd->airplay.registered) {
        before = packet->length;
        if (mdns_add_service_records(packet, &mdnsd->airplay, mdnsd->host_name,
                                     ttl)) {
            return -1;
        }
        if (packet->length != before) {
            answers += 4;
        }
    }

    if (include_raop && mdnsd->raop.registered) {
        before = packet->length;
        if (mdns_add_service_records(packet, &mdnsd->raop, mdnsd->host_name,
                                     ttl)) {
            return -1;
        }
        if (packet->length != before) {
            answers += 4;
        }
    }

    if (include_host &&
        mdns_add_host_records(mdnsd, packet, ttl ? MDNS_TTL_HOST : 0,
                              &answers, family)) {
        return -1;
    }

    if (!answers || packet->length > MDNS_COMBINED_PACKET_MAX) {
        return -1;
    }

    mdns_finish_response(packet, count_pos, answers);
    return 0;
}

static void mdns_send_packet4(mdnsd_t *mdnsd, const mdns_packet_t *packet,
                              const struct sockaddr_in *to)
{
    struct sockaddr_in addr;

    if (mdnsd->sock_fd4 == -1 || !packet->length) {
        return;
    }

    if (to) {
        addr = *to;
    } else {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(MDNS_PORT);
        inet_pton(AF_INET, MDNS_ADDR4, &addr.sin_addr);
    }

    sendto(mdnsd->sock_fd4, (const char *) packet->bytes, (int) packet->length, 0,
           (struct sockaddr *) &addr, sizeof(addr));
}

static void mdns_send_packet6(mdnsd_t *mdnsd, const mdns_packet_t *packet,
                              const struct sockaddr_in6 *to)
{
    struct sockaddr_in6 addr;

    if (mdnsd->sock_fd6 == -1 || !packet->length) {
        return;
    }

    if (to) {
        addr = *to;
    } else {
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(MDNS_PORT);
        addr.sin6_scope_id = mdnsd->ipv6_scope_id;
        inet_pton(AF_INET6, MDNS_ADDR6, &addr.sin6_addr);
    }

    sendto(mdnsd->sock_fd6, (const char *) packet->bytes, (int) packet->length, 0,
           (struct sockaddr *) &addr, sizeof(addr));
}

static void mdns_send_service_locked(mdnsd_t *mdnsd, const mdnsd_service_t *service,
                                     int include_host, uint32_t ttl,
                                     mdns_family_t family, const void *to)
{
    mdns_packet_t packet;

    if (service->registered &&
        !mdns_build_response(mdnsd, &packet, service, include_host, ttl,
                             family)) {
        if (family == MDNS_FAMILY_IPV4) {
            mdns_send_packet4(mdnsd, &packet, (const struct sockaddr_in *) to);
        } else {
            mdns_send_packet6(mdnsd, &packet, (const struct sockaddr_in6 *) to);
        }
    }
}

static void mdns_send_response_locked(mdnsd_t *mdnsd, int include_airplay,
                                      int include_raop, int include_host,
                                      uint32_t ttl, mdns_family_t family,
                                      const void *to)
{
    mdns_packet_t packet;

    if (include_airplay && include_raop &&
        mdnsd->airplay.registered && mdnsd->raop.registered &&
        !mdns_build_combined_response(mdnsd, &packet, include_airplay,
                                      include_raop, include_host, ttl,
                                      family)) {
        if (family == MDNS_FAMILY_IPV4) {
            mdns_send_packet4(mdnsd, &packet, (const struct sockaddr_in *) to);
        } else {
            mdns_send_packet6(mdnsd, &packet, (const struct sockaddr_in6 *) to);
        }
        return;
    }

    if (include_airplay) {
        mdns_send_service_locked(mdnsd, &mdnsd->airplay, include_host, ttl,
                                 family, to);
    }
    if (include_raop) {
        mdns_send_service_locked(mdnsd, &mdnsd->raop, include_host, ttl,
                                 family, to);
    }
    if (include_host && !include_airplay && !include_raop &&
        !mdns_build_response(mdnsd, &packet, NULL, 1, ttl, family)) {
        if (family == MDNS_FAMILY_IPV4) {
            mdns_send_packet4(mdnsd, &packet, (const struct sockaddr_in *) to);
        } else {
            mdns_send_packet6(mdnsd, &packet, (const struct sockaddr_in6 *) to);
        }
    }
}

static void mdns_announce_locked(mdnsd_t *mdnsd, uint32_t ttl)
{
    /* PM: once per advertised interface, each with its own A record */
    if (mdnsd->n_ifaces > 0) {
        for (int i = 0; i < mdnsd->n_ifaces; i++) {
            mdns_use_iface_locked(mdnsd, i);
            mdns_send_response_locked(mdnsd, 1, 1, 1, ttl, MDNS_FAMILY_IPV4, NULL);
        }
        mdns_use_iface_locked(mdnsd, 0);
    } else {
        mdns_send_response_locked(mdnsd, 1, 1, 1, ttl, MDNS_FAMILY_IPV4, NULL);
    }
    mdns_send_response_locked(mdnsd, 1, 1, 1, ttl, MDNS_FAMILY_IPV6, NULL);
}

static int mdns_question_matches_service(const mdnsd_t *mdnsd, const char *name,
                                         uint16_t type, int *airplay, int *raop,
                                         int *host)
{
    if (mdns_name_equals(name, "_services._dns-sd._udp.local") &&
        mdns_query_type_matches(type, DNS_TYPE_PTR)) {
        if (mdnsd->airplay.registered) {
            *airplay = 1;
        }
        if (mdnsd->raop.registered) {
            *raop = 1;
        }
        return *airplay || *raop;
    }

    if (mdnsd->airplay.registered &&
        ((mdns_name_equals(name, mdnsd->airplay.type) &&
          mdns_query_type_matches(type, DNS_TYPE_PTR)) ||
         (mdns_name_equals(name, mdnsd->airplay.name) &&
          (mdns_query_type_matches(type, DNS_TYPE_TXT) ||
           mdns_query_type_matches(type, DNS_TYPE_SRV))))) {
        *airplay = 1;
        *host = 1;
        return 1;
    }

    if (mdnsd->raop.registered &&
        ((mdns_name_equals(name, mdnsd->raop.type) &&
          mdns_query_type_matches(type, DNS_TYPE_PTR)) ||
         (mdns_name_equals(name, mdnsd->raop.name) &&
          (mdns_query_type_matches(type, DNS_TYPE_TXT) ||
           mdns_query_type_matches(type, DNS_TYPE_SRV))))) {
        *raop = 1;
        *host = 1;
        return 1;
    }

    if (mdns_name_equals(name, mdnsd->host_name) &&
        (mdns_query_type_matches(type, DNS_TYPE_A) ||
         mdns_query_type_matches(type, DNS_TYPE_AAAA))) {
        *host = 1;
        return 1;
    }

    return 0;
}

static void mdns_handle_query(mdnsd_t *mdnsd, const unsigned char *bytes,
                              size_t length, mdns_family_t family,
                              const void *from, int from_port,
                              unsigned int ifindex)
{
    uint16_t flags;
    uint16_t qdcount;
    size_t offset = 12;
    int include_airplay = 0;
    int include_raop = 0;
    int include_host = 0;
    int unicast_reply = 0;

    if (length < 12) {
        return;
    }

    flags = mdns_read_u16(bytes + 2);
    if (flags & 0x8000) {
        return;
    }

    qdcount = mdns_read_u16(bytes + 4);

    MUTEX_LOCK(mdnsd->mutex);
    /* PM: answer on (and with the A record of) the interface the query came
     * in on; ignore queries from interfaces we do not advertise on (VPN/VM) */
    if (family == MDNS_FAMILY_IPV4 && mdnsd->n_ifaces > 0) {
        int idx = mdns_find_iface_locked(mdnsd, ifindex,
                                         from ? ((const struct sockaddr_in *) from)->sin_addr.s_addr : 0);
        if (idx < 0) {
            MUTEX_UNLOCK(mdnsd->mutex);
            return;
        }
        mdns_use_iface_locked(mdnsd, idx);
    }
    for (uint16_t i = 0; i < qdcount; i++) {
        char name[MDNSD_MAX_NAME];
        uint16_t type;
        uint16_t cls;

        if (mdns_read_name(bytes, length, &offset, name, sizeof(name)) ||
            offset + 4 > length) {
            break;
        }
        type = mdns_read_u16(bytes + offset);
        cls = mdns_read_u16(bytes + offset + 2);
        offset += 4;

        if (cls & DNS_UNICAST_RESPONSE) {
            unicast_reply = 1;
        }
        mdns_question_matches_service(mdnsd, name, type, &include_airplay,
                                      &include_raop, &include_host);
    }

    if (include_airplay || include_raop || include_host) {
        mdns_send_response_locked(mdnsd, include_airplay, include_raop,
                                  include_host, MDNSD_TTL_SERVICE, family,
                                  NULL);
        if (from && (unicast_reply || from_port != htons(MDNS_PORT))) {
            mdns_send_response_locked(mdnsd, include_airplay, include_raop,
                                      include_host, MDNSD_TTL_SERVICE, family,
                                      from);
        }
    }
    MUTEX_UNLOCK(mdnsd->mutex);
}

static THREAD_RETVAL mdns_thread(void *arg)
{
    mdnsd_t *mdnsd = arg;

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        int fd4;
        int fd6;
        int max_fd = -1;
        int running;
        int ret;

        MUTEX_LOCK(mdnsd->mutex);
        fd4 = mdnsd->sock_fd4;
        fd6 = mdnsd->sock_fd6;
        running = mdnsd->running;
        MUTEX_UNLOCK(mdnsd->mutex);

        if (!running || (fd4 == -1 && fd6 == -1)) {
            break;
        }

        FD_ZERO(&rfds);
        if (fd4 != -1) {
            FD_SET(fd4, &rfds);
            max_fd = fd4;
        }
        if (fd6 != -1) {
            FD_SET(fd6, &rfds);
            if (fd6 > max_fd) {
                max_fd = fd6;
            }
        }
        tv.tv_sec = 0;
        tv.tv_usec = 250000;

        ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret > 0 && fd4 != -1 && FD_ISSET(fd4, &rfds)) {
            unsigned char buffer[MDNS_MAX_PACKET];
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            unsigned int ifindex = 0;
            int received = -1;
#ifdef _WIN32
            /* PM: WSARecvMsg + IP_PKTINFO tells which interface the query arrived on */
            LPFN_WSARECVMSG recvmsg_fn;
            MUTEX_LOCK(mdnsd->mutex);
            recvmsg_fn = mdnsd->recvmsg_fn;
            MUTEX_UNLOCK(mdnsd->mutex);
            if (recvmsg_fn) {
                union {
                    char buf[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))];
                    WSACMSGHDR align;
                } control;
                WSABUF wbuf;
                WSAMSG msg;
                DWORD got = 0;
                wbuf.buf = (char *) buffer;
                wbuf.len = sizeof(buffer);
                memset(&msg, 0, sizeof(msg));
                memset(&from, 0, sizeof(from));
                msg.name = (LPSOCKADDR) &from;
                msg.namelen = sizeof(from);
                msg.lpBuffers = &wbuf;
                msg.dwBufferCount = 1;
                msg.Control.buf = control.buf;
                msg.Control.len = sizeof(control.buf);
                if (recvmsg_fn((SOCKET) fd4, &msg, &got, NULL, NULL) == 0) {
                    received = (int) got;
                    for (WSACMSGHDR *c = WSA_CMSG_FIRSTHDR(&msg); c; c = WSA_CMSG_NXTHDR(&msg, c)) {
                        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
                            ifindex = ((IN_PKTINFO *) WSA_CMSG_DATA(c))->ipi_ifindex;
                        }
                    }
                }
            } else
#endif
            {
                received = (int) recvfrom(fd4, (char *) buffer, sizeof(buffer), 0,
                                          (struct sockaddr *) &from, &from_len);
            }
            if (received > 0) {
                mdns_handle_query(mdnsd, buffer, (size_t) received,
                                  MDNS_FAMILY_IPV4, &from, from.sin_port, ifindex);
            }
        }
        if (ret > 0 && fd6 != -1 && FD_ISSET(fd6, &rfds)) {
            unsigned char buffer[MDNS_MAX_PACKET];
            struct sockaddr_in6 from;
            socklen_t from_len = sizeof(from);
            int received = (int) recvfrom(fd6, (char *) buffer, sizeof(buffer), 0,
                                          (struct sockaddr *) &from, &from_len);
            if (received > 0) {
                mdns_handle_query(mdnsd, buffer, (size_t) received,
                                  MDNS_FAMILY_IPV6, &from, from.sin6_port, 0);
            }
        }
    }

    return NULL;
}

int mdnsd_txt_add(mdnsd_txt_t *txt, const char *key, const char *value)
{
    char item[256];
    int length = snprintf(item, sizeof(item), "%s=%s", key, value ? value : "");

    if (length < 0 || length > 255 ||
        txt->length + length + 1 > (int) sizeof(txt->bytes)) {
        return -1;
    }

    txt->bytes[txt->length++] = (unsigned char) length;
    memcpy(txt->bytes + txt->length, item, (size_t) length);
    txt->length += length;
    return 0;
}

mdnsd_t *mdnsd_init(const char *host_name)
{
    mdnsd_t *mdnsd = calloc(1, sizeof(*mdnsd));
    if (!mdnsd) {
        return NULL;
    }

    snprintf(mdnsd->host_name, sizeof(mdnsd->host_name), "%s",
             host_name && *host_name ? host_name : "UxPlay.local");
    mdnsd->sock_fd4 = -1;
    mdnsd->sock_fd6 = -1;
    MUTEX_CREATE(mdnsd->mutex);
    return mdnsd;
}

void mdnsd_destroy(mdnsd_t *mdnsd)
{
    if (!mdnsd) {
        return;
    }

    mdnsd_stop(mdnsd);
    MUTEX_DESTROY(mdnsd->mutex);
    free(mdnsd);
}

int mdnsd_start(mdnsd_t *mdnsd)
{
    if (!mdnsd) {
        return -1;
    }

    MUTEX_LOCK(mdnsd->mutex);
    if (mdnsd->running) {
        MUTEX_UNLOCK(mdnsd->mutex);
        return 0;
    }

    int error4 = 0;
    int error6 = 0;

    /* PM: all suitable IPv4 interfaces instead of mdns_get_default_ipv4() */
    mdnsd->n_ifaces = mdnsd_scan_interfaces(mdnsd->ifaces, MDNSD_MAX_IFACES);
    mdnsd->ipv4_addr = mdnsd->n_ifaces ? mdnsd->ifaces[0].addr : 0;
    mdns_get_default_ipv6(mdnsd->ipv6_addr, &mdnsd->ipv6_scope_id);

    error4 = mdns_open_socket4(mdnsd);
    if (error4 >= 0) {
        error4 = 0;
    } else {
        mdnsd->sock_fd4 = -1;
        mdnsd->n_ifaces = 0;
    }

    mdnsd->sock_fd6 = mdns_open_socket6(mdnsd->ipv6_scope_id);
    if (mdnsd->sock_fd6 < 0) {
        error6 = mdnsd->sock_fd6;
        mdnsd->sock_fd6 = -1;
    }

    if (mdnsd->sock_fd4 == -1 && mdnsd->sock_fd6 == -1) {
        MUTEX_UNLOCK(mdnsd->mutex);
        return error4 ? error4 : error6;
    }

    mdnsd->running = 1;
    THREAD_CREATE(mdnsd->thread, mdns_thread, mdnsd);
    if (!mdnsd->thread) {
        if (mdnsd->sock_fd4 != -1) {
            CLOSESOCKET(mdnsd->sock_fd4);
            mdnsd->sock_fd4 = -1;
        }
        if (mdnsd->sock_fd6 != -1) {
            CLOSESOCKET(mdnsd->sock_fd6);
            mdnsd->sock_fd6 = -1;
        }
        mdnsd->running = 0;
        MUTEX_UNLOCK(mdnsd->mutex);
        return -1;
    }

    MUTEX_UNLOCK(mdnsd->mutex);
    return 0;
}

void mdnsd_stop(mdnsd_t *mdnsd)
{
    int fd4 = -1;
    int fd6 = -1;
    thread_handle_t thread = 0;

    if (!mdnsd) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    if (mdnsd->running) {
        mdnsd->running = 0;
        fd4 = mdnsd->sock_fd4;
        fd6 = mdnsd->sock_fd6;
        thread = mdnsd->thread;
        mdnsd->sock_fd4 = -1;
        mdnsd->sock_fd6 = -1;
        mdnsd->thread = 0;
    }
    MUTEX_UNLOCK(mdnsd->mutex);

    if (fd4 != -1) {
        CLOSESOCKET(fd4);
    }
    if (fd6 != -1) {
        CLOSESOCKET(fd6);
    }
    if (thread) {
        THREAD_JOIN(thread);
    }
}

void mdnsd_set_services(mdnsd_t *mdnsd, const mdnsd_service_t *airplay,
                        const mdnsd_service_t *raop)
{
    if (!mdnsd) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    if (airplay) {
        mdnsd->airplay = *airplay;
    } else {
        memset(&mdnsd->airplay, 0, sizeof(mdnsd->airplay));
    }
    if (raop) {
        mdnsd->raop = *raop;
    } else {
        memset(&mdnsd->raop, 0, sizeof(mdnsd->raop));
    }
    MUTEX_UNLOCK(mdnsd->mutex);
}

void mdnsd_announce(mdnsd_t *mdnsd, uint32_t ttl)
{
    if (!mdnsd) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    mdns_announce_locked(mdnsd, ttl);
    MUTEX_UNLOCK(mdnsd->mutex);
}

void mdnsd_goodbye(mdnsd_t *mdnsd, const mdnsd_service_t *service)
{
    if (!mdnsd || !service) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    /* PM: goodbye on every advertised interface */
    for (int i = 0; i < (mdnsd->n_ifaces ? mdnsd->n_ifaces : 1); i++) {
        mdns_use_iface_locked(mdnsd, i);
        mdns_send_service_locked(mdnsd, service, 0, 0, MDNS_FAMILY_IPV4, NULL);
    }
    mdns_use_iface_locked(mdnsd, 0);
    mdns_send_service_locked(mdnsd, service, 0, 0, MDNS_FAMILY_IPV6, NULL);
    MUTEX_UNLOCK(mdnsd->mutex);
}

/* PM: see mdnsd.h */
int mdnsd_get_interfaces(mdnsd_t *mdnsd, mdnsd_iface_t *out, int max)
{
    int n = 0;
    if (!mdnsd) {
        return 0;
    }
    MUTEX_LOCK(mdnsd->mutex);
    if (mdnsd->running) {
        for (; n < mdnsd->n_ifaces && n < max; n++) {
            out[n] = mdnsd->ifaces[n];
        }
    }
    MUTEX_UNLOCK(mdnsd->mutex);
    return n;
}

int mdnsd_restart(mdnsd_t *mdnsd)
{
    int ret;
    if (!mdnsd) {
        return -1;
    }
    mdnsd_stop(mdnsd);
    ret = mdnsd_start(mdnsd);
    if (!ret) {
        mdnsd_announce(mdnsd, MDNSD_TTL_SERVICE);
    }
    return ret;
}
