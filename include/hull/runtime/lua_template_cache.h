/**
 * @file lua_template_cache.h
 * @brief On-disk cache for compiled template render functions.
 *
 * Memoizes the parse+pcall pass that turns generated Lua template
 * code into a render function. Backed by hull/blob_store in keyed
 * mode at $HOME/.hull/blobs/runtime/templates/. Shared
 * infrastructure with the bytecode cache (§1.5.b-X-1) and AOT
 * cache (§1.5.b-X-2).
 *
 * Cache key = SHA-256(LUA_VERSION || arch || endian ||
 *                     generated_code). Because the key hashes the
 * GENERATED code (post-resolve of inheritance + includes), any
 * change to the source template OR to any extends/include target
 * naturally invalidates the entry - no dependency tracking needed.
 *
 * In dev mode, the runtime calls hull.template.clear_cache() on
 * reload to drop the in-process function table; the on-disk store
 * doesn't need to be wiped because content-keyed entries become
 * orphans automatically (cleaned later by `hull cache prune`).
 *
 * Honors HULL_NO_CACHE=1 and HULL_NO_TEMPLATE_CACHE=1. Falls back
 * transparently to a fresh compile on any cache I/O failure.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LUA_TEMPLATE_CACHE_H
#define HL_LUA_TEMPLATE_CACHE_H

#include <stddef.h>

struct lua_State;

/**
 * @brief Compile a template into a render function, with on-disk
 *        memoization.
 *
 * Semantically identical to the previous template `_compile` flow:
 *   luaL_loadbuffer(L, code, len, chunkname);
 *   lua_pcall(L, 0, 1, 0);   // returns inner render function
 *
 * but skips both the parse AND the pcall on a cache hit - the
 * dumped inner function loads back directly.
 *
 * @param L          Lua state.
 * @param code       Generated template Lua source.
 * @param code_len   Length of `code`.
 * @param chunkname  Chunk name for tracebacks (e.g.
 *                   "=template:pages/home.html").
 * @return LUA_OK on success (render function on top of stack);
 *         non-zero on parse/run failure (error string on stack).
 */
int hl_lua_template_compile_cached(struct lua_State *L,
                                   const char *code, size_t code_len,
                                   const char *chunkname);

/**
 * @brief Tear down the process-wide template store handle.
 *
 * Tests that redirect `$HOME` need to drop the cached store. Not
 * for production use.
 */
void hl_lua_template_cache_reset(void);

#endif /* HL_LUA_TEMPLATE_CACHE_H */
