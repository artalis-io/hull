/*
 * commands/dispatch.h — Table-driven subcommand dispatcher
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_DISPATCH_H
#define HL_COMMANDS_DISPATCH_H

/*
 * Command handler signature.
 *   argc/argv — subcommand args (argv[0] is the subcommand name)
 *   hull_exe  — path to the hull binary (original argv[0])
 *
 * Returns process exit code (0 = success).
 */
typedef int (*HlCommandFn)(int argc, char **argv, const char *hull_exe);

typedef struct {
    const char  *name;       /* "build", "test", "keygen", etc. */
    HlCommandFn  handler;    /* C handler function */
} HlCommand;

/*
 * Dispatch a subcommand from the command table.
 *
 *   argc/argv — full program args (argv[1] is the subcommand)
 *
 * Returns the handler's exit code, or -1 if no subcommand matched.
 */
int hl_command_dispatch(int argc, char **argv);

#endif /* HL_COMMANDS_DISPATCH_H */
