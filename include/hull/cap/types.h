/*
 * cap/types.h — Runtime-agnostic value types for capabilities
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_TYPES_H
#define HL_CAP_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations for vendor types */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef struct HlAllocator HlAllocator;

typedef enum {
    HL_TYPE_NIL    = 0,
    HL_TYPE_INT    = 1,
    HL_TYPE_DOUBLE = 2,
    HL_TYPE_TEXT   = 3,
    HL_TYPE_BLOB   = 4,
    HL_TYPE_BOOL   = 5,
} HlValueType;

typedef struct {
    HlValueType type;
    union {
        int64_t     i;
        double      d;
        int         b;     /* bool */
        struct {
            const char *s;
            size_t      len;
        };
    };
} HlValue;

typedef struct {
    const char *name;
    HlValue   value;
} HlColumn;

#endif /* HL_CAP_TYPES_H */
