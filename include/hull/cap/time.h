/**
 * @file cap/time.h
 * @brief Time capability - wall clock + monotonic + ISO formatting.
 *
 * No allowlist; all callers may read time. All values are UTC.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_TIME_H
#define HL_CAP_TIME_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Current Unix timestamp in seconds (UTC).
 * @return Whole seconds since 1970-01-01T00:00:00Z.
 */
int64_t hl_cap_time_now(void);

/**
 * @brief Current Unix timestamp in milliseconds (UTC).
 * @return Milliseconds since 1970-01-01T00:00:00Z.
 */
int64_t hl_cap_time_now_ms(void);

/**
 * @brief Monotonic clock reading in milliseconds.
 *
 * Suitable for measuring intervals. NOT a wall-clock; the epoch is
 * implementation-defined (typically boot time on Linux, undefined origin
 * on macOS).
 *
 * @return Milliseconds since an implementation-defined epoch.
 */
int64_t hl_cap_time_clock(void);

/**
 * @brief Format the current date as `YYYY-MM-DD`.
 *
 * @param buf       Output buffer.
 * @param buf_size  Capacity. Needs ≥ 11 bytes (10 chars + NUL).
 *
 * @return Number of bytes written (excluding NUL), or `-1` if @p buf_size
 *         is too small.
 */
int hl_cap_time_date(char *buf, size_t buf_size);

/**
 * @brief Format the current datetime as ISO 8601 `YYYY-MM-DDTHH:MM:SSZ`.
 *
 * @param buf       Output buffer.
 * @param buf_size  Capacity. Needs ≥ 21 bytes (20 chars + NUL).
 *
 * @return Number of bytes written (excluding NUL), or `-1` if @p buf_size
 *         is too small.
 */
int hl_cap_time_datetime(char *buf, size_t buf_size);

#endif /* HL_CAP_TIME_H */
