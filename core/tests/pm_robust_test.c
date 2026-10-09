/* PM: offline robustness checks of the UxPlay core (no real network: every
 * socket is bound to the loopback address via pm_netutils_loopback_only, so
 * no firewall prompt).
 *
 *   httpd   accept() and select() failures do not end the listener thread;
 *           stop/start cycles; is_running reflects the real state
 *
 *   pm_robust_test     exit code 0 = all checks passed
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
#include "raop_rtp_mirror.h"
#include "dnssd.h"
#include "pm_audio_advert.h"
#include <plist/plist.h>

static void t_log(void *cls, int level, const char *msg) {
    (void) cls;
    printf("    log %d: %s\n", level, msg);
}

static int g_fail = 0;
static void check(int ok, const char *what) {
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_fail++;
}

/* ------------------------------------------------------------------ httpd */
static volatile LONG g_conn_init = 0, g_conn_destroy = 0;
static volatile LONG g_accept_fail = 0, g_select_fail = 0;
static int g_dummy;

static void *t_conn_init(void *opaque, unsigned char *l, int ll, unsigned char *r, int rl, unsigned int z) {
    (void) opaque; (void) l; (void) ll; (void) r; (void) rl; (void) z;
    InterlockedIncrement(&g_conn_init);
    return &g_dummy;
}
static void t_conn_request(void *ptr, http_request_t *req, http_response_t **resp) {
    (void) ptr; (void) req; *resp = NULL;
}
static void t_conn_destroy(void *ptr) { (void) ptr; InterlockedIncrement(&g_conn_destroy); }

static int t_accept(int server_fd, void *addr, void *addrlen) {
    if (g_accept_fail > 0) {
        InterlockedDecrement(&g_accept_fail);
        WSASetLastError(WSAECONNRESET);
        return -1;
    }
    return (int) accept(server_fd, (struct sockaddr *) addr, (int *) addrlen);
}
static int t_select(int nfds, void *rfds, void *tv) {
    if (g_select_fail > 0) {
        InterlockedDecrement(&g_select_fail);
        WSASetLastError(WSAEINVAL);
        return -1;
    }
    return select(nfds, (fd_set *) rfds, NULL, NULL, (struct timeval *) tv);
}

static SOCKET connect_loopback(unsigned short port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (struct sockaddr *) &a, sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static int wait_for(volatile LONG *v, LONG want, int ms) {
    for (int i = 0; i < ms / 10; i++) {
        if (*v >= want) return 1;
        Sleep(10);
    }
    return *v >= want;
}

static void test_httpd(logger_t *logger) {
    printf("httpd listener\n");
    httpd_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.conn_init = t_conn_init;
    cbs.conn_request = t_conn_request;
    cbs.conn_destroy = t_conn_destroy;
    httpd_t *h = httpd_init(logger, &cbs, 0);
    pm_httpd_accept_hook = t_accept;
    pm_httpd_select_hook = t_select;

    unsigned short port = 0;
    check(httpd_start(h, &port) == 1 && port != 0, "start on a loopback port");
    check(httpd_is_running(h), "running after start");

    g_accept_fail = 5;
    SOCKET c1 = connect_loopback(port);
    check(c1 != INVALID_SOCKET, "client connects");
    check(wait_for(&g_conn_init, 1, 3000), "accepted after 5 accept() failures");
    check(g_accept_fail == 0, "the failures were really hit");
    check(httpd_is_running(h), "still running after accept() failures");

    g_select_fail = 3;
    for (int i = 0; i < 300 && g_select_fail > 0; i++) Sleep(10);  /* the thread may sit in a 1 s select */
    SOCKET c2 = connect_loopback(port);
    check(wait_for(&g_conn_init, 2, 3000), "accepted after 3 select() failures");
    check(g_select_fail == 0 && httpd_is_running(h), "still running after select() failures");

    httpd_stop(h);
    check(!httpd_is_running(h), "not running after stop");
    check(g_conn_destroy == 2, "stop closed both connections");
    if (c1 != INVALID_SOCKET) closesocket(c1);
    if (c2 != INVALID_SOCKET) closesocket(c2);

    unsigned short port2 = port;
    check(httpd_start(h, &port2) == 1 && port2 == port, "restart on the same port");
    SOCKET c3 = connect_loopback(port2);
    check(wait_for(&g_conn_init, 3, 3000), "restarted listener accepts");
    httpd_stop(h);
    httpd_stop(h);  /* second stop is a no-op */
    check(!httpd_is_running(h), "stop twice is harmless");
    if (c3 != INVALID_SOCKET) closesocket(c3);
    httpd_destroy(h);
    pm_httpd_accept_hook = NULL;
    pm_httpd_select_hook = NULL;
}

/* ------------------------------------------------------- mirror thread */
static volatile LONG g_video_reset = 0, g_conn_reset = 0;
static void t_video_reset(void *cls, reset_type_t t) { (void) cls; (void) t; InterlockedIncrement(&g_video_reset); }
static void t_mirror_running(void *cls, bool r) { (void) cls; (void) r; }
static void t_conn_reset(void *cls, int reason) { (void) cls; (void) reason; InterlockedIncrement(&g_conn_reset); }

static int send_all(SOCKET s, const unsigned char *p, int n) {
    while (n > 0) {
        int r = send(s, (const char *) p, n, 0);
        if (r <= 0) return 0;
        p += r;
        n -= r;
    }
    return 1;
}

static raop_rtp_mirror_t *mirror_new(logger_t *logger, raop_ntp_t *ntp, raop_callbacks_t *cbs) {
    static const unsigned char aeskey[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    raop_rtp_mirror_t *m = raop_rtp_mirror_init(logger, cbs, ntp, "127.0.0.1", 4, aeskey);
    uint64_t scid = 1;
    if (m) raop_rtp_mirror_init_aes(m, &scid);
    return m;
}

static void test_mirror(logger_t *logger) {
    printf("mirror stream thread\n");
    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.video_reset = t_video_reset;
    cbs.mirror_video_running = t_mirror_running;
    cbs.conn_reset = t_conn_reset;
    timing_protocol_t tp = NTP;
    raop_ntp_t *ntp = raop_ntp_init(logger, &cbs, "127.0.0.1", 4, 7, &tp);
    check(ntp != NULL, "raop_ntp_init");
    if (!ntp) return;

    /* 1. the thread ends by itself (invalid packet), then stop/destroy */
    raop_rtp_mirror_t *m = mirror_new(logger, ntp, &cbs);
    unsigned short port = 0;
    raop_rtp_mirror_start(m, &port, 0);
    check(port != 0, "mirror listening on a loopback port");
    SOCKET c = connect_loopback(port);
    check(c != INVALID_SOCKET, "client connects to the mirror port");
    unsigned char hdr[128];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 0xf0; hdr[1] = 0xff; hdr[2] = 0xff; hdr[3] = 0xff;  /* payload size -16 (LE) */
    send_all(c, hdr, sizeof(hdr));
    Sleep(300);  /* the thread rejects the size and ends by itself */
    DWORD t0 = GetTickCount();
    raop_rtp_mirror_stop(m);
    check(GetTickCount() - t0 < 2000, "stop after a self-exit returns");
    SOCKET again = connect_loopback(port);
    check(again == INVALID_SOCKET, "stop closed the listening socket after a self-exit (no leaked :port)");
    if (again != INVALID_SOCKET) closesocket(again);
    closesocket(c);
    raop_rtp_mirror_destroy(m);
    check(1, "destroy after a self-exit (no use-after-free crash)");

    /* 2. stop while the thread is blocked in recv() (half a header sent) */
    m = mirror_new(logger, ntp, &cbs);
    port = 0;
    raop_rtp_mirror_start(m, &port, 0);
    c = connect_loopback(port);
    send_all(c, hdr, 40);
    Sleep(300);
    t0 = GetTickCount();
    raop_rtp_mirror_stop(m);
    DWORD took = GetTickCount() - t0;
    printf("    stop took %lu ms\n", (unsigned long) took);
    check(took < 1000, "stop wakes a blocking recv (no 2 min hang)");
    again = connect_loopback(port);
    check(again == INVALID_SOCKET, "listening socket closed");
    if (again != INVALID_SOCKET) closesocket(again);
    closesocket(c);
    raop_rtp_mirror_destroy(m);

    /* 3. the client closes the stream; a second client can connect */
    m = mirror_new(logger, ntp, &cbs);
    port = 0;
    raop_rtp_mirror_start(m, &port, 0);
    c = connect_loopback(port);
    Sleep(100);
    closesocket(c);
    Sleep(200);
    c = connect_loopback(port);
    check(c != INVALID_SOCKET, "reconnect after the client closed the stream");
    Sleep(100);
    raop_rtp_mirror_stop(m);
    if (c != INVALID_SOCKET) closesocket(c);
    raop_rtp_mirror_destroy(m);

    raop_ntp_destroy(ntp);
}

/* ------------------------------------------------------- PIN pairing */
static char g_pins[8][8];
static volatile LONG g_pin_count = 0;
static char g_last_pin_log[256];

static void t_display_pin(void *cls, char *pin) {
    (void) cls;
    LONG i = InterlockedIncrement(&g_pin_count) - 1;
    if (i < 8) snprintf(g_pins[i], sizeof(g_pins[i]), "%s", pin ? pin : "");
}
/* PM: "request: ..." INFO lines, collected from the httpd thread */
static CRITICAL_SECTION g_req_cs;
static char g_req_log[8192];
static volatile LONG g_req_lines;

static void t_raop_log(void *cls, int level, const char *msg) {
    (void) cls;
    if (!strncmp(msg, "request: ", 9)) {
        EnterCriticalSection(&g_req_cs);
        size_t l = strlen(g_req_log);
        snprintf(g_req_log + l, sizeof(g_req_log) - l, "%s\n", msg);
        InterlockedIncrement(&g_req_lines);
        LeaveCriticalSection(&g_req_cs);
        printf("    %s\n", msg);
    }
    if (strstr(msg, "CLIENT MUST NOW ENTER PIN")) snprintf(g_last_pin_log, sizeof(g_last_pin_log), "%s", msg);
    if (level <= LOGGER_ERR) printf("    log %d: %s\n", level, msg);
}
static void t_audio(void *cls, raop_ntp_t *ntp, audio_decode_struct *d) { (void) cls; (void) ntp; (void) d; }
static void t_feedback(void *cls) { (void) cls; }
static double t_client_volume(void *cls) { (void) cls; return -15.0; }
static void t_video(void *cls, raop_ntp_t *ntp, video_decode_struct *d) { (void) cls; (void) ntp; (void) d; }

/* connect from a chosen loopback source address (a different "phone") */
static SOCKET connect_from(const char *src, unsigned short port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    inet_pton(AF_INET, src, &a.sin_addr);
    bind(s, (struct sockaddr *) &a, sizeof(a));
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (struct sockaddr *) &a, sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    DWORD tmo = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &tmo, sizeof(tmo));
    return s;
}

/* one RTSP request; returns the status code (-1 = no response / closed) and
 * the body (caller frees) */
static int rtsp(SOCKET s, const char *url, const char *body, int len, char **resp_body, int *resp_len) {
    static int cseq = 1;
    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
                      "POST %s RTSP/1.0\r\nCSeq: %d\r\nContent-Type: application/x-apple-binary-plist\r\n"
                      "Content-Length: %d\r\nUser-Agent: AirPlay/800\r\n\r\n", url, cseq++, len);
    if (!send_all(s, (const unsigned char *) hdr, hl) || (len && !send_all(s, (const unsigned char *) body, len)))
        return -1;
    char buf[8192];
    int got = 0, header_end = -1, content_len = 0;
    while (got < (int) sizeof(buf) - 1) {
        int r = recv(s, buf + got, (int) sizeof(buf) - 1 - got, 0);
        if (r <= 0) break;
        got += r;
        buf[got] = 0;
        char *e = strstr(buf, "\r\n\r\n");
        if (e && header_end < 0) {
            header_end = (int) (e - buf) + 4;
            char *cl = strstr(buf, "Content-Length:");
            content_len = cl && cl < e ? atoi(cl + 15) : 0;
        }
        if (header_end >= 0 && got >= header_end + content_len) break;
    }
    if (header_end < 0) return -1;
    int status = 0;
    sscanf(buf, "RTSP/1.0 %d", &status);
    if (resp_body) {
        *resp_len = content_len;
        *resp_body = malloc(content_len + 1);
        memcpy(*resp_body, buf + header_end, content_len);
    }
    return status;
}

static int plist_post(SOCKET s, const char *url, plist_t dict, char **resp_body, int *resp_len) {
    char *bin = NULL;
    uint32_t bl = 0;
    plist_to_bin(dict, &bin, &bl);
    plist_free(dict);
    int st = rtsp(s, url, bin, (int) bl, resp_body, resp_len);
    plist_mem_free(bin);
    return st;
}

static int step1(SOCKET s) {
    plist_t d = plist_new_dict();
    plist_dict_set_item(d, "method", plist_new_string("pin"));
    plist_dict_set_item(d, "user", plist_new_string("AA:BB:CC:DD:EE:01"));
    char *rb = NULL;
    int rl = 0;
    int st = plist_post(s, "/pair-setup-pin", d, &rb, &rl);
    free(rb);
    return st;
}
static int step2(SOCKET s, int pk_len, int proof_len) {
    char pk[512], proof[128];
    for (int i = 0; i < (int) sizeof(pk); i++) pk[i] = (char) (i * 7 + 3);
    for (int i = 0; i < (int) sizeof(proof); i++) proof[i] = (char) (i * 13 + 1);
    plist_t d = plist_new_dict();
    plist_dict_set_item(d, "pk", plist_new_data(pk, pk_len));
    plist_dict_set_item(d, "proof", plist_new_data(proof, proof_len));
    return plist_post(s, "/pair-setup-pin", d, NULL, NULL);
}
static int step3(SOCKET s, int epk_len, int tag_len) {
    char epk[64] = { 1 }, tag[64] = { 2 };
    plist_t d = plist_new_dict();
    plist_dict_set_item(d, "epk", plist_new_data(epk, epk_len));
    plist_dict_set_item(d, "authTag", plist_new_data(tag, tag_len));
    return plist_post(s, "/pair-setup-pin", d, NULL, NULL);
}

static raop_t *raop_new(bool pin, unsigned short *port) {
    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.audio_process = t_audio;
    cbs.video_process = t_video;
    cbs.display_pin = t_display_pin;
    cbs.conn_feedback = t_feedback;
    cbs.audio_set_client_volume = t_client_volume;
    raop_t *r = raop_init(&cbs);
    if (!r) return NULL;
    raop_set_log_callback(r, t_raop_log, NULL);
    raop_set_log_level(r, LOGGER_INFO);
    if (raop_init2(r, 1, "02:11:22:33:44:55", "")) return NULL;
    if (pin) raop_set_plist(r, "pin", 0);
    *port = 0;
    if (raop_start_httpd(r, port) < 0) return NULL;
    return r;
}

static void test_pairing(void) {
    printf("PIN pairing (requirePin on)\n");
    unsigned short port = 0;
    raop_t *r = raop_new(true, &port);
    check(r != NULL, "receiver with PIN pairing on a loopback port");
    if (!r) return;

    SOCKET a = connect_from("127.0.0.1", port);
    check(step1(a) == 470, "pair-setup-pin without PAIR-PIN-START is refused (was: paired with \"0000\")");
    closesocket(a);

    a = connect_from("127.0.0.1", port);
    check(rtsp(a, "/pair-pin-start", NULL, 0, NULL, NULL) == 200, "PAIR-PIN-START");
    closesocket(a);  /* the iPhone closes this connection, shows the prompt ... */
    a = connect_from("127.0.0.1", port);
    check(rtsp(a, "/pair-pin-start", NULL, 0, NULL, NULL) == 200, "PAIR-PIN-START again (cancel + retry)");
    closesocket(a);
    check(g_pin_count == 2 && !strcmp(g_pins[0], g_pins[1]) && strlen(g_pins[0]) == 4,
          "the same PIN is shown for the retry (log 10-08 19:30: 9935 then 2978)");
    printf("    PINs shown: %s, %s\n", g_pins[0], g_pins[1]);

    SOCKET b = connect_from("127.0.0.2", port);  /* another phone */
    check(rtsp(b, "/pair-pin-start", NULL, 0, NULL, NULL) == 200, "PAIR-PIN-START from a second phone");
    closesocket(b);
    check(g_pin_count == 3, "second phone gets its own PIN shown");

    a = connect_from("127.0.0.1", port);
    check(step1(a) == 200, "pair-setup-pin step 1 with the shown PIN (first phone's PIN still valid)");
    check(step2(a, 256, 65) == 470, "step 2 with an over-long proof is refused");
    check(step2(a, 256, 10) == 470, "step 2 with a short proof is refused");
    check(step2(a, 256, 20) == 470, "wrong proof is refused (1st wrong try)");
    check(step2(a, 256, 20) == 470, "step 2 again without step 1 (no SRP state): refused, no crash");
    check(step3(a, 32, 16) == 470, "step 3 without a validated step 2: refused, no crash");
    check(step3(a, 8, 4) == 470, "step 3 with short epk/authTag: refused, no over-read");
    check(step1(a) == 200 && step2(a, 256, 20) == 470, "2nd wrong try");
    check(step1(a) == 200 && step2(a, 256, 20) == 470, "3rd wrong try");
    check(step1(a) == 470, "after 3 wrong tries the PIN is used up");
    closesocket(a);
    a = connect_from("127.0.0.1", port);
    check(rtsp(a, "/pair-pin-start", NULL, 0, NULL, NULL) == 200 && strstr(g_last_pin_log, "(new PIN)") != NULL,
          "the next PAIR-PIN-START draws a new PIN");
    closesocket(a);
    /* oversized body: the connection is dropped, the server keeps working */
    a = connect_from("127.0.0.1", port);
    {
        const char *h = "POST /fp-setup RTSP/1.0\r\nCSeq: 99\r\nContent-Length: 20000000\r\n\r\n";
        send_all(a, (const unsigned char *) h, (int) strlen(h));
        static unsigned char chunk[65536];
        int sent = 0, ok_send = 1;
        while (sent < 17 * 1024 * 1024 && ok_send) {
            ok_send = send_all(a, chunk, sizeof(chunk));
            if (ok_send) sent += sizeof(chunk);
        }
        char tmp[16];
        int rr = ok_send ? recv(a, tmp, sizeof(tmp), 0) : 0;
        check(!ok_send || rr <= 0, "a request body over 16 MB is refused (connection dropped)");
    }
    closesocket(a);
    a = connect_from("127.0.0.1", port);
    check(rtsp(a, "/pair-pin-start", NULL, 0, NULL, NULL) == 200, "server still answers afterwards");
    closesocket(a);
    raop_stop_httpd(r);
    raop_destroy(r);

    printf("PIN pairing off\n");
    r = raop_new(false, &port);
    check(r != NULL, "receiver without PIN pairing");
    if (!r) return;
    LONG before = g_pin_count;
    a = connect_from("127.0.0.1", port);
    check(rtsp(a, "/pair-pin-start", NULL, 0, NULL, NULL) == 470 && g_pin_count == before,
          "PAIR-PIN-START refused, no PIN shown");
    check(step2(a, 256, 20) == 470, "pair-setup-pin step 2 refused, no crash");
    check(step3(a, 32, 16) == 470, "pair-setup-pin step 3 refused, no crash");
    check(step1(a) == 470, "pair-setup-pin step 1 refused");
    closesocket(a);
    raop_stop_httpd(r);
    raop_destroy(r);
}

static int req_log_has(const char *needle) {
    EnterCriticalSection(&g_req_cs);
    int r = strstr(g_req_log, needle) != NULL;
    LeaveCriticalSection(&g_req_cs);
    return r;
}
static int wait_req_log(const char *needle) {
    for (int i = 0; i < 200; i++) {
        if (req_log_has(needle)) return 1;
        Sleep(10);
    }
    return 0;
}
/* send one raw request, return the bytes of the first answer (<= 0 = none) */
static int send_raw(SOCKET s, const char *req, char *resp, int resp_size) {
    if (!send_all(s, (const unsigned char *) req, (int) strlen(req))) return -1;
    int r = recv(s, resp, resp_size - 1, 0);
    resp[r > 0 ? r : 0] = 0;
    return r;
}

static void test_request_log(void) {
    printf("INFO request log (one line per request, body redacted, chatter limited)\n");
    unsigned short port = 0;
    raop_t *r = raop_new(false, &port);
    check(r != NULL, "receiver on a loopback port");
    if (!r) return;
    char resp[2048];
    SOCKET a = connect_from("127.0.0.1", port);
    send_raw(a, "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: AirPlay/1005.12.1\r\n\r\n", resp, sizeof(resp));
    check(!strncmp(resp, "RTSP/1.0 200", 12), "OPTIONS answered 200");
    check(wait_req_log("request: OPTIONS * RTSP/1.0 from 127.0.0.1 UA=\"AirPlay/1005.12.1\" body=0 B -> 200 OK"),
          "OPTIONS logged with client, User-Agent and the status sent");
    LONG before = g_req_lines;
    for (int i = 0; i < 5; i++) {
        char req[160];
        snprintf(req, sizeof(req), "POST /feedback RTSP/1.0\r\nCSeq: %d\r\n\r\n", 2 + i);
        send_raw(a, req, resp, sizeof(resp));
    }
    Sleep(50);
    check(g_req_lines - before == 1, "5 x POST /feedback -> 1 line");
    closesocket(a);

    SOCKET b = connect_from("127.0.0.1", port);
    DWORD tmo = 500;
    setsockopt(b, SOL_SOCKET, SO_RCVTIMEO, (const char *) &tmo, sizeof(tmo));
    int got = send_raw(b, "POST /play HTTP/1.1\r\nX-Apple-Session-ID: 1234\r\nUser-Agent: AppleCoreMedia/1.0\r\n"
                          "Content-Type: application/x-apple-binary-plist\r\nContent-Length: 11\r\n\r\nSECRETBODY!",
                       resp, sizeof(resp));
    check(wait_req_log("request: POST /play HTTP/1.1 from 127.0.0.1 UA=\"AppleCoreMedia/1.0\" X-Apple-Session-ID "
                       "body=11 B application/x-apple-binary-plist -> "), "POST /play (AirPlay video) logged");
    check(!req_log_has("SECRETBODY"), "request body is not logged");
    {
        char first[96];
        size_t k = 0;
        for (; got > 0 && k + 1 < sizeof(first) && resp[k] && resp[k] != '\r'; k++) first[k] = resp[k];
        first[k] = 0;
        printf("    POST /play answer: %s\n", k ? first : "(none)");
    }
    closesocket(b);
    raop_stop_httpd(r);
    raop_destroy(r);
}

/* RTSP GET /info (optionally with a bplist "qualifier"); returns the response
 * body parsed as a plist (caller frees) or NULL */
static plist_t get_info(SOCKET s, const char *qualifier) {
    static int cseq = 100;
    char *bin = NULL;
    uint32_t bl = 0;
    if (qualifier) {
        plist_t d = plist_new_dict();
        plist_t a = plist_new_array();
        plist_array_append_item(a, plist_new_string(qualifier));
        plist_dict_set_item(d, "qualifier", a);
        plist_to_bin(d, &bin, &bl);
        plist_free(d);
    }
    char hdr[256];
    int hl = qualifier
        ? snprintf(hdr, sizeof(hdr), "GET /info RTSP/1.0\r\nCSeq: %d\r\nContent-Type: application/x-apple-binary-plist\r\n"
                   "Content-Length: %u\r\n\r\n", cseq++, (unsigned) bl)
        : snprintf(hdr, sizeof(hdr), "GET /info RTSP/1.0\r\nCSeq: %d\r\n\r\n", cseq++);
    int ok = send_all(s, (const unsigned char *) hdr, hl) && (!bl || send_all(s, (const unsigned char *) bin, (int) bl));
    if (bin) plist_mem_free(bin);
    if (!ok) return NULL;
    static char buf[16384];
    int got = 0, header_end = -1, content_len = 0;
    while (got < (int) sizeof(buf) - 1) {
        int r = recv(s, buf + got, (int) sizeof(buf) - 1 - got, 0);
        if (r <= 0) break;
        got += r;
        buf[got] = 0;
        char *e = strstr(buf, "\r\n\r\n");
        if (e && header_end < 0) {
            header_end = (int) (e - buf) + 4;
            char *cl = strstr(buf, "Content-Length:");
            content_len = cl && cl < e ? atoi(cl + 15) : 0;
        }
        if (header_end >= 0 && got >= header_end + content_len) break;
    }
    if (header_end < 0 || strncmp(buf, "RTSP/1.0 200", 12) || content_len <= 0) {
        printf("    /info reply (%d bytes): %.80s\n", got, got > 0 ? buf : "(none)");
        return NULL;
    }
    plist_t root = NULL;
    plist_from_bin(buf + header_end, (uint32_t) content_len, &root);
    return root;
}

/* settings.ini airplay_advertise_audio: what each mode advertises, and that
 * the receiver still answers /info (features, txtRAOP) without _raop._tcp.
 * dnssd is never registered here (that would multicast on the real LAN):
 * airplay_server.cpp's start-up sequence is replayed up to the registration. */
static void test_audio_advert(void) {
    printf("airplay_advertise_audio A/B switch\n");
    pm_audio_advert_t p1 = pm_audio_advert_plan(1), p0 = pm_audio_advert_plan(0), p2 = pm_audio_advert_plan(2);
    pm_audio_advert_t px = pm_audio_advert_plan(7);
    check(p1.mode == 1 && p1.feature_bit9 && p1.raop_service, "1 (default): bit 9 on, _raop._tcp registered");
    check(p0.mode == 0 && !p0.feature_bit9 && !p0.raop_service, "0: bit 9 off, no _raop._tcp");
    check(p2.mode == 2 && p2.feature_bit9 && !p2.raop_service, "2: bit 9 on, no _raop._tcp");
    check(px.mode == 1 && px.feature_bit9 && px.raop_service, "unknown value -> default 1");

    for (int mode = 0; mode <= 1; mode++) {
        const pm_audio_advert_t plan = pm_audio_advert_plan(mode);
        const char hw[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
        int err = 0;
        dnssd_t *d = dnssd_init("PMTest", 6, hw, 6, 0, &err);
        check(d != NULL && !err, "dnssd_init (no network I/O)");
        if (!d) return;
        dnssd_set_airplay_features(d, 0, 0);
        dnssd_set_airplay_features(d, 4, 0);
        dnssd_set_airplay_features(d, 7, 1);
        dnssd_set_airplay_features(d, 9, plan.feature_bit9);
        dnssd_set_airplay_features(d, 27, 1);
        uint64_t f = dnssd_get_airplay_features(d);
        unsigned short port = 0;
        raop_t *r = raop_new(false, &port);
        check(r != NULL, "receiver on a loopback port");
        if (!r) { dnssd_destroy(d); return; }
        raop_set_dnssd(r, d);
        if (!plan.raop_service) check(dnssd_pm_build_raop_txt(d) == 0, "mode 0: TXT built without registering");
        check(!dnssd_pm_raop_registered(d), "_raop._tcp not registered");

        SOCKET c = connect_from("127.0.0.1", port);
        plist_t info = get_info(c, NULL);
        check(info != NULL, "GET /info answered");
        uint64_t got_f = 0;
        plist_t fn = info ? plist_dict_get_item(info, "features") : NULL;
        if (fn) plist_get_uint_val(fn, &got_f);
        printf("    mode %d: advertised 0x%llX, /info features 0x%llX\n", mode, (unsigned long long) f,
               (unsigned long long) got_f);
        check(got_f == f && ((got_f >> 7) & 1) == 1, "/info features = the advertised ones, mirroring bit 7 on");
        check((int) ((got_f >> 9) & 1) == plan.feature_bit9, mode ? "mode 1: bit 9 on" : "mode 0: bit 9 off in /info");
        plist_t disp = info ? plist_dict_get_item(info, "displays") : NULL;
        check(disp && plist_array_get_size(disp) == 1, "/info still lists the display (Screen Mirroring)");
        if (info) plist_free(info);

        if (!plan.raop_service) {
            plist_t t = get_info(c, "txtRAOP");
            char *data = NULL;
            uint64_t dl = 0;
            plist_t tn = t ? plist_dict_get_item(t, "txtRAOP") : NULL;
            if (tn) plist_get_data_val(tn, &data, &dl);
            int has_ft = 0;
            for (uint64_t i = 0; data && i + 3 < dl; i++)
                if (!memcmp(data + i, "ft=", 3)) has_ft = 1;
            check(dl > 20 && has_ft, "GET /info txtRAOP still answered (record built, not advertised)");
            if (data) plist_mem_free(data);
            if (t) plist_free(t);
        }
        closesocket(c);
        raop_stop_httpd(r);
        raop_destroy(r);
        dnssd_destroy(d);
    }
}

int main(void) {
    WSADATA wsa;
    setvbuf(stdout, NULL, _IONBF, 0);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    pm_netutils_loopback_only = 1;
    logger_t *logger = logger_init();
    logger_set_level(logger, LOGGER_ERR);
    logger_set_callback(logger, t_log, NULL);

    InitializeCriticalSection(&g_req_cs);
    ntp_global_init();
    test_httpd(logger);
    test_mirror(logger);
    test_pairing();
    test_request_log();
    test_audio_advert();

    logger_destroy(logger);
    printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
