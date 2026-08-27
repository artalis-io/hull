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
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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
    /* A "bundle" tool ships as a `hull-<name>.tar` archive of several files
     * (not a single executable) and installs by EXTRACTING to a DIRECTORY
     * $HOME/.hull/tools/<name>/ rather than writing one blob + a symlink. The
     * archive may be nested (subdirs), e.g. zig's lib/ tree. Used by the
     * libc-musl-<arch> static-link floor and the lld/zig toolchains.
     * 0 = a normal single-binary tool. */
    int         is_bundle;
    /* For a bundle: the primary file inside the extracted dir. It proves the
     * bundle is installed (tool_installed checks $dir/<bundle_entry>) and, when
     * it is the toolchain executable (zig, lld), is what hl_tools_lookup_path
     * resolves to ($HOME/.hull/tools/<name>/<bundle_entry>). Data-only bundles
     * (the floor) point it at a sentinel file (crt1.o) that is never exec-looked
     * up. NULL for a single-binary tool. */
    const char *bundle_entry;
    /* Asset-name shape for a bundle. 0 (default): the ARCH is baked into the
     * name and the bundle is published for exactly one platform, so the asset is
     * `hull-<name>.tar` (the libc-musl-<arch> floor). 1: the name is arch-free
     * and the bundle is published per-platform, so the asset carries a platform
     * suffix `hull-<name>-<platform>.tar` (the lld / zig toolchains, one artifact
     * per native platform). Ignored for a non-bundle tool. */
    int         bundle_per_platform;
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
 * filesystem paths from it - the registry is the trust boundary.
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
 * concatenation - never construct this path from user input directly.
 *
 * @returns 0 on success, -1 on invalid name / unset HOME / overflow.
 */
int hl_tools_install_path(const char *name, char *out, size_t out_sz);

/**
 * @brief Locate an executable tool by name. Read-only.
 *
 * Search order (first hit wins):
 *   1. `$HOME/.hull/tools/<name>` (canonical install location)
 *   2. `dirname(hull_exe)/<name>` - for ejected/portable installs
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

/* Bundle extraction now lives in the shared tar core: include hull/cap/tar.h
 * and call hl_tar_extract(). */

/**
 * @brief Where a resolved tool was found.
 */
typedef enum {
    HL_TOOL_SRC_NONE = 0,   /**< not resolvable as an executable anywhere */
    HL_TOOL_SRC_MANAGED,    /**< under $HOME/.hull/tools (via `hull tools install`) */
    HL_TOOL_SRC_SIBLING,    /**< next to the running hull binary (ejected/portable) */
    HL_TOOL_SRC_PATH        /**< on the system $PATH */
} HlToolSource;

/**
 * @brief Unified install + resolution status for one registered tool.
 *
 * The SINGLE source of truth shared by `hull tools list`, `hull agent
 * tools`, and `hull doctor` (and consistent with `hull build`, which
 * resolves via `hl_tools_lookup_path` directly), so all surfaces agree on
 * whether a tool is a hull-managed install, whether/where it resolves as an
 * executable, and how large a managed install is.
 *
 * `managed` and `resolved` are RELATED but DISTINCT: a data-only bundle (the
 * libc-musl floor) can be `managed` (installed under ~/.hull/tools with its
 * sentinel present) yet not `resolved` (its sentinel isn't an executable). A
 * system tool on $PATH is `resolved` (source PATH) but not `managed`.
 */
typedef struct {
    int                managed;       /**< a valid install exists under ~/.hull/tools */
    int                resolved;      /**< hl_tools_lookup_path found an executable */
    HlToolSource       source;        /**< where the executable resolved from */
    char               path[PATH_MAX];        /**< resolved executable, "" if !resolved */
    char               install_path[PATH_MAX];/**< ~/.hull/tools/<name>, "" if unknown */
    unsigned long long size_bytes;    /**< managed install size (recursive for a bundle
                                           directory, stat for a single file); 0 if
                                           not managed / unknown */
} HlToolStatus;

/**
 * @brief Recursively sum the on-disk byte size of a file or directory tree.
 *
 * Uses lstat and does not follow symlinks into other trees, so the result is
 * an extracted bundle's real footprint. Returns 0 on a missing path or error.
 * Recursion depth is bounded by the tree's real nesting.
 */
unsigned long long hl_tools_dir_size(const char *path);

/**
 * @brief Fill @p out with the unified status of @p name.
 *
 * Runs the managed-install check (works for single-binary AND bundle tools)
 * and the canonical executable resolver (`hl_tools_lookup_path`, so PATH and
 * sibling installs are seen, matching doctor/build), and computes an accurate
 * managed-install size. @p hull_exe is the running hull path (for the sibling
 * probe) or NULL.
 *
 * @returns 0 on success (state is carried in @p out), -1 on a NULL/invalid
 *          argument or an unregistered name.
 */
int hl_tools_status(const char *name, const char *hull_exe, HlToolStatus *out);

#endif /* HL_TOOLS_INSTALL_H */
