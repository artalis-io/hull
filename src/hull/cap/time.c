/*
 * hull_cap_time.c - Shared time capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/time.h"
#include <time.h>
#include <stdio.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#else
/* Linux / POSIX */
#endif

/* Whole seconds from CLOCK_REALTIME, the clock now_ms (and SQLite's 'now')
 * read. Not time(): glibc serves it from the coarse clock, a few ms behind,
 * so just after a second boundary time.now() could still name the previous
 * second while a timestamp SQLite or now_ms took a moment earlier named the
 * new one - a jobs reaper test saw a claim stamped "2 s ago" as 1 s old. */
int64_t hl_cap_time_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return (int64_t)time(NULL);
    return (int64_t)ts.tv_sec;
}

int64_t hl_cap_time_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return -1;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int64_t hl_cap_time_clock(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int hl_cap_time_date(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 11)
        return -1;

    time_t now = (time_t)hl_cap_time_now();
    struct tm tm;
    if (gmtime_r(&now, &tm) == NULL)
        return -1;

    int n = snprintf(buf, buf_size, "%04d-%02d-%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    if (n < 0 || (size_t)n >= buf_size)
        return -1;

    return 0;
}

int hl_cap_time_datetime(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 21)
        return -1;

    time_t now = (time_t)hl_cap_time_now();
    struct tm tm;
    if (gmtime_r(&now, &tm) == NULL)
        return -1;

    int n = snprintf(buf, buf_size, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (n < 0 || (size_t)n >= buf_size)
        return -1;

    return 0;
}
