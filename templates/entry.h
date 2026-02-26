/*
 * entry.h — HlStdlibEntry type definition for generated app registries
 *
 * This header is used by hull build when generating app_registry.c.
 * It defines the struct that modules.c references via weak symbol.
 */

#ifndef HL_STDLIB_ENTRY_H
#define HL_STDLIB_ENTRY_H

typedef struct {
    const char *name;
    const unsigned char *data;
    unsigned int len;
} HlStdlibEntry;

#endif /* HL_STDLIB_ENTRY_H */
