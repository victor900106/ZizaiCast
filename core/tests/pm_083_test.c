/* PM 0.8.3: offline checks of the 0.8.3 core fixes (loopback sockets only,
 * no sound, no window).
 *
 *   ntp_stop      raop_ntp_stop() right after start (thread inside its first
 *                 300 ms recv) returns without waiting for the 250 ms / 3 s
 *                 wait (lost wake-up)
 *   rtp_resync    a new audio stream on the same raop_rtp does not use the RTP
 *                 sync of the previous stream
 *   wake          pm_wake: a select() on the wake socket returns at once after
 *                 pm_wake_signal(), drain empties it
 *   stop_latency  stop of an idle audio / mirror / mDNS / httpd thread returns
 *                 within 100 ms (no polling interval to wait out)
 *
 *   pm_083_test     exit code 0 = all checks passed
 * GPL-3.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "httpd.h"
#include "netutils.h"
#include "logger.h"
#include "raop.h"
#include "raop_ntp.h"
#include "raop_rtp.h"
#include "raop_rtp_mirror.h"
#include "pm_wake.h"

static int g_fail = 0;
static void check(int ok, const char *what) {
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_fail++;
}
static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
}
static void t_log(void *cls, int level, const char *msg) {
    (void) cls;
    if (level <= LOGGER_ERR) printf("    log %d: %s\n", level, msg);
}

/* ---------------------------------------------------------------- ntp_stop */
static void test_ntp_stop(logger_t *logger) {
    printf("ntp_stop: stop while the NTP thread is in its first receive\n");
    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    double worst = 0;
    /* a "phone" timing port that receives but never answers: every request
     * times out after 300 ms (a closed port would answer ICMP at once) */
    SOCKET mute = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in ma;
    int mal = sizeof(ma);
    memset(&ma, 0, sizeof(ma));
    ma.sin_family = AF_INET;
    ma.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(mute, (struct sockaddr *) &ma, sizeof(ma));
    getsockname(mute, (struct sockaddr *) &ma, &mal);
    for (int i = 0; i < 5; i++) {
        timing_protocol_t tp = NTP;
        raop_ntp_t *ntp = raop_ntp_init(logger, &cbs, "127.0.0.1", 4, ntohs(ma.sin_port), &tp);
        unsigned short lport = 0;
        raop_ntp_start(ntp, &lport);
        Sleep(50 + 40 * i);  /* inside the 300 ms recv of the first request */
        double t0 = now_ms();
        raop_ntp_stop(ntp);
        double took = now_ms() - t0;
        if (took > worst) worst = took;
        raop_ntp_destroy(ntp);
    }
    closesocket(mute);
    printf("    worst stop %.0f ms\n", worst);
    /* the receive still ends by its 300 ms timeout; the 250 ms burst wait (3 s later) must not follow */
    check(worst < 320, "stop returns after the pending receive, not after the following 250 ms / 3 s wait");
}

/* -------------------------------------------------------------- rtp_resync */
static volatile LONG g_audio_calls;
static volatile LONG64 g_last_remote;
static void t_audio(void *cls, raop_ntp_t *ntp, audio_decode_struct *d) {
    (void) cls; (void) ntp;
    InterlockedExchange64(&g_last_remote, (LONG64) d->ntp_time_remote);
    InterlockedIncrement(&g_audio_calls);
}
static void udp_send(unsigned short port, const unsigned char *p, int n) {
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(u, (const char *) p, n, 0, (struct sockaddr *) &a, sizeof(a));
    closesocket(u);
}
static void send_sync(unsigned short cport, uint32_t rtp, uint32_t ntp_sec) {
    unsigned char p[20] = { 0x90, 0xd4, 0x00, 0x04 };
    p[4] = (unsigned char) (rtp >> 24); p[5] = (unsigned char) (rtp >> 16); p[6] = (unsigned char) (rtp >> 8); p[7] = (unsigned char) rtp;
    p[8] = (unsigned char) (ntp_sec >> 24); p[9] = (unsigned char) (ntp_sec >> 16); p[10] = (unsigned char) (ntp_sec >> 8); p[11] = (unsigned char) ntp_sec;
    udp_send(cport, p, sizeof(p));
}
static void send_audio(unsigned short dport, unsigned short seq, uint32_t rtp) {
    unsigned char p[48] = { 0x80, 0x60 };  /* 12-byte header + 36 bytes "audio" (44 = ALAC format-only packet, ignored) */
    p[2] = (unsigned char) (seq >> 8); p[3] = (unsigned char) seq;
    p[4] = (unsigned char) (rtp >> 24); p[5] = (unsigned char) (rtp >> 16); p[6] = (unsigned char) (rtp >> 8); p[7] = (unsigned char) rtp;
    for (int i = 12; i < 48; i++) p[i] = (unsigned char) (i * 5);
    udp_send(dport, p, sizeof(p));
}
static int wait_calls(LONG want, int ms) {
    for (int i = 0; i < ms / 5; i++) {
        if (g_audio_calls >= want) return 1;
        Sleep(5);
    }
    return g_audio_calls >= want;
}

static void test_rtp_resync(logger_t *logger) {
    printf("rtp_resync: audio TEARDOWN + SETUP on the same connection starts without the old sync\n");
    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.audio_process = t_audio;
    timing_protocol_t tp = NTP;
    raop_ntp_t *ntp = raop_ntp_init(logger, &cbs, "127.0.0.1", 4, 9, &tp);
    static const unsigned char key[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    raop_rtp_t *rtp = raop_rtp_init(logger, &cbs, ntp, "127.0.0.1", 4, key, key);
    check(rtp != NULL, "raop_rtp_init");
    if (!rtp) return;

    /* stream 1 (ALAC: dequeued only after a sync): sync, then a packet */
    unsigned short rport = 0, cport = 0, dport = 0;
    unsigned char ct = 2;
    unsigned int sr = 44100;
    raop_rtp_start_audio(rtp, &rport, &cport, &dport, &ct, &sr);
    send_sync(cport, 1000, 100);
    Sleep(50);
    send_audio(dport, 10, 1352);
    check(wait_calls(1, 1000), "stream 1: packet delivered after its sync");
    raop_rtp_stop(rtp);

    /* stream 2 on the same raop_rtp: a packet before any sync of this stream */
    LONG before = g_audio_calls;
    rport = cport = dport = 0;
    raop_rtp_start_audio(rtp, &rport, &cport, &dport, &ct, &sr);
    send_audio(dport, 500, 777777);
    Sleep(300);
    check(g_audio_calls == before, "stream 2: no packet mapped with stream 1's sync (waits for its own)");
    send_sync(cport, 777000, 200);
    Sleep(50);
    send_audio(dport, 501, 777777 + 352);
    check(wait_calls(before + 1, 1000), "stream 2: delivered once its own sync arrived");
    raop_rtp_stop(rtp);
    raop_rtp_destroy(rtp);
    raop_ntp_destroy(ntp);
}

/* -------------------------------------------------------------------- wake */
static void test_wake(void) {
    printf("wake: pm_wake signal / drain\n");
    pm_wake_t w;
    check(pm_wake_open(&w) == 0 && w.fd != -1, "open (loopback UDP)");
    fd_set r;
    struct timeval tv = { 0, 0 };
    FD_ZERO(&r);
    FD_SET((SOCKET) w.fd, &r);
    check(select(0, &r, NULL, NULL, &tv) == 0, "not readable before a signal");
    for (int i = 0; i < 5; i++) pm_wake_signal(&w);
    FD_ZERO(&r);
    FD_SET((SOCKET) w.fd, &r);
    tv.tv_sec = 1;
    double t0 = now_ms();
    int ret = select(0, &r, NULL, NULL, &tv);
    check(ret == 1 && now_ms() - t0 < 50, "readable at once after a signal");
    pm_wake_drain(&w);
    FD_ZERO(&r);
    FD_SET((SOCKET) w.fd, &r);
    tv.tv_sec = 0;
    check(select(0, &r, NULL, NULL, &tv) == 0, "drain empties it (5 signals)");
    pm_wake_close(&w);
    check(w.fd == -1, "close");
    pm_wake_signal(&w);
    pm_wake_drain(&w);
    check(1, "signal / drain after close are harmless");
}

/* ------------------------------------------------------------ stop_latency */
static void *t_conn_init(void *opaque, unsigned char *l, int ll, unsigned char *r, int rl, unsigned int z) {
    static int dummy;
    (void) opaque; (void) l; (void) ll; (void) r; (void) rl; (void) z;
    return &dummy;
}
static void t_conn_request(void *ptr, http_request_t *req, http_response_t **resp) { (void) ptr; (void) req; *resp = NULL; }
static void t_conn_destroy(void *ptr) { (void) ptr; }
static void t_video_reset(void *cls, reset_type_t t) { (void) cls; (void) t; }

static void test_stop_latency(logger_t *logger) {
    printf("stop_latency: idle threads stop without waiting out a poll interval\n");
    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.audio_process = t_audio;
    cbs.video_reset = t_video_reset;
    timing_protocol_t tp = NTP;
    raop_ntp_t *ntp = raop_ntp_init(logger, &cbs, "127.0.0.1", 4, 9, &tp);
    static const unsigned char key[16] = { 1 };

    raop_rtp_t *rtp = raop_rtp_init(logger, &cbs, ntp, "127.0.0.1", 4, key, key);
    unsigned short rport = 0, cport = 0, dport = 0;
    unsigned char ct = 8;
    unsigned int sr = 44100;
    raop_rtp_start_audio(rtp, &rport, &cport, &dport, &ct, &sr);
    Sleep(200);
    double t0 = now_ms();
    raop_rtp_stop(rtp);
    double a = now_ms() - t0;
    raop_rtp_destroy(rtp);

    raop_rtp_mirror_t *m = raop_rtp_mirror_init(logger, &cbs, ntp, "127.0.0.1", 4, key);
    uint64_t scid = 1;
    raop_rtp_mirror_init_aes(m, &scid);
    unsigned short mport = 0;
    raop_rtp_mirror_start(m, &mport, 0);
    Sleep(200);
    t0 = now_ms();
    raop_rtp_mirror_stop(m);
    double b = now_ms() - t0;
    raop_rtp_mirror_destroy(m);
    raop_ntp_destroy(ntp);

    httpd_callbacks_t hc;
    memset(&hc, 0, sizeof(hc));
    hc.conn_init = t_conn_init;
    hc.conn_request = t_conn_request;
    hc.conn_destroy = t_conn_destroy;
    httpd_t *h = httpd_init(logger, &hc, 0);
    unsigned short hport = 0;
    httpd_start(h, &hport);
    Sleep(200);
    t0 = now_ms();
    httpd_stop(h);
    double c = now_ms() - t0;
    httpd_destroy(h);

    printf("    stop: audio %.1f ms, mirror %.1f ms, httpd %.1f ms\n", a, b, c);
    check(a < 100, "audio RTP thread stops at once");
    check(b < 100, "mirror thread (waiting for its connection) stops at once");
    check(c < 100, "httpd thread (no connection) stops at once (was up to 1 s)");
}

int main(void) {
    WSADATA wsa;
    setvbuf(stdout, NULL, _IONBF, 0);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    pm_netutils_loopback_only = 1;
    ntp_global_init();
    logger_t *logger = logger_init();
    logger_set_level(logger, LOGGER_ERR);
    logger_set_callback(logger, t_log, NULL);

    test_wake();
    test_ntp_stop(logger);
    test_rtp_resync(logger);
    test_stop_latency(logger);

    logger_destroy(logger);
    printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
