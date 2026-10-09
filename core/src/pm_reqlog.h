/* PM: one INFO line per incoming RTSP/HTTP request (method, URL, User-Agent,
 * client, body size/type; the body itself is never logged), rate-limited so
 * the periodic chatter does not flood the log:
 *
 *  - "routine" requests (POST /feedback every ~2 s, GET_PARAMETER) are logged
 *    at most once per PM_REQLOG_ROUTINE_MS, with the number skipped since;
 *  - any other method+path(+Content-Type, so SET_PARAMETER volume/progress,
 *    artwork and metadata are separate kinds) is logged the first time and then
 *    at most once per PM_REQLOG_REPEAT_MS, again with the skipped count.
 *
 * Pure (no I/O, caller passes the clock) so tests can drive it. Only the httpd
 * thread calls it, so there is no locking. GPL-3.0-or-later. */
#ifndef PM_REQLOG_H
#define PM_REQLOG_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PM_REQLOG_SLOTS 24
#define PM_REQLOG_KEY_LEN 64
#define PM_REQLOG_ROUTINE_MS 60000u
#define PM_REQLOG_REPEAT_MS 10000u

typedef struct {
    char key[PM_REQLOG_KEY_LEN];   /* "METHOD /path" (query string dropped) */
    uint64_t last_ms;              /* when it was last logged */
    unsigned skipped;              /* suppressed since then */
    int used;
} pm_reqlog_slot_t;

typedef struct {
    pm_reqlog_slot_t slot[PM_REQLOG_SLOTS];
} pm_reqlog_t;

static inline int pm_reqlog_is_routine(const char *method, const char *url) {
    if (!method || !url) return 0;
    if (!strcmp(method, "POST") && !strcmp(url, "/feedback")) return 1;
    if (!strcmp(method, "GET_PARAMETER")) return 1;
    return 0;
}

static inline void pm_reqlog_key(const char *method, const char *url, const char *content_type,
                                 char *key, size_t n) {
    size_t plen = 0;
    if (!url) url = "";
    while (url[plen] && url[plen] != '?') plen++;
    if (plen > 36) plen = 36;
    snprintf(key, n, "%s %.*s %.12s", method ? method : "?", (int) plen, url,
             content_type ? content_type : "");
}

/* returns 1 if this request should be logged now; *skipped = number of the
 * same kind suppressed since it was last logged (0 if none). */
static inline int pm_reqlog_should_log(pm_reqlog_t *st, const char *method, const char *url,
                                       const char *content_type, uint64_t now_ms, unsigned *skipped) {
    char key[PM_REQLOG_KEY_LEN];
    uint64_t window = pm_reqlog_is_routine(method, url) ? PM_REQLOG_ROUTINE_MS : PM_REQLOG_REPEAT_MS;
    int free_i = -1, oldest_i = 0, i;
    *skipped = 0;
    pm_reqlog_key(method, url, content_type, key, sizeof(key));
    for (i = 0; i < PM_REQLOG_SLOTS; i++) {
        pm_reqlog_slot_t *s = &st->slot[i];
        if (!s->used) {
            if (free_i < 0) free_i = i;
            continue;
        }
        if (!strcmp(s->key, key)) {
            if (now_ms >= s->last_ms && now_ms - s->last_ms < window) {
                s->skipped++;
                return 0;
            }
            *skipped = s->skipped;
            s->skipped = 0;
            s->last_ms = now_ms;
            return 1;
        }
        if (st->slot[i].last_ms < st->slot[oldest_i].last_ms) oldest_i = i;
    }
    i = (free_i >= 0) ? free_i : oldest_i;   /* table full: recycle the stalest kind */
    memset(&st->slot[i], 0, sizeof(st->slot[i]));
    snprintf(st->slot[i].key, sizeof(st->slot[i].key), "%s", key);
    st->slot[i].last_ms = now_ms;
    st->slot[i].used = 1;
    return 1;
}

/* copy s into out (n bytes) keeping it printable and quote-free, at most max chars */
static inline void pm_reqlog_clean(const char *s, char *out, size_t n, size_t max) {
    size_t o = 0;
    if (!n) return;
    if (s) {
        for (; *s && o + 1 < n && o < max; s++) {
            unsigned char c = (unsigned char) *s;
            out[o++] = (c < 0x20 || c == 0x7f || c == '"') ? '_' : (char) c;
        }
    }
    out[o] = '\0';
}

/* "request: POST /play HTTP/1.1 from 192.168.1.4 UA="AirPlay/1005.12.1" session=yes
 *  body=1234 B application/x-apple-binary-plist -> 200 (+5 similar not logged)" */
static inline void pm_reqlog_format(char *buf, size_t n, const char *method, const char *url,
                                    const char *protocol, const char *client, const char *user_agent,
                                    int has_session_id, int body_len, const char *content_type,
                                    const char *result, unsigned skipped) {
    char m[24], u[160], p[16], ua[96], ct[64];
    char tail[48] = "";
    pm_reqlog_clean(method, m, sizeof(m), sizeof(m) - 1);
    pm_reqlog_clean(url, u, sizeof(u), sizeof(u) - 1);
    pm_reqlog_clean(protocol, p, sizeof(p), sizeof(p) - 1);
    pm_reqlog_clean(user_agent ? user_agent : "-", ua, sizeof(ua), sizeof(ua) - 1);
    pm_reqlog_clean(content_type ? content_type : "", ct, sizeof(ct), sizeof(ct) - 1);
    if (skipped) snprintf(tail, sizeof(tail), " (+%u similar not logged)", skipped);
    snprintf(buf, n, "request: %s %s %s from %s UA=\"%s\"%s body=%d B%s%s -> %s%s",
             m, u, p, client ? client : "?", ua, has_session_id ? " X-Apple-Session-ID" : "",
             body_len, ct[0] ? " " : "", ct, result ? result : "?", tail);
}

#endif
