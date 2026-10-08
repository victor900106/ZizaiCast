// PM: implementation of the tiny POSIX shim used by the MSVC port of UxPlay lib.
// Copyright (C) 2026 PhoneMirror contributors. GPL-3.0-or-later.
#include <windows.h>
#include "pm_posix.h"

void pm_usleep(unsigned long long usec) {
    Sleep((DWORD) ((usec + 999) / 1000));
}

int pm_clock_gettime(int clk, struct timespec *ts) {
    if (!ts) return -1;
    if (clk == CLOCK_MONOTONIC) {
        static LARGE_INTEGER freq;
        LARGE_INTEGER now;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        ts->tv_sec = (time_t) (now.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long) (((now.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart);
        return 0;
    }
    return timespec_get(ts, TIME_UTC) == TIME_UTC ? 0 : -1;
}
