/**
 * @file commands/cache.h
 * @brief `hull cache list|prune|clear` - manage runtime cache pool.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_CACHE_H
#define HL_COMMANDS_CACHE_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_cache(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_CACHE_H */
