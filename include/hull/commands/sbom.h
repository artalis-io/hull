/**
 * @file commands/sbom.h
 * @brief `hull sbom`. Print Software Bill of Materials.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_SBOM_H
#define HL_COMMANDS_SBOM_H

#include "hull/commands/dispatch.h"

/** @brief Entry point. Invoked by the command dispatcher. */
int hl_cmd_sbom(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_SBOM_H */
