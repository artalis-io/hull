/*
 * runtime/factory.h — Runtime factory registration
 *
 * Replaces the Lua-vs-JS if-ladders in app_context.c with a table-driven
 * lookup. Each runtime publishes an HlRuntimeFactory; the registry walks
 * a static array and picks the factory whose file extension matches
 * the app's entry point. Adding a third runtime is one new factory file
 * plus a single entry in the registry.
 *
 * Architectural roadmap item K (M3).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RUNTIME_FACTORY_H
#define HL_RUNTIME_FACTORY_H

#include <stddef.h>
#include "hull/runtime.h"

/* Forward declaration — defined in include/hull/app_context.h. */
typedef struct HlAppContextOpts HlAppContextOpts;

/*
 * Base-config bundle passed from app_context to the factory at create-time.
 * The factory copies these into rt->base BEFORE calling the runtime's init
 * (which may read e.g. db_handle during module registration). NULL fields
 * stay zero on the runtime.
 */
typedef struct HlRuntimeBaseConfig {
    struct HlDbRegistry      *db_registry;   /* owns all connections; may be NULL */
    HlAllocator              *alloc;
    const HlVfs              *app_vfs;
    const HlVfs              *platform_vfs;
    HlAsyncBackendPool       *thread_pool;
    const char               *db_path;
    struct KlCompressConfig  *compress;
    HlWasmCache              *wasm_cache;
    HlGpuCtx                 *gpu_ctx;
} HlRuntimeBaseConfig;

/*
 * A runtime factory bundles everything app_context needs to create + own
 * a runtime instance without knowing which runtime it is.
 *
 * `name` is a human-readable identifier (used in logs / doctor output).
 * `entry_extension` is the file extension match (".lua", ".js"); the
 * registry picks the factory whose extension matches the entry point.
 *
 * `create()` allocates HlLua/HlJS storage on the heap, builds the
 * runtime-specific config from `opts` (heap_limit, instruction_limit,
 * etc.), calls hl_X_init(), and returns the embedded HlRuntime *
 * through *out. The caller (app_context.c) then wires shared
 * resources (db_handle, vfs, alloc, ...) on the returned HlRuntime
 * before invoking load_app via rt->vt->load_app().
 *
 * `destroy()` matches `create()`: calls hl_X_free() on the runtime,
 * then frees the underlying HlLua/HlJS storage.
 */
typedef struct HlRuntimeFactory {
    const char *name;
    const char *entry_extension;

    int  (*create)(HlRuntime **out,
                   const HlAppContextOpts *opts,
                   const HlRuntimeBaseConfig *base);
    void (*destroy)(HlRuntime *rt);

    /* The runtime's vtable (also reachable via rt->vt after create). */
    const HlRuntimeVtable *vtable;
} HlRuntimeFactory;

/*
 * Look up a factory by file extension.
 *
 * `ext` may be either ".lua" (with dot) or "lua" (without). NULL or a
 * non-matching extension returns NULL.
 */
const HlRuntimeFactory *hl_runtime_factory_for_extension(const char *ext);

/*
 * Look up a factory by entry-point filename. Convenience wrapper that
 * extracts the extension and calls _for_extension above.
 */
const HlRuntimeFactory *hl_runtime_factory_for_filename(const char *filename);

/*
 * Iterate all registered factories. Returns a pointer to a static array
 * of factory pointers and writes its length into *count. The array is
 * compile-time-stable; the caller does not free it.
 *
 * Used by `hull doctor` and similar introspection.
 */
const HlRuntimeFactory *const *hl_runtime_factories(size_t *count);

#endif /* HL_RUNTIME_FACTORY_H */
