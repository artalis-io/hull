/*
 * commands/eject.h — hull eject subcommand
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_EJECT_H
#define HL_COMMANDS_EJECT_H

#include "hull/commands/dispatch.h"

int hl_cmd_eject(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_EJECT_H */
