/*
 * tools_install.c — Side-loaded Hull tool registry + path helpers.
 *
 * This module is the trust boundary for the `hull tools install` family
 * of commands. It exposes:
 *   - A compile-time-constant registry of known tools (currently:
 *     `wamrc`).
 *   - Strict name validation (`hl_tools_name_valid`) so callers
 *     never build paths from unvalidated strings.
 *   - The on-disk install path resolution (`hl_tools_dir`,
 *     `hl_tools_install_path`) used by the install / uninstall paths.
 *   - The lookup helper (`hl_tools_lookup_path`) used by hull's
 *     consumers of these tools (`cap/wasm.c` for `wamrc`,
 *     `commands/doctor.c` for status reporting).
 *
 * No HTTPS or signature verification lives here — those concerns are
 * in `release_io.{h,c}` (network I/O) and `commands/tools.c`
 * (orchestration).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/tools_install.h"
#include "hull/shared/fs_util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Registry ────────────────────────────────────────────────────────
 *
 * Static table; entries are NUL-terminated by an all-zeros sentinel.
 * Adding a tool here is half of the install-path wiring; the other
 * half is publishing matching `hull-<name>-<platform>` artifacts in
 * the release workflow and listing them in `hull.sha256`.
 */
static const HlToolSpec REGISTRY[] = {
    {
        .name              = "wamrc",
        .description       =
            "WAMR AOT compiler — produces compute/*.aot.<arch> modules "
            "(~50× faster than the interpreter on compute-heavy WASM).",
        .has_linux_x86_64  = 1,
        .has_linux_aarch64 = 1,
        .has_darwin_arm64  = 1,
        /* LLVM is too large to bundle into a cosmocc fat APE binary; cosmo
         * users build from source with `make wamrc`. See docs/tools_install.md. */
        .has_cosmo         = 0,
    },
    /* The static-link floor bundles for `hull build --linker=lld-static` (Tier B):
     * crt*.o + libc.a + the stub libm/libpthread + libgcc.a, so a box with only
     * hull + ld.lld can link a fully static musl binary - no musl-dev, no gcc.
     * Arch-baked into the name (one row per arch, each published only for its
     * platform) so the install dir matches the linker's ~/.hull/tools/
     * libc-musl-<arch>/ lookup. Native-only; musl is Linux. See docs/musl_build.md. */
    {
        .name              = "libc-musl-x86_64",
        .description       = "musl static-link floor (crt/libc/libgcc) for "
                             "`hull build --linker=lld-static` on x86_64.",
        .has_linux_x86_64  = 1,
        .is_bundle         = 1,
        .bundle_entry      = "crt1.o",   /* data-only floor; not exec-resolved */
    },
    {
        .name              = "libc-musl-aarch64",
        .description       = "musl static-link floor (crt/libc/libgcc) for "
                             "`hull build --linker=lld-static` on aarch64.",
        .has_linux_aarch64 = 1,
        .is_bundle         = 1,
        .bundle_entry      = "crt1.o",
    },
    { 0 }  /* sentinel */
};

const HlToolSpec *hl_tools_registry(void)
{
    return REGISTRY;
}

const HlToolSpec *hl_tools_find(const char *name)
{
    if (!name) return NULL;
    for (const HlToolSpec *t = REGISTRY; t->name; t++) {
        if (strcmp(t->name, name) == 0) return t;
    }
    return NULL;
}

/* ── Name validation ─────────────────────────────────────────────────
 *
 * Tool names appear in filesystem paths (`$HOME/.hull/tools/<name>`)
 * and in URLs (`hull-<name>-<platform>`). Restrict the character set
 * tightly so neither ever sees a `..`, slash, NUL, or anything
 * resembling a path component.
 */
int hl_tools_name_valid(const char *name)
{
    if (!name) return 0;
    size_t n = 0;
    for (const char *p = name; *p; p++, n++) {
        if (n + 1 >= HL_TOOL_NAME_MAX) return 0;
        char c = *p;
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return 0;
    }
    return n > 0;
}

/* ── Platform / asset helpers ────────────────────────────────────── */

int hl_tools_published_for(const HlToolSpec *spec, const char *platform)
{
    if (!spec || !platform) return 0;
    if (strcmp(platform, "linux-x86_64")  == 0) return spec->has_linux_x86_64;
    if (strcmp(platform, "linux-aarch64") == 0) return spec->has_linux_aarch64;
    if (strcmp(platform, "darwin-arm64")  == 0) return spec->has_darwin_arm64;
    if (strcmp(platform, "cosmo")         == 0) return spec->has_cosmo;
    return 0;
}

int hl_tools_asset_name(const HlToolSpec *spec, const char *platform,
                        char *out, size_t out_sz)
{
    if (!spec || !platform || !out || out_sz == 0) return -1;
    if (!hl_tools_published_for(spec, platform))   return -1;
    /* A bundle's arch is already baked into its name (libc-musl-<arch>) and it
     * is published for exactly that one platform, so the asset is
     * `hull-<name>.tar` - no redundant platform suffix. A binary tool keeps the
     * per-platform `hull-<name>-<platform>` form. */
    int n = spec->is_bundle
        ? snprintf(out, out_sz, "hull-%s.tar", spec->name)
        : snprintf(out, out_sz, "hull-%s-%s", spec->name, platform);
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return 0;
}


int hl_tools_dir(char *out, size_t out_sz)
{
    if (!out || out_sz < 2) return -1;

    const char *home = getenv("HOME");
    if (!home || !*home) {
        errno = ENOENT;
        return -1;
    }

    /* `$HOME/.hull` */
    char hull_dir[PATH_MAX];
    int n = snprintf(hull_dir, sizeof(hull_dir), "%s/.hull", home);
    if (n < 0 || (size_t)n >= sizeof(hull_dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (hl_ensure_dir(hull_dir, 0755) != 0) return -1;

    /* `$HOME/.hull/tools` */
    char tools_dir[PATH_MAX];
    n = snprintf(tools_dir, sizeof(tools_dir), "%s/tools", hull_dir);
    if (n < 0 || (size_t)n >= sizeof(tools_dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (hl_ensure_dir(tools_dir, 0755) != 0) return -1;

    /* Return with trailing slash for easy concatenation. */
    n = snprintf(out, out_sz, "%s/", tools_dir);
    if (n < 0 || (size_t)n >= out_sz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int hl_tools_install_path(const char *name, char *out, size_t out_sz)
{
    if (!name || !out || out_sz == 0) return -1;
    if (!hl_tools_name_valid(name)) {
        errno = EINVAL;
        return -1;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        errno = ENOENT;
        return -1;
    }
    int n = snprintf(out, out_sz, "%s/.hull/tools/%s", home, name);
    if (n < 0 || (size_t)n >= out_sz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/* ── Lookup ───────────────────────────────────────────────────────── */

/* `dirname()` is allowed to modify its input AND returns a static
 * buffer on some platforms — never use it. Strip the trailing path
 * component ourselves. Returns 0 if a directory exists, -1 if the
 * input has no separator. */
static int strip_basename(const char *path, char *out, size_t out_sz)
{
    if (!path || !out || out_sz == 0) return -1;
    const char *last = strrchr(path, '/');
    if (!last) return -1;
    size_t len = (size_t)(last - path);
    if (len == 0) {
        /* Path was "/foo" — directory is "/". */
        if (out_sz < 2) return -1;
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }
    if (len + 1 > out_sz) return -1;
    memcpy(out, path, len);
    out[len] = '\0';
    return 0;
}

/* Walk PATH and return the first directory containing an executable
 * `name`. Output buffer holds the full path on success. */
static int find_on_path(const char *name, char *out, size_t out_sz)
{
    const char *path = getenv("PATH");
    if (!path || !*path) return -1;

    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t seg_len = colon ? (size_t)(colon - p) : strlen(p);

        /* Empty segment ('::' or leading ':') means current directory
         * — POSIX-ism; skip for safety. */
        if (seg_len > 0) {
            /* Compose "<seg>/<name>" in a stack buffer first so we
             * can `access()` it without trampling the caller's out. */
            char cand[PATH_MAX];
            if (seg_len + 1 + strlen(name) + 1 <= sizeof(cand)) {
                memcpy(cand, p, seg_len);
                cand[seg_len] = '/';
                size_t nlen = strlen(name);
                memcpy(cand + seg_len + 1, name, nlen);
                cand[seg_len + 1 + nlen] = '\0';
                if (access(cand, X_OK) == 0) {
                    int n = snprintf(out, out_sz, "%s", cand);
                    if (n < 0 || (size_t)n >= out_sz) return -1;
                    return 0;
                }
            }
        }

        if (!colon) break;
        p = colon + 1;
    }
    return -1;
}

int hl_tools_lookup_path(const char *name, const char *hull_exe,
                         char *out, size_t out_sz)
{
    if (!name || !out || out_sz == 0) return -1;
    if (!hl_tools_name_valid(name)) {
        errno = EINVAL;
        return -1;
    }

    /* 0) An installed bundle: the toolchain executable lives INSIDE the
     *    extracted dir at $HOME/.hull/tools/<name>/<bundle_entry> (e.g. zig,
     *    lld). A data-only bundle (the floor) has a non-exec bundle_entry and
     *    won't match X_OK here, so it falls through harmlessly. */
    const HlToolSpec *bspec = hl_tools_find(name);
    if (bspec && bspec->is_bundle && bspec->bundle_entry) {
        char bdir[PATH_MAX];
        if (hl_tools_install_path(name, bdir, sizeof(bdir)) == 0) {
            char be[PATH_MAX];
            int n = snprintf(be, sizeof(be), "%s/%s", bdir, bspec->bundle_entry);
            if (n > 0 && (size_t)n < sizeof(be) && access(be, X_OK) == 0) {
                int m = snprintf(out, out_sz, "%s", be);
                if (m < 0 || (size_t)m >= out_sz) return -1;
                return 0;
            }
        }
        /* not installed as a bundle → fall through to PATH (a system zig/lld). */
    }

    /* 1) $HOME/.hull/tools/<name>  ── canonical install location.
     *    Use hl_tools_install_path so HOME-unset failures are surfaced
     *    identically across helpers. */
    char cand[PATH_MAX];
    if (hl_tools_install_path(name, cand, sizeof(cand)) == 0 &&
        access(cand, X_OK) == 0) {
        int n = snprintf(out, out_sz, "%s", cand);
        if (n < 0 || (size_t)n >= out_sz) return -1;
        return 0;
    }

    /* 2) dirname(hull_exe)/<name>  ── ejected / portable installs. */
    if (hull_exe && *hull_exe) {
        char dir[PATH_MAX];
        if (strip_basename(hull_exe, dir, sizeof(dir)) == 0) {
            int n = snprintf(cand, sizeof(cand), "%s/%s", dir, name);
            if (n > 0 && (size_t)n < sizeof(cand) && access(cand, X_OK) == 0) {
                int m = snprintf(out, out_sz, "%s", cand);
                if (m < 0 || (size_t)m >= out_sz) return -1;
                return 0;
            }
        }
    }

    /* 3) PATH lookup. */
    if (find_on_path(name, cand, sizeof(cand)) == 0) {
        int n = snprintf(out, out_sz, "%s", cand);
        if (n < 0 || (size_t)n >= out_sz) return -1;
        return 0;
    }

    return -1;
}
