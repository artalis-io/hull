/*
 * cap/types.h — Runtime-agnostic value types for capabilities
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_TYPES_H
#define HL_CAP_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

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

/* Key-value pair for deep-copying tables/objects across threads.
 * Keys and string values are heap-owned. */
typedef struct {
    char    *key;       /* heap-owned key string */
    HlValue  value;     /* strings in value are heap-owned */
} HlKV;

/* Free an array of HlKV (keys, string values, and the array itself). */
static inline void hl_kv_free(HlKV *kvs, int count)
{
    if (!kvs) return;
    for (int i = 0; i < count; i++) {
        free(kvs[i].key);
        if ((kvs[i].value.type == HL_TYPE_TEXT ||
             kvs[i].value.type == HL_TYPE_BLOB) && kvs[i].value.s)
            free((void *)(uintptr_t)kvs[i].value.s);  /* value.s is const-typed for the read API but owned here */
    }
    free(kvs);
}

#endif /* HL_CAP_TYPES_H */
