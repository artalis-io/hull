/**
 * @file commands/analyze.h
 * @brief `hull analyze` — static source analysis (syntax diagnostics) for Hull apps.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_ANALYZE_H
#define HL_COMMANDS_ANALYZE_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_analyze(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_ANALYZE_H */
