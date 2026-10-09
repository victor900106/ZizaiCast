/* PM: unit checks of the INFO request-log limiter/formatter (pm_reqlog.h).
 * No sockets. exit code 0 = all checks passed. GPL-3.0-or-later. */
#include <stdio.h>
#include <string.h>
#include "pm_reqlog.h"

static int g_fail = 0;
static void check(int ok, const char *what) {
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_fail++;
}

int main(void) {
    static pm_reqlog_t st;
    unsigned sk = 99;
    int i, n;
    char line[512];

    printf("pm_reqlog: routine chatter\n");
    check(pm_reqlog_should_log(&st, "POST", "/feedback", NULL, 1000, &sk) == 1 && sk == 0, "first /feedback is logged");
    n = 0;
    for (i = 1; i <= 29; i++) n += pm_reqlog_should_log(&st, "POST", "/feedback", NULL, 1000 + i * 2000, &sk);
    check(n == 0, "next 29 /feedback within 60 s are not logged");
    check(pm_reqlog_should_log(&st, "POST", "/feedback", NULL, 1000 + 60000, &sk) == 1 && sk == 29,
          "after 60 s one line again, reporting 29 skipped");
    check(pm_reqlog_should_log(&st, "GET_PARAMETER", "rtsp://1.2.3.4/123", NULL, 5000, &sk) == 1, "GET_PARAMETER first logged");
    check(pm_reqlog_should_log(&st, "GET_PARAMETER", "rtsp://1.2.3.4/123", NULL, 6000, &sk) == 0, "GET_PARAMETER repeat not logged");

    printf("pm_reqlog: everything else\n");
    check(pm_reqlog_should_log(&st, "POST", "/play", NULL, 10000, &sk) == 1, "POST /play logged");
    check(pm_reqlog_should_log(&st, "POST", "/reverse", NULL, 10001, &sk) == 1, "POST /reverse logged (different kind)");
    check(pm_reqlog_should_log(&st, "GET", "/server-info", NULL, 10002, &sk) == 1, "GET /server-info logged");
    check(pm_reqlog_should_log(&st, "POST", "/scrub?position=1.0", NULL, 10003, &sk) == 1, "POST /scrub logged");
    check(pm_reqlog_should_log(&st, "POST", "/scrub?position=2.0", NULL, 10500, &sk) == 0, "same path, other query: same kind, suppressed");
    check(pm_reqlog_should_log(&st, "POST", "/scrub?position=3.0", NULL, 20003, &sk) == 1 && sk == 1, "10 s later logged with 1 skipped");
    check(pm_reqlog_should_log(&st, "SET_PARAMETER", "rtsp://x/1", NULL, 30000, &sk) == 1, "SET_PARAMETER logged");
    check(pm_reqlog_should_log(&st, "SET_PARAMETER", "rtsp://x/1", NULL, 30100, &sk) == 0, "SET_PARAMETER burst suppressed");
    check(pm_reqlog_should_log(&st, "SET_PARAMETER", "rtsp://x/1", "text/parameters", 30200, &sk) == 1,
          "SET_PARAMETER with a Content-Type is its own kind");
    check(pm_reqlog_should_log(&st, "SET_PARAMETER", "rtsp://x/1", "image/jpeg", 30300, &sk) == 1,
          "SET_PARAMETER artwork is another kind");
    check(pm_reqlog_should_log(&st, "SET_PARAMETER", "rtsp://x/1", "image/jpeg", 30400, &sk) == 0,
          "artwork repeat suppressed");
    check(pm_reqlog_should_log(&st, "POST", "/play", NULL, 9000, &sk) == 1, "clock going back does not mute a kind forever");

    printf("pm_reqlog: table full\n");
    for (i = 0; i < 100; i++) {
        char url[32];
        snprintf(url, sizeof(url), "/u%d", i);
        n = pm_reqlog_should_log(&st, "GET", url, NULL, 40000 + i, &sk);
        if (!n) break;
    }
    check(i == 100, "100 distinct kinds all logged (stalest slot recycled)");

    printf("pm_reqlog: format\n");
    pm_reqlog_format(line, sizeof(line), "POST", "/play", "HTTP/1.1", "192.168.1.104", "AirPlay/1005.12.1",
                     1, 1234, "application/x-apple-binary-plist", "200 OK", 3);
    printf("    %s\n", line);
    check(!strcmp(line, "request: POST /play HTTP/1.1 from 192.168.1.104 UA=\"AirPlay/1005.12.1\" "
                        "X-Apple-Session-ID body=1234 B application/x-apple-binary-plist -> 200 OK "
                        "(+3 similar not logged)"), "full line");
    pm_reqlog_format(line, sizeof(line), "GET", "/info\r\nX: \"y\"", "RTSP/1.0", "::1", NULL, 0, 0, NULL,
                     "no reply (HLS off)", 0);
    printf("    %s\n", line);
    check(!strchr(line, '\r') && !strchr(line, '\n') && strstr(line, "UA=\"-\"") &&
          strstr(line, "body=0 B -> no reply (HLS off)") && !strstr(line, "\"y\""),
          "control chars and quotes neutralised, missing UA shown as -");
    {
        char big[1000];
        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        pm_reqlog_format(line, sizeof(line), "GET", big, "HTTP/1.1", "1.2.3.4", big, 0, 0, big, "200 OK", 0);
        check(strlen(line) < sizeof(line) && strstr(line, "-> 200 OK"), "overlong fields truncated, status kept");
    }

    printf(g_fail ? "pm_reqlog_test: %d FAILED\n" : "pm_reqlog_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
