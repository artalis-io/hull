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
    argv[n++] = ctx->cc;
    argv[n++] = "-std=c11"; argv[n++] = "-O2"; argv[n++] = "-w";
    argv[n++] = "-c";
    if (include_dir) { argv[n++] = "-I"; argv[n++] = include_dir; }
    argv[n++] = "-o"; argv[n++] = obj;
    argv[n++] = src;
    argv[n]   = NULL;
    return hl_tool_spawn(argv) == 0 ? 0 : -1;
}

static int sys_link(HlCompiler *c, const char *output,
                    const char **objs, const char **libs)
{
    SysCtx *ctx = (SysCtx *)c->ctx;
    const char *argv[HL_COMPILER_MAX_ARGS];
    int n = 0;
    argv[n++] = ctx->cc;
    argv[n++] = "-o"; argv[n++] = output;
    for (int i = 0; objs && objs[i] && n < HL_COMPILER_MAX_ARGS - 2; i++)
        argv[n++] = objs[i];
    for (int i = 0; libs && libs[i] && n < HL_COMPILER_MAX_ARGS - 2; i++)
        argv[n++] = libs[i];
    argv[n] = NULL;
    return hl_tool_spawn(argv) == 0 ? 0 : -1;
}

static void sys_destroy(HlCompiler *c)
{
    free(c->ctx);
    /* caller frees c itself via hl_compiler_destroy macro */
}

static const HlCompilerVtable sys_vtable = {
    sys_name, sys_is_available, sys_version,
    sys_compile, sys_link, sys_destroy
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

/* ── Selection ──────────────────────────────────────────────────── */

HlCompiler *hl_compiler_select(const char *explicit_cc)
{
    /* Explicit "tcc" sentinel → tcc backend */
    if (explicit_cc && strcmp(explicit_cc, "tcc") == 0) {
#ifdef HL_ENABLE_TCC
        HlCompiler *t = hl_compiler_tcc_new();
        if (t && hl_compiler_is_available(t)) return t;
        if (t) hl_compiler_destroy(t);
        fprintf(stderr, "hull: tcc requested but not available on this "
                        "platform (cosmo APE archives or macOS Mach-O)\n");
#else
        fprintf(stderr, "hull: tcc not compiled into this build "
                        "(rebuild with HL_ENABLE_TCC=1)\n");
#endif
        return NULL;
    }

    /* Explicit "system" sentinel → skip tcc, force system auto-detect */
    int skip_tcc = (explicit_cc && strcmp(explicit_cc, "system") == 0);

    /* Explicit named compiler */
    if (explicit_cc && !skip_tcc) {
        HlCompiler *c = hl_compiler_system_new(explicit_cc);
        if (c && hl_compiler_is_available(c)) return c;
        if (c) hl_compiler_destroy(c);
        fprintf(stderr, "hull: compiler '%s' not found in PATH\n", explicit_cc);
        return NULL;
    }

#ifdef HL_ENABLE_TCC
    /* Auto: try embedded tcc first (if cosmo archives not present — checked
     * inside tcc_new via build_assets) */
    if (!skip_tcc) {
        HlCompiler *t = hl_compiler_tcc_new();
        if (t && hl_compiler_is_available(t)) return t;
        if (t) hl_compiler_destroy(t);
    }
#endif

    /* Auto: try system compilers in PATH */
    static const char *candidates[] = { "cc", "gcc", "clang", NULL };
    for (const char **p = candidates; *p; p++) {
        HlCompiler *c = hl_compiler_system_new(*p);
        if (c && hl_compiler_is_available(c)) return c;
        if (c) hl_compiler_destroy(c);
    }

    return NULL;
}
