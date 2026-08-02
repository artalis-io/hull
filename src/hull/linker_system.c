/*
 * linker_system.c — system linker backend + linker selection
 *
 * Invokes cc/gcc/clang as the link driver (it resolves crt startup + libc
 * / libSystem for the host). This is the "compiler-free but not linker-free"
 * default; embedded lld/mold backends (a fully toolchain-free box) are a
 * later addition behind the same vtable. See docs/compiler_free_build.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/linker.h"
#include "hull/cap/tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define HL_LINKER_MAX_ARGS 256

typedef struct {
    char cc[PATH_MAX];
} SysCtx;

static const char *sys_name(HlLinker *l)
{
    SysCtx *ctx = (SysCtx *)l->ctx;
    const char *slash = strrchr(ctx->cc, '/');
    return slash ? slash + 1 : ctx->cc;
}

static int sys_is_available(HlLinker *l)
{
    SysCtx *ctx = (SysCtx *)l->ctx;
    const char *argv[] = { ctx->cc, "--version", NULL };
    char *out = hl_tool_spawn_read(argv, NULL);
    if (out) { free(out); return 1; }
    return 0;
}

static int sys_link(HlLinker *l, const char *output,
                    const char **objs, const char **libs,
                    const HlLinkTarget *tgt)
{
    SysCtx *ctx = (SysCtx *)l->ctx;
    const char *argv[HL_LINKER_MAX_ARGS];
    int n = 0;
    argv[n++] = ctx->cc;
    argv[n++] = "-o"; argv[n++] = output;
    /* The system cc infers format/arch from the objects natively; tgt is
     * reserved for cross / embedded-linker backends. Reproducibility of the
     * Build-ID is handled by GNU ld's content-addressed default (see the
     * matching note in compiler.c::sys_link). */
    (void)tgt;
    for (int i = 0; objs && objs[i] && n < HL_LINKER_MAX_ARGS - 2; i++)
        argv[n++] = objs[i];
    for (int i = 0; libs && libs[i] && n < HL_LINKER_MAX_ARGS - 2; i++)
        argv[n++] = libs[i];
    argv[n] = NULL;
    return hl_tool_spawn(argv) == 0 ? 0 : -1;
}

static void sys_destroy(HlLinker *l)
{
    free(l->ctx);
    /* caller frees l itself via hl_linker_destroy macro */
}

static const HlLinkerVtable sys_vtable = {
    sys_name, sys_is_available, sys_link, sys_destroy
};

HlLinker *hl_linker_system_new(const char *cc_path)
{
    if (!cc_path) return NULL;
    SysCtx *ctx = (SysCtx *)malloc(sizeof(SysCtx));
    if (!ctx) return NULL;
    snprintf(ctx->cc, sizeof(ctx->cc), "%s", cc_path);
    HlLinker *l = (HlLinker *)malloc(sizeof(HlLinker));
    if (!l) { free(ctx); return NULL; }
    l->vtable = &sys_vtable;
    l->ctx    = ctx;
    return l;
}

/* ── Selection ──────────────────────────────────────────────────── */

HlLinker *hl_linker_select(const char *explicit_linker, const char *hull_exe)
{
    (void)hull_exe;   /* reserved for future embedded lld/mold resolution */

    int is_system = (explicit_linker && strcmp(explicit_linker, "system") == 0);
    if (explicit_linker && !is_system) {
        HlLinker *l = hl_linker_system_new(explicit_linker);
        if (l && hl_linker_is_available(l)) return l;
        if (l) hl_linker_destroy(l);
        fprintf(stderr, "hull: linker '%s' not found in PATH\n", explicit_linker);
        return NULL;
    }

#ifdef __COSMOPOLITAN__
    /* Cosmo hull links cosmo-format archives → prefer cosmocc as the driver
     * (mirrors compiler.c's cosmo resolution). */
    {
        const char *home = getenv("HOME");
        char path[512];
        if (home && *home) {
            int m = snprintf(path, sizeof(path), "%s/.cosmocc/bin/cosmocc", home);
            if (m > 0 && (size_t)m < sizeof(path)) {
                HlLinker *l = hl_linker_system_new(path);
                if (l && hl_linker_is_available(l)) return l;
                if (l) hl_linker_destroy(l);
            }
            m = snprintf(path, sizeof(path), "%s/cosmocc/bin/cosmocc", home);
            if (m > 0 && (size_t)m < sizeof(path)) {
                HlLinker *l = hl_linker_system_new(path);
                if (l && hl_linker_is_available(l)) return l;
                if (l) hl_linker_destroy(l);
            }
        }
        HlLinker *l = hl_linker_system_new("/opt/cosmo/bin/cosmocc");
        if (l && hl_linker_is_available(l)) return l;
        if (l) hl_linker_destroy(l);
        l = hl_linker_system_new("cosmocc");
        if (l && hl_linker_is_available(l)) return l;
        if (l) hl_linker_destroy(l);
    }
#endif

    static const char *candidates[] = { "cc", "gcc", "clang", NULL };
    for (const char **p = candidates; *p; p++) {
        HlLinker *l = hl_linker_system_new(*p);
        if (l && hl_linker_is_available(l)) return l;
        if (l) hl_linker_destroy(l);
    }
    return NULL;
}
