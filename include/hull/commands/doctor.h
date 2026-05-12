/*
 * commands/doctor.h — hull doctor subcommand
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_DOCTOR_H
#define HL_COMMANDS_DOCTOR_H

#include "hull/commands/dispatch.h"

int hl_cmd_doctor(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_DOCTOR_H */
