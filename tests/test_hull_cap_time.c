/*
 * test_hull_cap_time.c — Tests for shared time capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/hull_cap.h"
#include <string.h>
#include <time.h>

UTEST(hull_cap_time, now_returns_reasonable_timestamp)
{
    int64_t t = hull_cap_time_now();
    /* Should be after 2024-01-01 and before 2100-01-01 */
    ASSERT_GT(t, 1704067200LL);  /* 2024-01-01 */
    ASSERT_LT(t, 4102444800LL); /* 2100-01-01 */
}

UTEST(hull_cap_time, now_ms_greater_than_now)
{
    int64_t s = hull_cap_time_now();
    int64_t ms = hull_cap_time_now_ms();
    /* ms should be roughly s * 1000 */
    ASSERT_GT(ms, s * 1000 - 2000);
    ASSERT_LT(ms, s * 1000 + 2000);
}

UTEST(hull_cap_time, clock_monotonic)
{
    int64_t t1 = hull_cap_time_clock();
    /* Spin briefly */
    volatile int x = 0;
    for (int i = 0; i < 100000; i++)
        x += i;
    (void)x;
    int64_t t2 = hull_cap_time_clock();
    ASSERT_GE(t2, t1);
}

UTEST(hull_cap_time, date_format)
{
    char buf[16];
    int rc = hull_cap_time_date(buf, sizeof(buf));
    ASSERT_EQ(rc, 0);

    /* Should be YYYY-MM-DD format */
    ASSERT_EQ(strlen(buf), 10u);
    ASSERT_EQ(buf[4], '-');
    ASSERT_EQ(buf[7], '-');
}

UTEST(hull_cap_time, datetime_format)
{
    char buf[32];
    int rc = hull_cap_time_datetime(buf, sizeof(buf));
    ASSERT_EQ(rc, 0);

    /* Should be YYYY-MM-DDTHH:MM:SSZ format */
    ASSERT_EQ(strlen(buf), 20u);
    ASSERT_EQ(buf[4], '-');
    ASSERT_EQ(buf[7], '-');
    ASSERT_EQ(buf[10], 'T');
    ASSERT_EQ(buf[13], ':');
    ASSERT_EQ(buf[16], ':');
    ASSERT_EQ(buf[19], 'Z');
}

UTEST(hull_cap_time, date_buffer_too_small)
{
    char buf[5];
    int rc = hull_cap_time_date(buf, sizeof(buf));
    ASSERT_EQ(rc, -1);
}

UTEST(hull_cap_time, datetime_buffer_too_small)
{
    char buf[10];
    int rc = hull_cap_time_datetime(buf, sizeof(buf));
    ASSERT_EQ(rc, -1);
}

UTEST(hull_cap_time, null_buffer)
{
    int rc = hull_cap_time_date(NULL, 16);
    ASSERT_EQ(rc, -1);

    rc = hull_cap_time_datetime(NULL, 32);
    ASSERT_EQ(rc, -1);
}

UTEST_MAIN();
