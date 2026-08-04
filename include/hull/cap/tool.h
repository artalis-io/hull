/*
 * cap/tool.h — Pure-C tool capability surface
 *
 * Declares the controlled-process and unveil/filesystem helpers used by
 * Hull tooling. All process execution goes through an allowlisted
 * fork/execvp path. All filesystem operations validate paths against
 * unveiled directories.
 *
 * The Lua `tool` global is declared separately in hull/runtime/tool.h
 * (item F — cap layer is now runtime-free).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_TOOL_H
#define HL_CAP_TOOL_H

#include <stddef.h>

/* ── Unveil path table ─────────────────────────────────────────────── */

#define HL_TOOL_MAX_UNVEILED 16

typedef struct {
    const char *path;
    const char *perms;   /* "r", "rw", "rwc", "rx" etc. */
} HlToolUnveilEntry;

typedef struct HlToolUnveilCtx {
    HlToolUnveilEntry entries[HL_TOOL_MAX_UNVEILED];
    int count;
    int sealed;
} HlToolUnveilCtx;

/*
 * Initialize the tool unveil context (empty, unsealed).
 */
void hl_tool_unveil_init(HlToolUnveilCtx *ctx);

/*
 * Add a path to the unveil table. The path is resolved via realpath()
 * (or used as-is if it doesn't exist yet). Returns 0 on success, -1 if full.
 */
int hl_tool_unveil_add(HlToolUnveilCtx *ctx, const char *path, const char *perms);

/*
 * Free all strdup'd paths in the unveil context and reset to empty.
 */
void hl_tool_unveil_free(HlToolUnveilCtx *ctx);

/*
 * Seal the unveil table — no more entries can be added.
 */
void hl_tool_unveil_seal(HlToolUnveilCtx *ctx);

/*
 * Check if a path is allowed under the unveil table.
 * `needed` is a single char: 'r' for read, 'w' for write, 'x' for execute.
 * Returns 0 if allowed, -1 if denied.
 */
int hl_tool_unveil_check(const HlToolUnveilCtx *ctx, const char *path, char needed);

/* ── C-level tool functions (callable from test runner etc.) ──────── */

/*
 * Spawn a process from an argv array (no shell).
 * Validates argv[0] against the compiler allowlist.
 * Returns the child exit code, or -1 on fork/exec error.
 */
int hl_tool_spawn(const char *const argv[]);

/*
 * Like hl_tool_spawn, but apply extra environment entries (an array of
 * "KEY=VALUE" strings, NULL-terminated; NULL = none) in the child before exec.
 * Used to redirect a spawned toolchain's cache into a sandbox-writable dir
 * (e.g. ZIG_GLOBAL_CACHE_DIR under the build tmpdir for zig cross-compiles).
 */
int hl_tool_spawn_env(const char *const argv[], const char *const envadd[]);

/*
 * Drive an allowlisted compiler @p driver THROUGH a POSIX shell @p shell,
 * running `<shell> [sh] -c 'exec "$0" "$@"' <driver> <args...>` (the "sh"
 * applet selector is inserted only when @p shell is busybox). Used only on the
 * cosmo/Windows build path, where cosmocc's driver is a `#!/bin/sh` script that
 * Windows cannot execvp directly: a bundled busybox-w64 supplies the shell.
 *
 * This does NOT widen the "no arbitrary shell" invariant: @p shell's basename
 * must be sh / busybox, @p driver must pass hl_tool_check_allowlist, @p args are
 * dangerous-flag-validated, and the `-c` program is a COMPILE-TIME LITERAL - so
 * no app-derived byte is ever parsed as shell code (driver is $0, args are $@,
 * all positional). @p args / @p envadd are NULL-terminated arrays (NULL = none).
 * Returns the driver's exit code, or -1 on a validation / spawn failure.
 */
int hl_tool_spawn_driver_shell(const char *shell, const char *driver,
                               const char *const args[],
                               const char *const envadd[]);

/*
 * Spawn a process and capture its stdout.
 * Returns a malloc'd string (caller frees), or NULL on error.
 * If out_len is non-NULL, the output length is stored there.
 */
char *hl_tool_spawn_read(const char *const argv[], size_t *out_len);

/*
 * Check if a binary name is in the compiler allowlist.
 * Accepts: cc, gcc, clang, cosmocc, cosmoar, ar (and versioned variants).
 * Returns 0 if allowed, -1 if rejected.
 */
int hl_tool_check_allowlist(const char *binary);

/*
 * Validate argv for dangerous compiler flags that can execute arbitrary code.
 * Rejects: -load, -fplugin, -fplugin=, -Xlinker, -Wl,, @response_file.
 * Returns 0 if clean, -1 if a dangerous flag is found.
 */
int hl_tool_validate_args(const char *const argv[]);

/*
 * Recursively find files matching a glob pattern.
 * Skips dotfiles/dirs, node_modules, vendor.
 * Returns a NULL-terminated array of strdup'd paths (caller frees each + array).
 * Validates dir against unveil context if ctx is non-NULL.
 */
char **hl_tool_find_files(const char *dir, const char *pattern,
                          const HlToolUnveilCtx *ctx);

/*
 * Same as hl_tool_find_files, but lets the caller opt OUT of the
 * default "vendor" skip. Use for walking `static/` where
 * `static/vendor/htmx.min.js` is a legitimate vendored asset that
 * MUST be embedded. node_modules / dotfiles are still skipped
 * unconditionally (no legitimate reason to recurse into them).
 */
char **hl_tool_find_files_ex(const char *dir, const char *pattern,
                             const HlToolUnveilCtx *ctx,
                             int include_vendor);

/*
 * Copy a file (binary-safe).
 * Returns 0 on success, -1 on error.
 */
int hl_tool_copy(const char *src, const char *dst,
                 const HlToolUnveilCtx *ctx);

/*
 * Create directories recursively (like mkdir -p).
 * Validates path against unveil context if ctx is non-NULL.
 * Returns 0 on success, -1 on error.
 */
int hl_tool_mkdir(const char *path, const HlToolUnveilCtx *ctx);

/*
 * Recursively remove a directory tree.
 * Uses lstat (no symlink following). Validates path against unveil context.
 * Returns 0 on success, -1 on error.
 */
int hl_tool_rmdir(const char *path, const HlToolUnveilCtx *ctx);

#endif /* HL_CAP_TOOL_H */
