/*
 * commands/deploy.h — hull deploy subcommand
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_DEPLOY_H
#define HL_COMMANDS_DEPLOY_H

#include "hull/commands/dispatch.h"

int hl_cmd_deploy(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_DEPLOY_H */
