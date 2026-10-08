/* PM: offline simulation of the audio jitter buffer (raop_buffer.c) around a
 * pause -> play during mirroring. Packets are fed in real time (one every
 * ~10.9 ms like AAC-ELD 480 spf), dequeued exactly like raop_rtp.c does
 * (resend mode, i.e. the phone sent a controlPort), and the time from the
 * arrival of the first audio packet after the gap until it leaves the buffer
 * is printed.
 *
 *   pm_buffer_test            all scenarios; exit code 1 if a fixed-path
 *                             scenario takes >= 50 ms
 * GPL-3.0-or-later. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#include "raop_buffer.h"
#include "logger.h"

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
}

static void make_packet(unsigned char *p, int *len, unsigned short seq, unsigned int ts, int nodata) {
    memset(p, 0, 44);
    p[0] = 0x80;
    p[1] = 0x60;
    p[2] = (unsigned char) (seq >> 8);
    p[3] = (unsigned char) seq;
    p[4] = (unsigned char) (ts >> 24);
    p[5] = (unsigned char) (ts >> 16);
    p[6] = (unsigned char) (ts >> 8);
    p[7] = (unsigned char) ts;
    if (nodata) {
        p[12] = 0x00; p[13] = 0x68; p[14] = 0x34; p[15] = 0x00;
        *len = 16;
    } else {
        for (int i = 12; i < 44; i++) p[i] = (unsigned char) (i * 7 + seq);
        *len = 44;
    }
}

typedef struct {
    const char *name;
    int before;       /* audio packets before the pause */
    int nodata;       /* seqnums used by "no data" packets during the pause */
    int silent_seqs;  /* seqnums skipped without any packet (sender sent nothing) */
    int after;        /* audio packets after the pause */
    int mark_nodata;  /* 1 = fixed raop_rtp (reports no-data seqnums to the buffer) */
    unsigned max_wait;/* raop_buffer hole timeout; 1000000 = upstream (wait for a full buffer) */
    int lose_first;   /* first audio packet after the pause is lost (never resent) */
} scenario_t;

static double run(const scenario_t *sc, int *ok_count) {
    unsigned char key[16] = {0}, iv[16] = {0};
    logger_t *logger = logger_init();
    raop_buffer_t *b = raop_buffer_init(logger, key, iv);
    raop_buffer_set_max_wait(b, sc->max_wait);
    unsigned short seq = 40000;
    unsigned int ts = 123456;
    unsigned char pkt[64];
    int len;
    double t_first_after = -1, t_out = -1;
    unsigned short first_after_seq = 0;
    int total = sc->before + sc->nodata + sc->silent_seqs + sc->after;
    int delivered = 0;
    double t0 = now_ms();
    for (int i = 0; i < total; i++) {
        /* real-time pacing: 480 samples @ 44.1 kHz */
        double due = t0 + i * 480.0 * 1000.0 / 44100.0;
        while (now_ms() < due) Sleep(0);
        int is_nodata = i >= sc->before && i < sc->before + sc->nodata;
        int is_silent = i >= sc->before + sc->nodata && i < sc->before + sc->nodata + sc->silent_seqs;
        int is_after = i >= sc->before + sc->nodata + sc->silent_seqs;
        int first_after = is_after && i == sc->before + sc->nodata + sc->silent_seqs;
        if (is_silent || (first_after && sc->lose_first)) {
            /* nothing arrives for this seqnum */
        } else if (is_nodata) {
            if (sc->mark_nodata) raop_buffer_mark_nodata(b, seq);
        } else {
            make_packet(pkt, &len, seq, ts, 0);
            raop_buffer_enqueue(b, pkt, (unsigned short) len, 1);
            if (is_after && t_first_after < 0) {
                t_first_after = now_ms();
                first_after_seq = seq;
            }
            void *payload;
            unsigned int size;
            uint32_t rts;
            unsigned short s;
            while ((payload = raop_buffer_dequeue(b, &size, &rts, &s, 0))) {
                delivered++;
                if (t_out < 0 && t_first_after >= 0 && (unsigned short) (s - first_after_seq) < 0x8000) t_out = now_ms();
                free(payload);
            }
        }
        seq++;
        ts += 480;
    }
    raop_buffer_stats_t st;
    raop_buffer_get_stats(b, &st);
    double lat = t_out >= 0 ? t_out - t_first_after : -1;
    printf("%-58s first post-gap packet out after %8.1f ms%s | delivered %d, holds %u, nodata skipped %u, hole "
           "timeouts %u, overrun skips %u, overflow flushes %u\n",
           sc->name, lat, lat < 0 ? " (NEVER)" : "", delivered, st.holds, st.nodata_skipped, st.timeout_skips,
           st.overrun_skipped, st.overflow_flushes);
    if (sc->mark_nodata && lat >= 0 && lat < 50) (*ok_count)++;
    raop_buffer_destroy(b);
    logger_destroy(logger);
    return lat;
}

int main(void) {
    timeBeginPeriod(1);
    const unsigned UP = 1000000;
    scenario_t sc[] = {
        /* name                                                     before nodata silent after mark wait lose */
        {"upstream: 1 s pause sent as 92 no-data packets", 60, 92, 0, 300, 0, UP, 0},
        {"upstream: 0.4 s pause (37 no-data packets)", 60, 37, 0, 300, 0, UP, 0},
        {"upstream: 3 s pause (276 no-data, > buffer)", 60, 276, 0, 60, 0, UP, 0},
        {"fixed: 1 s pause sent as 92 no-data packets", 60, 92, 0, 60, 1, 40, 0},
        {"fixed: 0.4 s pause (37 no-data packets)", 60, 37, 0, 60, 1, 40, 0},
        {"fixed: 3 s pause (276 no-data, > buffer)", 60, 276, 0, 60, 1, 40, 0},
        {"fixed: seq jump of 92 with no packets at all", 60, 0, 92, 60, 1, 40, 0},
        {"fixed: 1 s pause + first audio packet lost", 60, 92, 0, 60, 1, 40, 1},
    };
    int n = (int) (sizeof(sc) / sizeof(sc[0])), ok = 0, fixed = 0;
    for (int i = 0; i < n; i++) {
        run(&sc[i], &ok);
        if (sc[i].mark_nodata) fixed++;
    }
    timeEndPeriod(1);
    /* every "fixed" scenario must be < 50 ms (the lost-packet one is bounded by
     * the 40 ms hole timeout measured from the next packet) */
    printf("fixed scenarios with the first post-gap packet out in < 50 ms: %d of %d\n", ok, fixed);
    return ok == fixed ? 0 : 1;
}
