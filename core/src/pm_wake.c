/* PM 0.8.3: see pm_wake.h. GPL-3.0-or-later. */
#include "pm_wake.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
typedef SOCKET pm_sock_t;
#define PM_CLOSE closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
typedef int pm_sock_t;
#define PM_CLOSE close
#endif
#include <string.h>

int pm_wake_open(pm_wake_t *w) {
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    w->fd = -1;
    pm_sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return -1;
#else
    if (s < 0) return -1;
#endif
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* loopback only: no firewall prompt, not reachable from the LAN */
    a.sin_port = 0;
    if (bind(s, (struct sockaddr *) &a, sizeof(a)) != 0 || getsockname(s, (struct sockaddr *) &a, &al) != 0 ||
        connect(s, (struct sockaddr *) &a, sizeof(a)) != 0) {  /* connected to itself: only its own bytes arrive */
        PM_CLOSE(s);
        return -1;
    }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL no_reset = FALSE;  /* no WSAECONNRESET from a stray ICMP */
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &no_reset, sizeof(no_reset), NULL, 0, &bytes, NULL, NULL);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    w->fd = (int) s;
    return 0;
}

void pm_wake_close(pm_wake_t *w) {
    if (w->fd != -1) {
        PM_CLOSE((pm_sock_t) w->fd);
        w->fd = -1;
    }
}

void pm_wake_signal(pm_wake_t *w) {
    if (w->fd != -1) {
        char c = 1;
        send((pm_sock_t) w->fd, &c, 1, 0);  /* a full queue already means "wake up" */
    }
}

void pm_wake_drain(pm_wake_t *w) {
    if (w->fd != -1) {
        char buf[64];
        for (int i = 0; i < 1024; i++) {  /* nonblocking: ends at "would block" */
            if (recv((pm_sock_t) w->fd, buf, sizeof(buf), 0) <= 0) break;
        }
    }
}
