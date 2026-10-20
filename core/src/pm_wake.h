/* PM 0.8.3: a wake-up event for the socket threads (httpd, audio RTP, mirror,
 * mDNS). Upstream polled: select() with a 5 ms (RTP, mirror), 250 ms (mDNS)
 * or ~1 s (httpd) timeout, only to notice a stop request or a value set from
 * another thread (volume, flush, metadata). Here each of those threads also
 * selects on a loopback UDP socket connected to itself; pm_wake_signal()
 * sends it one byte, so the thread sleeps until there is real work.
 * If the socket cannot be created (fd == -1) the callers fall back to their
 * old polling timeout. GPL-3.0-or-later. */
#ifndef PM_WAKE_H
#define PM_WAKE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pm_wake_s {
    int fd;  /* -1 = unavailable: poll as before */
} pm_wake_t;

/* returns 0 on success; w->fd = -1 on failure */
int pm_wake_open(pm_wake_t *w);
void pm_wake_close(pm_wake_t *w);
/* any thread; harmless after a failed open */
void pm_wake_signal(pm_wake_t *w);
/* the owning thread, after select() reported the fd readable */
void pm_wake_drain(pm_wake_t *w);

#ifdef __cplusplus
}
#endif

#endif
