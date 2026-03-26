/*
 * commands/dev.h — hull dev subcommand (hot-reload dev server)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_DEV_H
#define HL_COMMANDS_DEV_H

#include "hull/commands/dispatch.h"

int hl_cmd_dev(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_DEV_H */
