/*
 * cap/time.h — Time capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_TIME_H
#define HL_CAP_TIME_H

#include <stddef.h>
#include <stdint.h>

int64_t hl_cap_time_now(void);
int64_t hl_cap_time_now_ms(void);
int64_t hl_cap_time_clock(void);
int hl_cap_time_date(char *buf, size_t buf_size);
int hl_cap_time_datetime(char *buf, size_t buf_size);

#endif /* HL_CAP_TIME_H */
