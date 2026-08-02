/*
 * compiler.c — System compiler backend + compiler selection
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/compiler.h"
#include "hull/cap/tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Maximum argv entries for compile/link commands */
#define HL_COMPILER_MAX_ARGS 256

/* ── System backend ─────────────────────────────────────────────── */

typedef struct {
    char cc[PATH_MAX];
} SysCtx;

static const char *sys_name(HlCompiler *c)
{
    SysCtx *ctx = (SysCtx *)c->ctx;
    /* Return just the basename */
    const char *slash = strrchr(ctx->cc, '/');
    return slash ? slash + 1 : ctx->cc;
}

static int sys_is_available(HlCompiler *c)
{
    SysCtx *ctx = (SysCtx *)c->ctx;
    const char *argv[] = { ctx->cc, "--version", NULL };
    char *out = hl_tool_spawn_read(argv, NULL);
    if (out) { free(out); return 1; }
    return 0;
}

static char *sys_version(HlCompiler *c)
{
    SysCtx *ctx = (SysCtx *)c->ctx;
    const char *argv[] = { ctx->cc, "--version", NULL };
    char *out = hl_tool_spawn_read(argv, NULL);
    if (!out) return NULL;
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    return out; /* caller frees */
}

static int sys_compile(HlCompiler *c, const char *src, const char *obj,
                       const char *include_dir)
{
    SysCtx *ctx = (SysCtx *)c->ctx;
    const char *argv[HL_COMPILER_MAX_ARGS];
    int n = 0;

    /* Reproducibility: strip the tempdir absolute path from the .o
     * file's embedded source-name (STT_FILE on ELF, N_OSO on Mach-O).
     * `hull build` creates a random `/tmp/hull_XXXXXX/` tempdir per
     * invocation; without this, two builds of the same source produce
     * .o files whose embedded paths differ, which on Linux feeds into
     * the link-time Build-ID hash and breaks byte-identity.
     *
     * `-ffile-prefix-map` is gcc 8+ / clang 10+; both are present on
     * supported Linux distros and macOS Xcode.
     *
     * The buffer lives on the stack for the lifetime of this call,
     * which is fine since hl_tool_spawn() runs execve and returns
     * before we exit. */
    char map_buf[PATH_MAX + 32];
    char srcdir[PATH_MAX];
    snprintf(srcdir, sizeof(srcdir), "%s", src);
    char *slash = strrchr(srcdir, '/');
    if (slash) {
        *slash = '\0';
        snprintf(map_buf, sizeof(map_buf), "-ffile-prefix-map=%s=.", srcdir);
    } else {
        /* src has no directory component; nothing to map */
        map_buf[0] = '\0';
    }

    argv[n++] = ctx->cc;
    argv[n++] = "-std=c11"; argv[n++] = "-O2"; argv[n++] = "-w";
    if (map_buf[0]) argv[n++] = map_buf;
    argv[n++] = "-c";
    if (include_dir) { argv[n++] = "-I"; argv[n++] = include_dir; }
    argv[n++] = "-o"; argv[n++] = obj;
    argv[n++] = src;
    argv[n]   = NULL;
    return hl_tool_spawn(argv) == 0 ? 0 : -1;
}

static void sys_destroy(HlCompiler *c)
{
    free(c->ctx);
    /* caller frees c itself via hl_compiler_destroy macro */
}

static const HlCompilerVtable sys_vtable = {
    sys_name, sys_is_available, sys_version,
    sys_compile, sys_destroy
};

HlCompiler *hl_compiler_system_new(const char *cc_path)
{
    if (!cc_path) return NULL;
    SysCtx *ctx = (SysCtx *)malloc(sizeof(SysCtx));
    if (!ctx) return NULL;
    snprintf(ctx->cc, sizeof(ctx->cc), "%s", cc_path);
    HlCompiler *c = (HlCompiler *)malloc(sizeof(HlCompiler));
    if (!c) { free(ctx); return NULL; }
    c->vtable = &sys_vtable;
    c->ctx    = ctx;
    return c;
}

/* ── Native driver resolution (shared with the linker) ──────────── */

/* Is `path` a runnable toolchain driver? Probe `<path> --version`. */
static int driver_ok(const char *path)
{
    const char *argv[] = { path, "--version", NULL };
    char *out = hl_tool_spawn_read(argv, NULL);
    if (!out) return 0;
    free(out);
    return 1;
}

int hl_driver_resolve_native(char *out, size_t outsz)
{
    if (!out || outsz == 0) return -1;

#ifdef __COSMOPOLITAN__
    /* Cosmo hull: prefer cosmocc since the embedded platform .a's are
     * cosmo-format archives. Native cc/gcc/clang would link them to ELF/Mach-O
     * (not APE), so the output wouldn't be a portable cosmo binary anyway.
     * Check known install locations before $PATH because the cosmocc.zip
     * installer typically doesn't put cosmocc on PATH by default. §3.1 of
     * roadmap_next.md. */
    {
        const char *home = getenv("HOME");
        char path[512];
        if (home && *home) {
            int n = snprintf(path, sizeof(path), "%s/.cosmocc/bin/cosmocc", home);
            if (n > 0 && (size_t)n < sizeof(path) && driver_ok(path)) {
                snprintf(out, outsz, "%s", path); return 0;
            }
            n = snprintf(path, sizeof(path), "%s/cosmocc/bin/cosmocc", home);
            if (n > 0 && (size_t)n < sizeof(path) && driver_ok(path)) {
                snprintf(out, outsz, "%s", path); return 0;
            }
        }
        if (driver_ok("/opt/cosmo/bin/cosmocc")) {
            snprintf(out, outsz, "/opt/cosmo/bin/cosmocc"); return 0;
        }
        if (driver_ok("cosmocc")) { snprintf(out, outsz, "cosmocc"); return 0; }
        /* Fall through to cc/gcc/clang. They won't produce a working APE
         * against cosmo .a's, but letting them resolve gives a clearer
         * link-time error than a silent "no compiler found." */
    }
#endif

    static const char *candidates[] = { "cc", "gcc", "clang", NULL };
    for (const char **p = candidates; *p; p++)
        if (driver_ok(*p)) { snprintf(out, outsz, "%s", *p); return 0; }

    return -1;
}

/* ── Selection ──────────────────────────────────────────────────── */

HlCompiler *hl_compiler_select(const char *explicit_cc)
{
    /* "system" sentinel → force auto-detect below (kept for symmetry with
     * `hull build --compiler=system`; the emit path is the default now). */
    int is_system = (explicit_cc && strcmp(explicit_cc, "system") == 0);

    /* Explicit named compiler (a path or a PATH name). */
    if (explicit_cc && !is_system) {
        HlCompiler *c = hl_compiler_system_new(explicit_cc);
        if (c && hl_compiler_is_available(c)) return c;
        if (c) hl_compiler_destroy(c);
        fprintf(stderr, "hull: compiler '%s' not found in PATH\n", explicit_cc);
        return NULL;
    }

    /* Auto: the shared native-driver resolver (cc/gcc/clang; cosmocc on a
     * cosmo hull). */
    char cc[PATH_MAX];
    if (hl_driver_resolve_native(cc, sizeof cc) == 0)
        return hl_compiler_system_new(cc);
    return NULL;
}
