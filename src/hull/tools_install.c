/*
 * tools_install.c - Side-loaded Hull tool registry + path helpers.
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
 * No HTTPS or signature verification lives here - those concerns are
 * in `release_io.{h,c}` (network I/O) and `commands/tools.c`
 * (orchestration).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/tools_install.h"
#include "hull/shared/fs_util.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Registry ────────────────────────────────────────────────────────
 *
 * Static table; entries are NUL-terminated by an all-zeros sentinel. This
 * registry is the CONSUMER-side source of truth (install / lookup / list); it is
 * NOT the only edit site. Adding a tool is TWO coordinated edits: (1) a row here,
 * and (2) publishing matching `hull-<name>[-<platform>]` (binary) or
 * `hull-<name>[-<platform>].tar` (bundle) artifacts in .github/workflows/
 * release.yml (the build job + the flatten/sha256/attest/publish lists). The two
 * are kept from drifting by scripts/check_tools_registry.sh (a CI guard asserting
 * every registry name has a matching release.yml asset) + the post-release
 * tests/release_smoke.sh (which actually fetches + verifies each). See T4d.
 */
static const HlToolSpec REGISTRY[] = {
    {
        .name              = "wamrc",
        .description       =
            "WAMR AOT compiler - produces compute/*.aot.<arch> modules "
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
    /* The musl-built Hull platform archive SET (libhull_platform.a + slim +
     * every libhull_feature-*.a) for `hull build --target=<arch>-linux-musl`
     * (cross via zig) or `--linker=lld-static` (Tier B). The counterpart of the
     * libc-musl floor above: the floor is the musl libc, this is Hull's own
     * archives, musl-built. A data-only bundle extracting to $HOME/.hull/tools/
     * platform-musl-<arch>/; build.lua resolves the dir via the sentinel
     * bundle_entry libhull_platform.a. Unlike the floor (on-musl-host only),
     * this is a CROSS-target asset: you install it on ANY native host to
     * cross-build TO musl, so all three native platforms can fetch it (the tar
     * bytes are host-independent). No cosmo (a fat APE can't drive a native
     * cross tree). Asset: hull-platform-musl-<arch>.tar. See docs/musl_build.md
     * + audit #4c. */
    {
        .name              = "platform-musl-x86_64",
        .description       = "musl-built Hull platform archive set for "
                             "`hull build --target=x86_64-linux-musl`.",
        .has_linux_x86_64  = 1,
        .has_linux_aarch64 = 1,
        .has_darwin_arm64  = 1,
        .is_bundle         = 1,
        .bundle_entry      = "libhull_platform.a", /* data-only; dir sentinel */
    },
    {
        .name              = "platform-musl-aarch64",
        .description       = "musl-built Hull platform archive set for "
                             "`hull build --target=aarch64-linux-musl`.",
        .has_linux_x86_64  = 1,
        .has_linux_aarch64 = 1,
        .has_darwin_arm64  = 1,
        .is_bundle         = 1,
        .bundle_entry      = "libhull_platform.a",
    },
    /* The toolchain-free LINKER for `hull build --linker=zig --target=<triple>`.
     * A multi-file tree shipped as a per-platform bundle (arch-free name, one
     * artifact per native platform -> hull-zig-<platform>.tar). The bundle
     * extracts to $HOME/.hull/tools/zig/ and the driver at zig/<bundle_entry> is
     * what hl_tools_lookup_path resolves. zig is fully self-contained (a static
     * driver + its libc/compiler-rt tree, and its OWN bundled lld), so it runs
     * anywhere. Native-only (cosmo can't drive a native zig tree). See
     * docs/toolchain_free_build.md.
     *
     * NOTE: a standalone `lld` bundle is intentionally NOT registered. Every
     * binary lld distribution (Homebrew / apt / LLVM release) is DYNAMICALLY
     * linked against libLLVM + the lld*.dylib set (an absolute-path
     * libLLVM.dylib on macOS), so a flat bundle of just the binaries can't run
     * portably. A runnable lld bundle needs a statically-linked lld (a full
     * LLVM source build) - a tracked follow-up. `hull build --linker=lld` still
     * works against a system / PATH lld (its dylibs intact on the user's box). */
    {
        .name                 = "zig",
        .description          = "Zig toolchain (self-contained: driver + libc + "
                                "its own lld) for `hull build --linker=zig "
                                "--target=<triple>` - turnkey cross-compilation.",
        .has_linux_x86_64     = 1,
        .has_linux_aarch64    = 1,
        .has_darwin_arm64     = 1,
        .is_bundle            = 1,
        .bundle_entry         = "zig",   /* the zig driver binary */
        .bundle_per_platform  = 1,
    },
    /* The trimmed Cosmopolitan cosmocc toolchain (+ a busybox-w64 ride-along at
     * bin/busybox.exe) for `hull build` of a cosmo-APE app - the ONLY toolchain
     * that can link an APE (obj_emit has no APE format). cosmocc binaries are
     * themselves APEs, so ONE arch-free bundle (hull-cosmocc.tar) serves every
     * host and a cosmo `hull` can drive it. UNLIKE every other tool here, this is
     * COSMO-ONLY (has_cosmo = 1, all natives 0): a native hull resolves only
     * cc/gcc/clang (the cosmocc branch of hl_driver_resolve_native is
     * `#ifdef __COSMOPOLITAN__`), so only a cosmo hull can use it. On Windows the
     * cosmo hull drives cosmocc's #!/bin/sh driver through the bundled busybox
     * (item A); the toolchain is trimmed from ~1.37 GB to fit the 512 MB download
     * cap (item C). bundle_entry bin/cosmocc is the exec resolved by
     * hl_tools_lookup_path (also probed directly by the compiler resolver, item
     * D). Producer: scripts/build_cosmocc_bundle.sh. Full design:
     * docs/cosmocc_install.md. */
    {
        .name                 = "cosmocc",
        .description          = "Cosmopolitan cosmocc toolchain (trimmed) + "
                                "busybox-w64, for `hull build` of a cosmo-APE app "
                                "on any host (incl. Windows).",
        .has_cosmo            = 1,
        .is_bundle            = 1,
        .bundle_entry         = "bin/cosmocc",
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
    /* Three asset shapes:
     *   - a binary tool:            `hull-<name>-<platform>`  (wamrc)
     *   - a single-platform bundle: `hull-<name>.tar`         (libc-musl-<arch>:
     *       the arch is already in the name, published for one platform)
     *   - a per-platform bundle:    `hull-<name>-<platform>.tar`  (lld / zig:
     *       arch-free name, one artifact per native platform) */
    int n;
    if (!spec->is_bundle)
        n = snprintf(out, out_sz, "hull-%s-%s", spec->name, platform);
    else if (spec->bundle_per_platform)
        n = snprintf(out, out_sz, "hull-%s-%s.tar", spec->name, platform);
    else
        n = snprintf(out, out_sz, "hull-%s.tar", spec->name);
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return 0;
}


/* Home directory, with a Windows fallback. A cosmo `hull` running on Windows
 * has no $HOME (Windows populates $USERPROFILE), so fall back to it - keeps
 * `$HOME/.hull/...` resolution working when a cosmo-APE hull runs natively on
 * Windows (see docs/cosmocc_install.md, spec item D). */
static const char *tools_home(void)
{
    const char *home = getenv("HOME");
    if (home && *home) return home;
    home = getenv("USERPROFILE");
    return (home && *home) ? home : NULL;
}

int hl_tools_dir(char *out, size_t out_sz)
{
    if (!out || out_sz < 2) return -1;

    const char *home = tools_home();
    if (!home) {
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
    const char *home = tools_home();
    if (!home) {
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
 * buffer on some platforms - never use it. Strip the trailing path
 * component ourselves. Returns 0 if a directory exists, -1 if the
 * input has no separator. */
static int strip_basename(const char *path, char *out, size_t out_sz)
{
    if (!path || !out || out_sz == 0) return -1;
    const char *last = strrchr(path, '/');
    if (!last) return -1;
    size_t len = (size_t)(last - path);
    if (len == 0) {
        /* Path was "/foo" - directory is "/". */
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
         * - POSIX-ism; skip for safety. */
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

/* ── Unified status (parity across tools-list / agent-tools / doctor) ──── */

unsigned long long hl_tools_dir_size(const char *path)
{
    struct stat st;
    if (!path || lstat(path, &st) != 0) return 0;
    if (S_ISREG(st.st_mode)) return (unsigned long long)st.st_size;
    if (!S_ISDIR(st.st_mode)) return 0;   /* symlink / special: don't follow */

    DIR *d = opendir(path);
    if (!d) return 0;
    unsigned long long total = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) continue;
        total += hl_tools_dir_size(child);
    }
    closedir(d);
    return total;
}

int hl_tools_status(const char *name, const char *hull_exe, HlToolStatus *out)
{
    if (!name || !out) return -1;
    memset(out, 0, sizeof(*out));
    const HlToolSpec *spec = hl_tools_find(name);
    if (!spec) return -1;

    /* Managed-install check - works for a single-binary tool (a symlink to the
     * CAS blob, stat() follows it -> regular file) AND a bundle (a directory
     * whose bundle_entry sentinel is present). This is the "installed via
     * `hull tools install`, uninstallable" concept, independent of executable
     * resolution below (a data-only floor is managed but not resolvable). */
    if (hl_tools_install_path(name, out->install_path,
                              sizeof(out->install_path)) == 0) {
        struct stat st;
        if (stat(out->install_path, &st) == 0) {
            if (spec->is_bundle) {
                if (S_ISDIR(st.st_mode) && spec->bundle_entry) {
                    char sentinel[PATH_MAX];
                    int n = snprintf(sentinel, sizeof(sentinel), "%s/%s",
                                     out->install_path, spec->bundle_entry);
                    struct stat cs;
                    if (n > 0 && (size_t)n < sizeof(sentinel) &&
                        stat(sentinel, &cs) == 0 && S_ISREG(cs.st_mode)) {
                        out->managed    = 1;
                        out->size_bytes = hl_tools_dir_size(out->install_path);
                    }
                }
            } else if (S_ISREG(st.st_mode)) {
                out->managed    = 1;
                out->size_bytes = (unsigned long long)st.st_size;
            }
        }
    }

    /* Executable resolution - the SAME order doctor + build use. Classify the
     * source so callers can distinguish a hull-managed install from a
     * sibling/PATH one. Managed installs are probed first by the resolver, so a
     * present managed executable always classifies as MANAGED. */
    if (hl_tools_lookup_path(name, hull_exe, out->path, sizeof(out->path)) == 0) {
        out->resolved = 1;
        size_t ilen = strlen(out->install_path);
        if (ilen && strncmp(out->path, out->install_path, ilen) == 0 &&
            (out->path[ilen] == '\0' || out->path[ilen] == '/')) {
            out->source = HL_TOOL_SRC_MANAGED;
        } else if (hull_exe && *hull_exe) {
            char dir[PATH_MAX];
            char sib[PATH_MAX];
            int n;
            if (strip_basename(hull_exe, dir, sizeof(dir)) == 0 &&
                (n = snprintf(sib, sizeof(sib), "%s/%s", dir, name)) > 0 &&
                (size_t)n < sizeof(sib) && strcmp(out->path, sib) == 0) {
                out->source = HL_TOOL_SRC_SIBLING;
            } else {
                out->source = HL_TOOL_SRC_PATH;
            }
        } else {
            out->source = HL_TOOL_SRC_PATH;
        }
    }
    return 0;
}
