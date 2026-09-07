/**
 * @file commands/extract_manifest_js.h
 * @brief `hull __extract-manifest-js` - INTERNAL, not a user-facing verb.
 *
 * The isolated child half of JS manifest extraction (issue #427). Spawned
 * by hl_manifest_extract_js_from_file(); never typed by a user, never listed
 * in `hull help` or the shell completions.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_EXTRACT_MANIFEST_JS_H
#define HL_COMMANDS_EXTRACT_MANIFEST_JS_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_extract_manifest_js(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_EXTRACT_MANIFEST_JS_H */
