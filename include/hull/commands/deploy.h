/**
 * @file commands/deploy.h
 * @brief `hull deploy` - generate deployment config (Dockerfile, systemd, fly.toml).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_DEPLOY_H
#define HL_COMMANDS_DEPLOY_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_deploy(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_DEPLOY_H */
