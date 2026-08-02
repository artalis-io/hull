/*
 * runtime/tool.h — Lua bindings for the `tool` global
 *
 * Source lives in src/hull/runtime/lua/mod_tool.c (moved out of the cap
 * layer as part of architectural roadmap item F). The pure-C tool
 * capability (unveil + allowlisted spawn + filesystem helpers) is
 * declared in hull/cap/tool.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_RUNTIME_TOOL_H
#define HL_RUNTIME_TOOL_H

#ifdef HL_ENABLE_LUA

/* Forward declarations */
typedef struct lua_State        lua_State;
typedef struct HlToolUnveilCtx  HlToolUnveilCtx;
typedef struct HlCompiler       HlCompiler;
typedef struct HlLinker         HlLinker;

/*
 * Register the `tool` global table in the Lua state.
 *
 * Provides:
 *   tool.spawn(argv_table)        — fork/execvp with allowlist, return (bool, int)
 *   tool.spawn_read(argv_table)   — spawn and capture stdout, return string|nil
 *   tool.find_files(dir, pattern) — recursive file search, return table
 *   tool.copy(src, dst)           — copy file, return bool
 *   tool.mkdir(path)              — recursive mkdir, return bool
 *   tool.rmdir(path)              — recursive remove, return bool
 *   tool.tmpdir()                 — create temp directory, return path
 *   tool.exit(code)               — exit process
 *   tool.read_file(path)          — read file, return string or nil
 *   tool.write_file(path, data)   — write file, return bool
 *   tool.file_exists(path)        — check existence, return bool
 *   tool.stderr(msg)              — write to stderr
 *   tool.loadfile(path)           — load Lua chunk, return function or nil+err
 *   tool.extract_platform(dir)    — extract embedded platform .a, return bool
 *   tool.extract_platform_cosmo(dir) — extract multi-arch + .aarch64/ layout
 *   tool.platform_archs()         — return table of embedded arch names or nil
 *   tool.cc                       — configured compiler (string field)
 *
 * The unveil context pointer is stored in the Lua registry for
 * path validation by filesystem functions.
 */
void hl_lua_tool_register(lua_State *L, HlToolUnveilCtx *ctx);

/*
 * Expose the compiler vtable as tool.compiler in the Lua tool global.
 * Must be called after hl_lua_tool_register(). compiler must not be NULL.
 */
void hl_lua_tool_expose_compiler(lua_State *L, HlCompiler *compiler);

/*
 * Expose the linker vtable as tool.linker in the Lua tool global (the
 * compiler-free build's link step). Must be called after
 * hl_lua_tool_register(). linker must not be NULL.
 */
void hl_lua_tool_expose_linker(lua_State *L, HlLinker *linker);

#endif /* HL_ENABLE_LUA */

#endif /* HL_RUNTIME_TOOL_H */
