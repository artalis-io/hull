/*
 * entry.h - HlEntry type definition for generated registries
 *
 * Canonical struct for all embedded file arrays: app modules (Lua, JS,
 * JSON), templates, static assets, migrations, and stdlib entries.
 *
 * Included by generated registry .c files and by source files that
 * reference the extern entry arrays.
 */

#ifndef HL_ENTRY_H
#define HL_ENTRY_H

typedef struct {
    const char *name;
    const unsigned char *data;
    unsigned int len;
} HlEntry;

#endif /* HL_ENTRY_H */
