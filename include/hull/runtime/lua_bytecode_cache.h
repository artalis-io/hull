/**
 * @file lua_bytecode_cache.h
 * @brief On-disk Lua bytecode cache backed by $HOME/.hull/cache/.
 *
 * Drop-in replacement for luaL_loadbuffer that memoizes the compiled
 * bytecode across process invocations. Speeds up `hull dev` reloads
 * and `hull` cold start on apps with sizable stdlib + app surface
 * (every embedded .lua chunk is parsed once instead of every boot).
 *
 * Cache key = SHA-256(LUA_VERSION || arch || endian || source).
 * The key encodes everything that can change the resulting bytecode,
 * so different Lua versions / hosts cohabit safely.
 *
 * Cache root = $HOME/.hull/cache/lua-bytecode/ (auto-allowed by the
 * sandbox; see docs/blob.md §"Runtime-infrastructure caches"). The
 * cache is shared system-wide across all Hull apps — same source
 * bytes produce the same .luac regardless of which app loaded it
 * first.
 *
 * On ANY error (HOME unset, disk full, dump failure, mmap failure,
 * stale partial write) the call falls through transparently to a
 * vanilla luaL_loadbuffer on the source. The cache is an
 * optimization; correctness never depends on it.
 *
 * Disable via HULL_NO_CACHE=1 or HULL_NO_BYTECODE_CACHE=1.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LUA_BYTECODE_CACHE_H
#define HL_LUA_BYTECODE_CACHE_H

#include <stddef.h>

struct lua_State;

/**
 * Load a Lua chunk like luaL_loadbuffer, transparently caching the
 * compiled bytecode on disk.
 *
 * @param L          Lua state.
 * @param src        Source bytes (Lua chunk text).
 * @param src_len    Length of `src`.
 * @param chunkname  Chunk name for tracebacks (mirrors luaL_loadbuffer).
 * @return LUA_OK on success (compiled function on top of stack);
 *         non-zero on parse failure (error string on top of stack).
 *         Mirrors luaL_loadbuffer's contract exactly.
 */
int hl_lua_load_cached(struct lua_State *L,
                       const char *src, size_t src_len,
                       const char *chunkname);

#endif /* HL_LUA_BYTECODE_CACHE_H */
