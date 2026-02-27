/*
 * runtime.h — Pluggable runtime vtable
 *
 * Defines HlRuntime (shared base) and HlRuntimeVtable so main.c
 * can drive Lua or QuickJS through a single polymorphic interface.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RUNTIME_H
#define HL_RUNTIME_H

#include <stddef.h>

/* Forward declarations */
typedef struct HlAllocator HlAllocator;
typedef struct HlFsConfig HlFsConfig;
typedef struct HlEnvConfig HlEnvConfig;
typedef struct HlHttpConfig HlHttpConfig;
typedef struct HlManifest HlManifest;
typedef struct sqlite3 sqlite3;
typedef struct KlServer KlServer;

typedef struct HlRuntime HlRuntime;

typedef struct HlRuntimeVtable {
    int   (*init)(HlRuntime *rt, const void *config);
    int   (*load_app)(HlRuntime *rt, const char *filename);
    int   (*wire_routes_server)(HlRuntime *rt, KlServer *server,
                                void *(*alloc_fn)(size_t));
    int   (*extract_manifest)(HlRuntime *rt, HlManifest *out);
    void  (*free_manifest_strings)(HlRuntime *rt, HlManifest *m);
    void  (*destroy)(HlRuntime *rt);
    const char *name;
} HlRuntimeVtable;

struct HlRuntime {
    const HlRuntimeVtable *vt;
    sqlite3      *db;
    HlAllocator  *alloc;
    HlFsConfig   *fs_cfg;
    HlEnvConfig  *env_cfg;
    HlHttpConfig *http_cfg;
};

#endif /* HL_RUNTIME_H */
