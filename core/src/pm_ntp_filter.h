/* PM: the NTP offset filter of raop_ntp.c as a pure function (unit-tested in
 * tests/pm_clock_test.cpp).
 *
 * Upstream smooths the best (lowest-delay) sample with an EWMA of alpha 0.05
 * at one sample per 3 s. When the client's clock offset really jumps (seen
 * after an iPhone pauses and resumes the mirror stream: the raw offset moved
 * by about 2.7 s), that EWMA needs minutes to follow: 5% of the remaining
 * error per 3 s, which the log shows as "NTP offset moved by +94.8, +90.0,
 * +85.5 ms ..." every 3 s for over a minute.
 *
 * Here a best sample that differs from the smoothed offset by more than
 * PM_NTP_STEP_S is not blended in: it is held back, and if
 * PM_NTP_STEP_COUNT samples in a row agree on the far value the filter steps
 * to it at once (like ntpd's step threshold). A single outlier is ignored.
 * GPL-3.0-or-later. */
#ifndef PM_NTP_FILTER_H
#define PM_NTP_FILTER_H

#include <math.h>
#include <stdbool.h>

#define PM_NTP_STEP_S     0.050  /* far from the smoothed offset: step instead of blending */
#define PM_NTP_STEP_COUNT 2      /* far samples in a row needed for a step */

typedef struct pm_ntp_filter_s {
    bool have;        /* smoothed holds a value */
    double smoothed;  /* seconds */
    int far_count;    /* consecutive far samples */
    double far_last;  /* the last far sample (the next one must agree with it) */
} pm_ntp_filter_t;

/* Feeds one best sample (seconds). Returns 1 if the filter stepped (a jump
 * was accepted), else 0. */
static inline int pm_ntp_filter_update(pm_ntp_filter_t *f, double best, double alpha) {
    if (!f->have) {
        f->have = true;
        f->smoothed = best;
        f->far_count = 0;
        return 0;
    }
    if (fabs(best - f->smoothed) > PM_NTP_STEP_S) {
        if (f->far_count > 0 && fabs(best - f->far_last) > PM_NTP_STEP_S) f->far_count = 0;  /* disagrees */
        f->far_last = best;
        if (++f->far_count >= PM_NTP_STEP_COUNT) {
            f->smoothed = best;
            f->far_count = 0;
            return 1;
        }
        return 0;  /* hold the smoothed value: maybe an outlier */
    }
    f->far_count = 0;
    f->smoothed = (1.0 - alpha) * f->smoothed + alpha * best;
    return 0;
}

#endif
