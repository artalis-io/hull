/*
 * tool.h — Tool mode: unsandboxed Lua VM for build tools
 *
 * hull build/verify/inspect/manifest run Lua stdlib scripts with
 * full filesystem access (io/os available). hull keygen is pure C.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_TOOL_H
#define HL_TOOL_H

/*
 * Run a Lua stdlib module as a tool (unsandboxed).
 * `module` is the Lua module name (e.g., "hull.build").
 * `argc`/`argv` are the CLI args (after subcommand stripping).
 * `hull_exe` is the path to the hull binary (argv[0] from main).
 *
 * Returns process exit code (0 = success).
 */
int hull_tool(const char *module, int argc, char **argv, const char *hull_exe);

/*
 * Generate an Ed25519 keypair and write to files.
 * Pure C — no Lua VM needed.
 *
 * Returns process exit code (0 = success).
 */
int hull_keygen(int argc, char **argv);

#endif /* HL_TOOL_H */
