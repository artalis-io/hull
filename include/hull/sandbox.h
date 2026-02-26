/*
 * sandbox.h — Kernel-level sandbox enforcement
 *
 * Applies pledge/unveil (or platform equivalents) based on the
 * application manifest.  After hl_sandbox_apply(), the process
 * can only access the filesystem paths and syscall families that
 * the manifest declares.
 *
 * Platform support:
 *   Cosmopolitan — pledge + unveil (built-in)
 *   Linux 5.13+  — Landlock (unveil), seccomp future
 *   macOS/other  — no-op (C-level cap validation is the defense)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SANDBOX_H
#define HL_SANDBOX_H

#include "hull/manifest.h"

/*
 * Apply kernel sandbox based on manifest capabilities.
 *
 *   manifest  — declared capabilities (may have present==0)
 *   db_path   — SQLite database path (always allowed rw)
 *
 * When manifest.present is false, no sandbox is applied (permissive).
 * Returns 0 on success, -1 on error (logged).
 */
int hl_sandbox_apply(const HlManifest *manifest, const char *db_path);

#endif /* HL_SANDBOX_H */
