/*
 * commands/flavor.h - `hull flavor` subcommand.
 *
 * Fetch signed per-flavor platform libraries from the release matching this
 * hull's version into $HOME/.hull/platform/, so `hull build --flavor=<flavor>`
 * can link them without building from source. Same trust chain as
 * `hull tools install` (the shared hl_release_io_fetch_verified_manifest).
 *
 * Named "flavor", not "platform": in Hull "platform" is the build TARGET
 * (darwin-arm64, linux-x86_64, linux-aarch64, cosmo). This command installs a
 * flavor (a named build config), so the verb matches `hull build --flavor`.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_COMMANDS_FLAVOR_H
#define HL_COMMANDS_FLAVOR_H

#include "hull/commands/dispatch.h"

/* `hull flavor install <flavor> [--all] [--repo=ORG/NAME]` / `list`. */
int hl_cmd_flavor(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_FLAVOR_H */
