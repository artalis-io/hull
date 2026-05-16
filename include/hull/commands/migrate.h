/**
 * @file commands/migrate.h
 * @brief `hull migrate` — apply SQL migrations to the app database.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_MIGRATE_H
#define HL_COMMANDS_MIGRATE_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_migrate(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_MIGRATE_H */
