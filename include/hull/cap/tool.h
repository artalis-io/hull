/*
 * cap/tool.h — Controlled process/filesystem access for tool scripts
 *
 * Provides the `tool` global table in Lua tool mode.
 * Replaces raw os/io access with an explicit, auditable interface.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_TOOL_H
#define HL_CAP_TOOL_H

/* Forward declaration */
typedef struct lua_State lua_State;

/*
 * Register the `tool` global table in the Lua state.
 *
 * Provides:
 *   tool.exec(cmd)             — execute shell command, return bool
 *   tool.read(cmd)             — execute and capture stdout, return string
 *   tool.tmpdir()              — create temp directory, return path
 *   tool.exit(code)            — exit process
 *   tool.read_file(path)       — read file, return string or nil
 *   tool.write_file(path,data) — write file, return bool
 *   tool.file_exists(path)     — check existence, return bool
 *   tool.stderr(msg)           — write to stderr
 *   tool.loadfile(path)        — load Lua chunk, return function or nil+err
 */
void hl_cap_tool_register(lua_State *L);

#endif /* HL_CAP_TOOL_H */
