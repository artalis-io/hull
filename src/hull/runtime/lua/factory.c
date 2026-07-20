/*
 * runtime/lua/factory.c — Lua runtime factory
 *
 * Heap-allocates an HlLua, builds HlLuaConfig from HlAppContextOpts,
 * initializes via hl_lua_init, and returns the embedded HlRuntime *.
 * Counterpart in runtime/js/factory.c. Both feed the registry in
 * src/hull/runtime/factory.c.
 *
 * Architectural roadmap item K.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_LUA

#include "hull/runtime/factory.h"
#include "hull/runtime/lua.h"
#include "hull/app_context.h"

#include <stdlib.h>
#include <string.h>

static int lua_create(HlRuntime **out, const HlAppContextOpts *opts,
                      const HlRuntimeBaseConfig *base)
{
    if (!out || !opts) return -1;

    HlLua *lua = (HlLua *)calloc(1, sizeof(HlLua));
    if (!lua) return -1;

    /* Copy base config into rt->base BEFORE init: hl_lua_init reads
     * db_registry (for the db module), alloc (custom Lua allocator),
     * etc. during module registration. */
    if (base) {
        lua->base.db_registry    = base->db_registry;
        lua->base.alloc          = base->alloc;
        lua->base.app_vfs        = base->app_vfs;
        lua->base.platform_vfs   = base->platform_vfs;
        lua->base.thread_pool    = base->thread_pool;
        lua->base.db_path        = base->db_path;
        lua->base.compress       = base->compress;
#ifdef HL_ENABLE_WASM
        lua->base.wasm_cache     = base->wasm_cache;
#endif
        /* Base-resident: propagate the GPU context whether it came from a
         * monolithic HL_ENABLE_GPU build or a composed --with=gpu feature.
         * NULL (no backend) is the inert case. */
        lua->base.gpu_ctx        = base->gpu_ctx;
    }
    /* opts->alloc is honoured separately if base->alloc isn't set. */
    if (!lua->base.alloc && opts->alloc) lua->base.alloc = opts->alloc;

    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.sandbox = opts->sandbox ? opts->sandbox : 1;
    if (opts->heap_limit > 0)
        cfg.max_heap_bytes = (size_t)opts->heap_limit;
    if (opts->instruction_limit > 0)
        cfg.max_instructions = opts->instruction_limit;

    if (hl_lua_init(lua, &cfg) != 0) {
        free(lua);
        return -1;
    }

    /* hl_lua_init doesn't set base.vt — wire it here. */
    lua->base.vt = &hl_lua_vtable;
    *out = &lua->base;
    return 0;
}

static void lua_destroy(HlRuntime *rt)
{
    if (!rt) return;
    HlLua *lua = (HlLua *)rt;   /* base is the first field of HlLua */
    hl_lua_free(lua);
    free(lua);
}

const HlRuntimeFactory hl_lua_factory = {
    .name            = "lua",
    .entry_extension = ".lua",
    .create          = lua_create,
    .destroy         = lua_destroy,
    .vtable          = &hl_lua_vtable,
};

#endif /* HL_ENABLE_LUA */
