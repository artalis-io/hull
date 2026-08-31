/**
 * @file commands/update.h
 * @brief `hull update` - self-update from the latest signed GitHub release.
 *
 * Downloads the OS/arch-matched asset from `api.github.com`, verifies its
 * SHA-256 against `hull.sha256`, verifies the Ed25519 signature over the
 * manifest against the embedded #HL_RELEASE_PUBKEY_HEX (when configured),
 * and atomically replaces the running binary via `rename(2)`.
 *
 * @par Flags:
 *   - `--check`   - print `update available` / `up to date` and exit.
 *   - `--force`   - reinstall the current version.
 *   - `--channel=stable|beta`
 *   - `--repo=ORG/NAME`
 *
 * No external dependencies - HTTPS via keel's `KlHttpRedirectClient`, SHA-256
 * via mbedTLS, Ed25519 via TweetNaCl, CA trust via the embedded Mozilla
 * bundle.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CMD_UPDATE_H
#define HL_CMD_UPDATE_H

#include "hull/commands/dispatch.h"

/** @brief Entry point - invoked by the command dispatcher. */
int hl_cmd_update(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_CMD_UPDATE_H */
