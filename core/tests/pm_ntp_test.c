/* PM: offline check of the NTP clock mapping (raop_ntp.c) that turns the
 * sender timestamps of video frames and audio packets into local time
 * (video_decode_struct / audio_decode_struct ntp_time_local, which
 * pm::AirPlayServer hands to the sinks as ntpLocalNs).
 *
 * A fake "iPhone" answers raop_ntp's timing requests on 127.0.0.1 with a
 * clock that counts from an arbitrary boot epoch (+ the 1900->1970 seconds,
 * like iOS) and runs SKEW_PPM fast. Mirror-frame timestamps (seconds since
 * boot, no 1900 epoch) and audio sync timestamps (with the 1900 epoch) for a
 * known capture instant are then converted and compared with that instant.
 *
 *   pm_ntp_test     exit code 0 = all mapped within 3 ms
 * Upstream (client_time_received never set) maps everything to 0.
 * GPL-3.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "raop.h"
#include "raop_ntp.h"
#include "logger.h"

#define SEC 1000000000ULL
#define SECONDS_1900_TO_1970 2208988800ULL

static const int64_t BOOT_AGO_NS = 3723LL * 1000000000LL;  /* phone booted 1 h 2 min 3 s ago */
static const double SKEW_PPM = 40.0;                       /* phone clock runs 40 ppm fast */
static int64_t g_boot_local_ns;                            /* local time of the phone's boot */
static volatile LONG g_quit = 0;
static volatile LONG g_requests = 0;

/* phone clock: ns since boot */
static uint64_t phone_ns_at(uint64_t local_ns) {
    double since = (double) ((int64_t) local_ns - g_boot_local_ns);
    return (uint64_t) (since * (1.0 + SKEW_PPM * 1e-6));
}

static uint64_t ns_to_q32(uint64_t ns) {
    uint64_t s = ns / SEC, f = ns % SEC;
    return (s << 32) | ((f << 32) / SEC);
}

static void put_be64(unsigned char *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) { p[i] = (unsigned char) v; v >>= 8; }
}

static DWORD WINAPI fake_phone(LPVOID arg) {
    SOCKET s = (SOCKET) (uintptr_t) arg;
    DWORD tmo = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &tmo, sizeof(tmo));
    while (!g_quit) {
        unsigned char req[64], resp[32];
        struct sockaddr_in from;
        int fl = sizeof(from);
        int n = recvfrom(s, (char *) req, sizeof(req), 0, (struct sockaddr *) &from, &fl);
        if (n < 32) continue;
        uint64_t t2 = phone_ns_at(raop_ntp_get_local_time());
        Sleep(2);  /* "processing" + asymmetric path */
        memset(resp, 0, sizeof(resp));
        resp[0] = 0x80; resp[1] = 0xd3; resp[3] = 0x07;
        memcpy(resp + 8, req + 24, 8);  /* origin = client's transmit time */
        put_be64(resp + 16, ns_to_q32(t2) + (SECONDS_1900_TO_1970 << 32));
        put_be64(resp + 24, ns_to_q32(phone_ns_at(raop_ntp_get_local_time())) + (SECONDS_1900_TO_1970 << 32));
        sendto(s, (const char *) resp, sizeof(resp), 0, (struct sockaddr *) &from, fl);
        InterlockedIncrement(&g_requests);
    }
    return 0;
}

/* raop_rtp_mirror.c: 128-byte header bytes 8..15 = Q32.32 since boot (LE) */
static uint64_t map_video(raop_ntp_t *ntp, uint64_t capture_local) {
    uint64_t raw = raop_ntp_adjust_remote_timestamp_offset(ntp, ns_to_q32(phone_ns_at(capture_local)), true);
    return raop_ntp_convert_remote_time(ntp, raop_ntp_timestamp_to_nano_seconds(ntp, raw));
}

/* raop_rtp.c sync packet: bytes 8..15 = NTP time incl. 1900 epoch (BE) */
static uint64_t map_audio(raop_ntp_t *ntp, uint64_t capture_local) {
    uint64_t ts = ns_to_q32(phone_ns_at(capture_local)) + (SECONDS_1900_TO_1970 << 32);
    uint64_t raw = raop_ntp_adjust_remote_timestamp_offset(ntp, ts, false);
    return raop_ntp_convert_remote_time(ntp, raop_ntp_timestamp_to_nano_seconds(ntp, raw));
}

static int check(const char *what, raop_ntp_t *ntp, uint64_t (*map)(raop_ntp_t *, uint64_t)) {
    double worst = 0;
    int zero = 0;
    for (int i = 0; i < 20; i++) {
        uint64_t now = raop_ntp_get_local_time();
        uint64_t capture = now - 60 * 1000000ULL;  /* frame captured 60 ms ago */
        uint64_t local = map(ntp, capture);
        if (!local) { zero++; continue; }
        double err = ((double) (int64_t) (local - capture)) / 1e6;
        if (err < 0) err = -err;
        if (err > worst) worst = err;
        Sleep(5);
    }
    int ok = !zero && worst < 3.0;
    printf("  %-28s %s (worst error %.3f ms%s)\n", what, ok ? "OK  " : "FAIL", worst,
           zero ? ", some mapped to 0 = no NTP mapping" : "");
    return ok;
}

int main(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    ntp_global_init();
    g_boot_local_ns = (int64_t) raop_ntp_get_local_time() - BOOT_AGO_NS;

    SOCKET ps = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bind(ps, (struct sockaddr *) &a, sizeof(a));
    int al = sizeof(a);
    getsockname(ps, (struct sockaddr *) &a, &al);
    HANDLE th = CreateThread(NULL, 0, fake_phone, (LPVOID) (uintptr_t) ps, 0, NULL);

    logger_t *logger = logger_init();
    logger_set_level(logger, LOGGER_WARNING);
    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    timing_protocol_t tp = NTP;
    raop_ntp_t *ntp = raop_ntp_init(logger, &cbs, "127.0.0.1", 4, ntohs(a.sin_port), &tp);
    if (!ntp) { printf("raop_ntp_init failed\n"); return 2; }

    int ok = 1;
    /* the first mirror frame arrives before any NTP exchange: it latches the
     * fixed offset (with 80 ms of transit in it) and is not mapped yet */
    uint64_t v0 = map_video(ntp, raop_ntp_get_local_time() - 80 * 1000000ULL);
    printf("before NTP sync: video frame mapped to %llu (expected 0 = not synced)\n", (unsigned long long) v0);
    ok &= (v0 == 0);

    unsigned short lport = 0;
    raop_ntp_start(ntp, &lport);
    Sleep(1500);  /* initial burst: one exchange every 250 ms */
    int64_t off = 0;
    double delay = 0;
    raop_ntp_get_sync_params(ntp, &off, &delay);
    printf("after %ld NTP exchanges: offset %.3f ms, delay %.3f ms (phone clock %+.0f ppm, booted %.0f s ago)\n",
           g_requests, off / 1e6, delay * 1e3, SKEW_PPM, BOOT_AGO_NS / 1e9);
    ok &= check("video (mirror header ts)", ntp, map_video);
    ok &= check("audio (RTP sync ntp ts)", ntp, map_audio);

    raop_ntp_stop(ntp);
    raop_ntp_destroy(ntp);
    g_quit = 1;
    WaitForSingleObject(th, 2000);
    closesocket(ps);
    logger_destroy(logger);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
