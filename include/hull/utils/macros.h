/*
 * macros.h - Hull project-wide utility macros.
 *
 * Intentionally minimal: only macros that several TUs need today
 * (or are likely to next month) live here.  Per-module helpers
 * stay local to that module.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_MACROS_H
#define HL_MACROS_H

/**
 * @brief Number of elements in a statically-sized array.
 *
 * Compile-time only - passing a decayed pointer silently produces
 * the wrong answer.  Prefer this over open-coded
 * `sizeof(x)/sizeof(x[0])` so a future refactor that turns the
 * array into a pointer fails to compile rather than silently
 * dividing pointer-size / element-size.
 */
#define HL_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#endif /* HL_MACROS_H */
