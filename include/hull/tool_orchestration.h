/*
 * tool_orchestration.h — orchestration bindings for the `tool` global
 *
 * Split out of src/hull/runtime/lua/mod_tool.c (May 2026 architectural
 * audit, finding A-1). The base `tool.*` surface — spawn, mkdir, copy,
 * read_file, etc. — is a thin Lua binding over `hl_cap_tool_*` and
 * lives at runtime/lua/. The orchestration entries below pull
 * cross-layer dependencies (commands/, agent_lib, dev_state,
 * module_registry, module_resolver, migrate) that don't belong inside
 * runtime/, so they live here under src/hull/ proper and are spliced
 * onto the `tool` global after the base table is installed.
 *
 * Lookup convention: every function in this file takes the `tool`
 * table on the Lua stack (set as the LUA_LOADED_TABLE["tool"] or
 * present as a global), reads the unveil context out of the registry
 * if it needs filesystem access, and adds one entry to `tool`.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_TOOL_ORCHESTRATION_H
#define HL_TOOL_ORCHESTRATION_H

#ifdef HL_ENABLE_LUA

typedef struct lua_State lua_State;

/*
 * Splice orchestration entries onto the existing `tool` global.
 *
 * Must be called after hl_lua_tool_register() has installed the base
 * table. Adds:
 *
 *   tool.modules_resolve(manifest)     → { ok, modules | error }
 *   tool.doctor_json()                 → JSON string
 *   tool.agent_errors(app_dir)         → (json | nil, rc)
 *   tool.agent_context(task, level?)   → (json | nil, rc)
 *   tool.modules_available()           → { count, modules = [...] }
 *   tool.migrate_status(app_dir, db)   → ({entries}|nil, count|error)  [HL_ENABLE_DB only]
 *   tool.dev_status()                  → { pid, alive, reload_count, ... }
 *   tool.dev_drain()                   → int (new lines appended)
 *   tool.dev_recent_lines(n)           → table of strings (newest first)
 *   tool.dev_check_file_change()       → bool
 *   tool.dev_reload()                  → bool
 */
void hl_lua_tool_register_orchestration(lua_State *L);

#endif /* HL_ENABLE_LUA */

#endif /* HL_TOOL_ORCHESTRATION_H */
