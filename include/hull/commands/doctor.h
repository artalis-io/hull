/**
 * @file commands/doctor.h
 * @brief `hull doctor` — environment + distribution readiness check.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_DOCTOR_H
#define HL_COMMANDS_DOCTOR_H

#include "hull/commands/dispatch.h"

#include <stdio.h>

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_doctor(int argc, char **argv, const HlCommandEnv *env);

/**
 * @brief Gather every doctor probe and emit the result as JSON to `f`.
 *
 * Same payload `hull doctor --json` writes to stdout. Used by the
 * `--tui` rendering path (which feeds the JSON into a Lua tool module
 * that builds a TUI from it) and by any future consumer that wants
 * a machine-readable snapshot in-process. Idempotent — calling
 * multiple times produces the same JSON, modulo PATH/FS state.
 */
void hl_doctor_collect_json(FILE *f);

#endif /* HL_COMMANDS_DOCTOR_H */
