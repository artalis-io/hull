/*
 * commands/update.h — hull update subcommand
 *
 * Downloads the latest signed release from GitHub, verifies SHA-256,
 * and atomically replaces the running binary.
 *
 *   hull update                — install latest if newer than current
 *   hull update --check        — print "update available" / "up to date"
 *   hull update --force        — reinstall current version
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CMD_UPDATE_H
#define HL_CMD_UPDATE_H

#include "hull/commands/dispatch.h"

int hl_cmd_update(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_CMD_UPDATE_H */
