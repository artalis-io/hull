/**
 * @file commands/dispatch.h
 * @brief Table-driven subcommand dispatcher (`hull <verb> ...`).
 *
 * Hull's `main()` parses global flags into an #HlCommandEnv, then looks
 * up `argv[1]` in a static command table and calls the matching
 * #HlCommandFn. Adding a new command:
 *   1. Create `src/hull/commands/<name>.c` exporting `hl_cmd_<name>`.
 *   2. Add one row to the table in `src/hull/commands/dispatch.c`.
 *   3. Add `commands/<name>.h` with the prototype.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_DISPATCH_H
#define HL_COMMANDS_DISPATCH_H

/**
 * @brief Parsed global flags + binary path passed to every command.
 */
typedef struct {
    const char *hull_exe;      /**< Path to the hull binary (argv[0]). */
    const char *app_dir;       /**< `--app-dir` value, or `"."`. */
    int         verbose;       /**< `--verbose` flag. */
    int         json_output;   /**< `--json` flag. */
} HlCommandEnv;

/**
 * @brief Command handler signature.
 *
 * @param argc  Subcommand argc.
 * @param argv  Subcommand argv (argv[0] is the subcommand name).
 * @param env   Parsed global flags + hull binary path.
 * @return Process exit code (0 = success).
 */
typedef int (*HlCommandFn)(int argc, char **argv, const HlCommandEnv *env);

/** @brief One row of the command table. */
typedef struct {
    const char  *name;       /**< Verb name (e.g. `"build"`, `"test"`). */
    HlCommandFn  handler;    /**< C handler function. */
} HlCommand;

/**
 * @brief Dispatch `hull <verb>` from the command table.
 *
 * Parses global flags (`--app-dir`, `--verbose`, `--json`) before the
 * subcommand name. Looks up `argv[1]` in the static command table and
 * invokes the matching handler.
 *
 * @param argc Full program argc.
 * @param argv Full program argv.
 * @return The handler's exit code, or -1 if no subcommand matched.
 */
int hl_command_dispatch(int argc, char **argv);

#endif /* HL_COMMANDS_DISPATCH_H */
