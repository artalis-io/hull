/*
 * hull_cap_time.c — Shared time capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap.h"
#include <time.h>
#include <stdio.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#else
/* Linux / POSIX */
#endif

int64_t hl_cap_time_now(void)
{
    return (int64_t)time(NULL);
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

    time_t now = time(NULL);
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

    time_t now = time(NULL);
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
