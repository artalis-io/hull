/*
 * commands/keygen.c — hull keygen subcommand
 *
 * Pure C implementation — generates Ed25519 keypair.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/keygen.h"
#include "hull/tool.h"

int hl_cmd_keygen(int argc, char **argv, const HlCommandEnv *env)
{
    (void)env;
    return hull_keygen(argc, argv);
}
