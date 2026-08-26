/**
 * @file commands/compute.h
 * @brief `hull compute` - WASM compute-module developer tooling.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_COMPUTE_H
#define HL_COMMANDS_COMPUTE_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_compute(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_COMPUTE_H */
