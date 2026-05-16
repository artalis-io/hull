/**
 * @file commands/dev.h
 * @brief `hull dev` — hot-reload development server.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_DEV_H
#define HL_COMMANDS_DEV_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_dev(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_DEV_H */
