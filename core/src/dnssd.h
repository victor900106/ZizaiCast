/**
 *  Copyright (C) 2012  Juho Vähä-Herttua
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

#ifndef DNSSD_H
#define DNSSD_H
#include <stdint.h>

#ifndef DNSSD_API
# define DNSSD_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DNSSD_ERROR_NOERROR       0
#define DNSSD_ERROR_HWADDRLEN     1
#define DNSSD_ERROR_OUTOFMEM      2
#define DNSSD_ERROR_LIBNOTFOUND   3
#define DNSSD_ERROR_PROCNOTFOUND  4
#define DNSSD_ERROR_BADFEATURES   5
#define DNSSD_ERROR_BADNAME       6

typedef struct dnssd_s {

    char *name;
    int name_len;

    char *hw_addr;
    int hw_addr_len;

    char *pk;

    uint32_t features1;
    uint32_t features2;

    unsigned char pin_pw;
    void *dnssd_private;
  
/* p2p support (macOS only) */
#if defined(__APPLE__) && defined(UXPLAY_HAVE_APPLE_P2P)
    int peer_to_peer;
#endif
} dnssd_t;

void *dnssd_private_init(dnssd_t *dnssd_public, int *error);
void dnssd_private_destroy(void *dnssd_private);
void dnssd_error_text(int *error, const char *appname);

DNSSD_API dnssd_t *dnssd_init(const char *name, int name_len, const char *hw_addr, int hw_addr_len, unsigned char pin_pw, int *error);

DNSSD_API int dnssd_register_raop(dnssd_t *dnssd, unsigned short port);
DNSSD_API int dnssd_register_airplay(dnssd_t *dnssd, unsigned short port);

DNSSD_API void dnssd_unregister_raop(dnssd_t *dnssd);
DNSSD_API void dnssd_unregister_airplay(dnssd_t *dnssd);

DNSSD_API const char *dnssd_get_raop_txt(dnssd_t *dnssd, int *length);
DNSSD_API const char *dnssd_get_airplay_txt(dnssd_t *dnssd, int *length);
DNSSD_API const char *dnssd_get_name(dnssd_t *dnssd, int *length);
DNSSD_API const char *dnssd_get_hw_addr(dnssd_t *dnssd, int *length);
DNSSD_API void dnssd_set_airplay_features(dnssd_t *dnssd, int bit, int val);
DNSSD_API uint64_t dnssd_get_airplay_features(dnssd_t *dnssd);
DNSSD_API void dnssd_set_pk(dnssd_t *dnssd, char * pk_str);

/* An external dns_sd.h library (Avahi's compat layer, Bonjour) hands each
   registered service a socket that the application must service with
   DNSServiceProcessResult() whenever it is readable; otherwise the library never
   reads its daemon connection and the messages queue up in the daemon (for Avahi,
   in the system dbus-daemon, without bound). DNSSD_SERVICE_RAOP / _AIRPLAY select
   the service. dnssd_get_service_fd() returns -1 when there is nothing to service
   (not registered, or the internal mdnsd backend). */
#define DNSSD_SERVICE_RAOP     0
#define DNSSD_SERVICE_AIRPLAY  1
DNSSD_API int dnssd_get_service_fd(dnssd_t *dnssd, int service);
DNSSD_API int dnssd_process_service(dnssd_t *dnssd, int service);

DNSSD_API void dnssd_destroy(dnssd_t *dnssd);

/* PM: multi-interface advertising + network-change handling (internal mdnsd
   backend only; see mdnsd/mdnsd.h). Interfaces as "name=a.b.c.d" items. */
typedef struct {
    uint32_t addr;         /* network byte order */
    unsigned int ifindex;
    char name[128];        /* UTF-8 adapter friendly name */
} dnssd_pm_iface_t;
/* interfaces the running responder advertises on */
DNSSD_API int dnssd_pm_get_interfaces(dnssd_t *dnssd, dnssd_pm_iface_t *out, int max);
/* interfaces a restart would pick now */
DNSSD_API int dnssd_pm_scan_interfaces(dnssd_pm_iface_t *out, int max);
/* reopen the mDNS socket on the current interfaces and announce (0 = ok) */
DNSSD_API int dnssd_pm_restart(dnssd_t *dnssd);
/* re-send the announcement on all advertised interfaces */
DNSSD_API void dnssd_pm_announce(dnssd_t *dnssd);
/* build the _raop._tcp TXT record (still served by GET /info "txtRAOP") without
 * registering the service; no network I/O (0 = ok) */
DNSSD_API int dnssd_pm_build_raop_txt(dnssd_t *dnssd);
/* 1 if _raop._tcp / _airplay._tcp are currently registered */
DNSSD_API int dnssd_pm_raop_registered(dnssd_t *dnssd);
DNSSD_API int dnssd_pm_airplay_registered(dnssd_t *dnssd);

/* p2p support (macOS only) */
#if defined(__APPLE__) && defined(UXPLAY_HAVE_APPLE_P2P)
DNSSD_API void dnssd_set_peer_to_peer(dnssd_t *dnssd, int enabled);
#endif

#ifdef __cplusplus
}
#endif
#endif
