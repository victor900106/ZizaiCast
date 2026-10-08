// PM: POSIX odds and ends needed by the UxPlay protocol library under MSVC.
// Force-included (/FI) into every C file of pm_core, see core/CMakeLists.txt.
#pragma once
#ifdef _WIN32
#include <time.h>
#include <basetsd.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

// PM: usleep -> Sleep (rounded up to whole milliseconds).
void pm_usleep(unsigned long long usec);
#ifndef usleep
#define usleep(x) pm_usleep((unsigned long long)(x))
#endif

// PM: clock_gettime(CLOCK_REALTIME/CLOCK_MONOTONIC) via timespec_get. UxPlay uses
// CLOCK_REALTIME as its "local NTP" clock (ns since the Unix epoch) and for
// absolute pthread_cond_timedwait() deadlines (pthreads4w also uses wall time).
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
int pm_clock_gettime(int clk, struct timespec *ts);
#define clock_gettime(c, ts) pm_clock_gettime((c), (ts))

#ifdef __cplusplus
}
#endif
#endif
