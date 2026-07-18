/*
 * commands/feature.h - `hull feature` subcommand.
 *
 * Fetch signed per-feature libraries (libhull_feature-<name>-<arch>.a) from the
 * release matching this hull's version into $HOME/.hull/feature/, so `hull build`
 * can compose a large optional subsystem (e.g. DuckDB) into an app binary
 * without building it from source. Same Ed25519-signed trust chain as
 * `hull flavor install` / `hull tools install` (the shared hl_release_io_*).
 *
 * A feature is an additive, composable subsystem shipped as its own bolt-on
 * archive -- distinct from a flavor (a preset base build). See
 * docs/features_and_flavors.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_COMMANDS_FEATURE_H
#define HL_COMMANDS_FEATURE_H

#include "hull/commands/dispatch.h"

/* `hull feature install <name> [--repo=ORG/NAME]` / `list` / `uninstall <name>`. */
int hl_cmd_feature(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_FEATURE_H */
