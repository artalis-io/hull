/*
 * commands/platform.h - `hull platform` subcommand.
 *
 * Fetch signed per-flavor platform libraries from the release matching this
 * hull's version into $HOME/.hull/platform/, so `hull build --flavor=<flavor>`
 * can link them without building from source. Same trust chain as
 * `hull tools install` (the shared hl_release_io_fetch_verified_manifest).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_COMMANDS_PLATFORM_H
#define HL_COMMANDS_PLATFORM_H

#include "hull/commands/dispatch.h"

/* `hull platform install <flavor> [--all] [--repo=ORG/NAME]` / `list`. */
int hl_cmd_platform(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_PLATFORM_H */
