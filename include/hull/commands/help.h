/**
 * @file commands/help.h
 * @brief Top-level usage printer (`hull help` / `hull --help`).
 *
 * Lists every subcommand registered in the dispatch table along with a
 * one-line description. `main.c` also intercepts `--help` / `-h` and
 * routes them here so both styles work.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_HELP_H
#define HL_COMMANDS_HELP_H

#include "hull/commands/dispatch.h"

int hl_cmd_help(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_HELP_H */
