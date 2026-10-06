/*
 * tool.h - Tool mode: unsandboxed Lua VM for build tools
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
 * What a tool command's argv names, for the tool sandbox's grants. `argv[0]`
 * is the subcommand; `module` the tool module ("hull.build").
 *
 * hl_tool_argv_app_dir: the first positional argument (not an option or an
 * option's value) that names an existing directory and is not itself a
 * symbolic link; NULL when none does. A link named like a subcommand word
 * (`build -> ~/.ssh` in a cloned repo, then `hull compute build`) would
 * otherwise have its target granted.
 *
 * hl_tool_argv_scaffold_target: the directory `hull new <name>` /
 * `hull init [dir]` scaffolds into (the last positional, as new.lua and
 * init.lua take it; init defaults to "."); NULL for other modules, or for
 * `hull new` without a name.
 *
 * hl_tool_argv_read_files: the files the command reads by name - the values
 * of --sign (a key file except for `hull deploy`, where --sign is a switch),
 * --platform-sig, --platform-key, --developer-key, --gethull-key and
 * --binary, and `hull sign-platform <prefix>`'s <prefix>.key / .pub. Up to
 * `max` heap strings into `out` (each freed by the caller); returns the
 * count.
 */
const char *hl_tool_argv_app_dir(const char *module, int argc, char **argv);
const char *hl_tool_argv_scaffold_target(const char *module, int argc, char **argv);
int hl_tool_argv_read_files(const char *module, int argc, char **argv,
                            char **out, int max);

/*
 * Generate an Ed25519 keypair and write to files.
 * Pure C - no Lua VM needed.
 *
 * Returns process exit code (0 = success).
 */
int hull_keygen(int argc, char **argv);

#endif /* HL_TOOL_H */
