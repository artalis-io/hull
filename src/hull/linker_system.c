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
#include "hull/tools_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

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
    /* --linker=lld: drive cc with a side-loaded lld (Tier A). Resolve the
     * `lld` tool (~/.hull/tools -> dirname(hull) -> $PATH); its directory holds
     * the personality names cc's `-fuse-ld=lld` needs (ld.lld on ELF, ld64.lld
     * on Mach-O), which `hull tools install lld` places alongside it. The tool
     * name must be dotless (hl_tools_name_valid), so we look up `lld`, not
     * `ld.lld`. docs/toolchain_free_build.md */
    if (explicit_linker && strcmp(explicit_linker, "lld") == 0) {
        char lld[PATH_MAX] = {0};
        hl_tools_lookup_path("lld", hull_exe, lld, sizeof lld);
        HlLinker *l = hl_linker_lld_new("cc", lld[0] ? lld : NULL);
        if (l && hl_linker_is_available(l)) return l;
        if (l) hl_linker_destroy(l);
        fprintf(stderr, "hull: --linker=lld requested but no usable lld + cc was found "
                        "(install lld with `hull tools install lld`, or put ld.lld on PATH)\n");
        return NULL;
    }

    /* --linker=lld-static: Tier B. Invoke ld.lld DIRECTLY against a static musl
     * floor (crt1.o + libc.a), no C compiler. Resolve the dotless `lld` tool,
     * derive ld.lld from its directory, and discover the libc floor from
     * HULL_LIBC_DIR / the standard musl paths (crt1.o is the sentinel).
     * docs/toolchain_free_build.md. */
    if (explicit_linker && strcmp(explicit_linker, "lld-static") == 0) {
        char lld[PATH_MAX] = {0};
        hl_tools_lookup_path("lld", hull_exe, lld, sizeof lld);
        char ld[PATH_MAX] = {0};
        if (lld[0]) {
            char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", lld);
            char *s = strrchr(dir, '/');
            if (s) { *s = '\0'; snprintf(ld, sizeof ld, "%s/ld.lld", dir); }
        }
        char libdir[PATH_MAX] = {0};
        const char *env = getenv("HULL_LIBC_DIR");
        const char *cands[] = { env, "/usr/lib", "/lib", NULL };
        for (const char **c = cands; *c; c++) {
            if (!**c) continue;
            char crt[PATH_MAX];
            snprintf(crt, sizeof crt, "%s/crt1.o", *c);
            if (access(crt, R_OK) == 0) { snprintf(libdir, sizeof libdir, "%s", *c); break; }
        }
        HlLinker *l = hl_linker_lld_direct_new(ld[0] ? ld : NULL, libdir[0] ? libdir : NULL);
        if (l && hl_linker_is_available(l)) return l;
        if (l) hl_linker_destroy(l);
        fprintf(stderr, "hull: --linker=lld-static needs ld.lld + a static libc floor "
                        "(crt1.o + libc.a). Set HULL_LIBC_DIR, or use a musl system "
                        "(Alpine) with lld + musl-dev.\n");
        return NULL;
    }

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
