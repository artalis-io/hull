/**
 * @file commands/test.h
 * @brief `hull test` — run an app's unit tests in-process.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_TEST_H
#define HL_COMMANDS_TEST_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_test(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_TEST_H */
