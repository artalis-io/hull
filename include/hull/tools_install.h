/**
 * @file tools_install.h
 * @brief Side-loaded Hull tool registry + on-disk install helpers.
 *
 * Hull ships first-party tools (e.g. `wamrc`) as separately-downloaded
 * artifacts kept under `$HOME/.hull/tools/`. The trust chain reuses
 * the same Ed25519-signed `hull.sha256` manifest that protects the
 * `hull` binary itself; see docs/tools_install.md.
 *
 * This header exposes:
 *   - The static registry of known tools (one entry per published tool).
 *   - The path-resolution helpers that `commands/tools.c`,
 *     `commands/doctor.c`, and `cap/wasm.c` use to locate installed
 *     binaries.
 *
 * Path construction is hardened against traversal: every tool name
 * passing through these helpers is validated against
 * `[A-Za-z0-9_-]+`. Callers should still resolve via
 * `hl_tools_find()` first to confirm the name is registered before
 * doing any I/O.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_TOOLS_INSTALL_H
#define HL_TOOLS_INSTALL_H

#include <stddef.h>

/* Maximum tool-name length. Tool names are short identifiers and never
 * paths, so a generous cap is fine. Anything longer is rejected by
 * `hl_tools_name_valid()`. */
#define HL_TOOL_NAME_MAX 32

/**
 * @brief One row in the tool registry.
 *
 * Per-platform availability is a fixed set of bool flags rather than
 * a list so the table stays compile-time-constant and there's no
 * allocation. To add a new platform, extend both this struct and
 * `hl_tools_published_for()`.
 */
typedef struct {
    const char *name;               /**< e.g. "wamrc" */
    const char *description;        /**< Human-readable one-liner */
    int         has_linux_x86_64;
    int         has_linux_aarch64;
    int         has_darwin_arm64;
    int         has_cosmo;
} HlToolSpec;

/**
 * @brief Return a NULL-terminated static array of all registered tools.
 *
 * Pointer is stable for the lifetime of the process. Iterate until
 * `name == NULL`.
 */
const HlToolSpec *hl_tools_registry(void);

/**
 * @brief Look up a tool by name. NULL if not registered.
 *
 * Callers should run @p name through this BEFORE constructing any
 * filesystem paths from it — the registry is the trust boundary.
 */
const HlToolSpec *hl_tools_find(const char *name);

/**
 * @brief Validate a tool-name string.
 *
 * Accepts non-empty `[A-Za-z0-9_-]+` of length ≤ HL_TOOL_NAME_MAX-1.
 * Rejects empty, dots, slashes, null bytes, control characters,
 * Unicode. Used as an extra defense-in-depth layer when building paths.
 *
 * @returns 1 if valid, 0 otherwise.
 */
int hl_tools_name_valid(const char *name);

/**
 * @brief Whether @p spec publishes a binary for @p platform.
 *
 * @p platform values come from `hl_release_io_platform()`:
 * `"linux-x86_64"`, `"linux-aarch64"`, `"darwin-arm64"`, `"cosmo"`.
 */
int hl_tools_published_for(const HlToolSpec *spec, const char *platform);

/**
 * @brief Build the release asset name (`hull-<tool>-<platform>`).
 *
 * @returns 0 on success, -1 if @p platform is unknown or buffer too small.
 */
int hl_tools_asset_name(const HlToolSpec *spec, const char *platform,
                        char *out, size_t out_sz);

/**
 * @brief Resolve `$HOME/.hull/tools/` and ensure it exists (mode 0755).
 *
 * Creates `~/.hull` and `~/.hull/tools` if missing. The returned path
 * always ends with a `/` so callers can concatenate file names directly.
 *
 * @returns 0 on success, -1 if HOME is unset or mkdir failed
 *          (with errno set).
 */
int hl_tools_dir(char *out, size_t out_sz);

/**
 * @brief Build the canonical install path for @p name:
 *        `$HOME/.hull/tools/<name>`.
 *
 * Does NOT create the directory or check whether the file exists.
 * Validates @p name against `hl_tools_name_valid()` before
 * concatenation — never construct this path from user input directly.
 *
 * @returns 0 on success, -1 on invalid name / unset HOME / overflow.
 */
int hl_tools_install_path(const char *name, char *out, size_t out_sz);

/**
 * @brief Locate an executable tool by name. Read-only.
 *
 * Search order (first hit wins):
 *   1. `$HOME/.hull/tools/<name>` (canonical install location)
 *   2. `dirname(hull_exe)/<name>` — for ejected/portable installs
 *      (skipped if @p hull_exe is NULL or has no directory component)
 *   3. `<name>` resolved via the system $PATH
 *
 * Each candidate is checked with `access(X_OK)`. The first one that's
 * executable is copied to @p out. No file is opened or executed.
 *
 * @param hull_exe Path to the running hull binary (env->hull_exe), or NULL.
 * @returns 0 on success, -1 if no executable was found anywhere.
 */
int hl_tools_lookup_path(const char *name, const char *hull_exe,
                         char *out, size_t out_sz);

#endif /* HL_TOOLS_INSTALL_H */
