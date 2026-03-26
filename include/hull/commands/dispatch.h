/*
 * commands/dispatch.h — Table-driven subcommand dispatcher
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_DISPATCH_H
#define HL_COMMANDS_DISPATCH_H

/* Environment passed to all command handlers */
typedef struct {
    const char *hull_exe;      /* path to hull binary (argv[0]) */
    const char *app_dir;       /* --app-dir value, or "." */
    int         verbose;       /* --verbose flag */
    int         json_output;   /* --json flag */
} HlCommandEnv;

/*
 * Command handler signature.
 *   argc/argv — subcommand args (argv[0] is the subcommand name)
 *   env       — parsed global flags and hull binary path
 *
 * Returns process exit code (0 = success).
 */
typedef int (*HlCommandFn)(int argc, char **argv, const HlCommandEnv *env);

typedef struct {
    const char  *name;       /* "build", "test", "keygen", etc. */
    HlCommandFn  handler;    /* C handler function */
} HlCommand;

/*
 * Dispatch a subcommand from the command table.
 *
 *   argc/argv — full program args (argv[1] is the subcommand)
 *
 * Parses global flags (--app-dir, --verbose, --json) before the
 * subcommand name.  Returns the handler's exit code, or -1 if no
 * subcommand matched.
 */
int hl_command_dispatch(int argc, char **argv);

#endif /* HL_COMMANDS_DISPATCH_H */
