/*
 * entry.h — HlEntry type definition for generated registries
 *
 * Canonical struct for all embedded file arrays: app modules (JS, Lua,
 * JSON), templates, static assets, migrations, and stdlib entries.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_ENTRY_H
#define HL_ENTRY_H

typedef struct {
    const char *name;
    const unsigned char *data;
    unsigned int len;
} HlEntry;

#endif /* HL_ENTRY_H */
