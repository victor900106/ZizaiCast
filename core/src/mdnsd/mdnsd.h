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

#ifndef MDNSD_H
#define MDNSD_H

#include <stdint.h>

#define MDNSD_MAX_NAME 256
#define MDNSD_TXT_MAX 512
#define MDNSD_TTL_SERVICE 4500

typedef struct {
    unsigned char bytes[MDNSD_TXT_MAX];
    int length;
} mdnsd_txt_t;

typedef struct {
    char type[MDNSD_MAX_NAME];
    char name[MDNSD_MAX_NAME];
    unsigned short port;
    mdnsd_txt_t txt;
    int registered;
} mdnsd_service_t;

typedef struct mdnsd_s mdnsd_t;

int mdnsd_txt_add(mdnsd_txt_t *txt, const char *key, const char *value);

mdnsd_t *mdnsd_init(const char *host_name);
void mdnsd_destroy(mdnsd_t *mdnsd);

int mdnsd_start(mdnsd_t *mdnsd);
void mdnsd_stop(mdnsd_t *mdnsd);

void mdnsd_set_services(mdnsd_t *mdnsd, const mdnsd_service_t *airplay,
                        const mdnsd_service_t *raop);
void mdnsd_announce(mdnsd_t *mdnsd, uint32_t ttl);
void mdnsd_goodbye(mdnsd_t *mdnsd, const mdnsd_service_t *service);

/* PM: multi-interface IPv4 advertising (Windows). Upstream picks ONE IPv4
 * interface; here the responder joins 224.0.0.251 on every suitable interface
 * (up, multicast-capable, not loopback / link-local 169.254 / VPN / VM
 * adapter), announces on each of them with that interface's own A record and
 * answers a query with the address of the interface it arrived on. */
#define MDNSD_MAX_IFACES 16
typedef struct {
    uint32_t addr;          /* network byte order */
    uint32_t mask;          /* network byte order */
    unsigned int ifindex;
    int has_gateway;
    int is_virtual;         /* VPN / VM adapter: only used if nothing else is up */
    char name[128];         /* UTF-8 friendly name, e.g. "Wi-Fi" */
} mdnsd_iface_t;

/* Interfaces a (re)start would use now; returns the count. */
int mdnsd_scan_interfaces(mdnsd_iface_t *out, int max);
/* Interfaces the running responder is joined on; returns the count. */
int mdnsd_get_interfaces(mdnsd_t *mdnsd, mdnsd_iface_t *out, int max);
/* Close and reopen the socket on the current interfaces, then announce the
 * registered services (no-op announce if none). Returns 0 on success. */
int mdnsd_restart(mdnsd_t *mdnsd);

#endif
