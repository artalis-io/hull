/*
 * linker_lld.c — LLVM lld linker backend (Tier A)
 *
 * Links through a side-loaded lld (`hull tools install lld`) by driving the
 * system cc with `-B<lld_dir> -fuse-ld=lld`, so cc still supplies the crt +
 * libc floor while lld does the actual link. This is the first tier of the
 * toolchain-free axis (docs/toolchain_free_build.md); Tier B (invoking lld
 * directly with a bundled crt/libc) removes the cc dependency and is a
 * follow-up. Selected with `hull build --linker=lld`.
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

#define HL_LINKER_MAX_ARGS 256

typedef struct {
    char cc[PATH_MAX];        /* the driver (cc/gcc/clang) */
    char bprefix[PATH_MAX];   /* "-B<dir>" where <dir> holds ld.lld / ld64.lld */
} LldCtx;

static const char *lld_name(HlLinker *l) { (void)l; return "lld"; }

static int lld_is_available(HlLinker *l)
{
    LldCtx *ctx = (LldCtx *)l->ctx;
    const char *argv[] = { ctx->cc, "--version", NULL };
    char *out = hl_tool_spawn_read(argv, NULL);
    if (!out) return 0;             /* no driver */
    free(out);
    return ctx->bprefix[0] != '\0'; /* and an lld was resolved */
}

static int lld_link(HlLinker *l, const char *output,
                    const char **objs, const char **libs,
                    const HlLinkTarget *tgt)
{
    LldCtx *ctx = (LldCtx *)l->ctx;
    const char *argv[HL_LINKER_MAX_ARGS];
    int n = 0;
    argv[n++] = ctx->cc;
    argv[n++] = ctx->bprefix;       /* -B<lld_dir>: cc finds ld.lld here */
    argv[n++] = "-fuse-ld=lld";
    argv[n++] = "-o"; argv[n++] = output;
    /* tgt (format/arch) is reserved for the Tier B cross path, where lld is
     * invoked directly with an explicit target + bundled crt/libc. Under
     * Tier A the driving cc infers the native target. */
    (void)tgt;
    for (int i = 0; objs && objs[i] && n < HL_LINKER_MAX_ARGS - 2; i++)
        argv[n++] = objs[i];
    for (int i = 0; libs && libs[i] && n < HL_LINKER_MAX_ARGS - 2; i++)
        argv[n++] = libs[i];
    argv[n] = NULL;
    return hl_tool_spawn(argv) == 0 ? 0 : -1;
}

static void lld_destroy(HlLinker *l)
{
    free(l->ctx);
    /* caller frees l itself via hl_linker_destroy macro */
}

static const HlLinkerVtable lld_vtable = {
    lld_name, lld_is_available, lld_link, lld_destroy
};

/*
 * Create the lld backend. cc_path is the driver; lld_bin is a resolved
 * ld.lld / ld64.lld executable (its directory becomes the -B prefix). Either
 * may be NULL, yielding an unavailable backend (is_available() returns 0).
 */
HlLinker *hl_linker_lld_new(const char *cc_path, const char *lld_bin)
{
    LldCtx *ctx = (LldCtx *)calloc(1, sizeof(LldCtx));
    if (!ctx) return NULL;
    if (cc_path) snprintf(ctx->cc, sizeof(ctx->cc), "%s", cc_path);
    if (lld_bin && *lld_bin) {
        /* -B<dirname(lld_bin)> so `-fuse-ld=lld` resolves to this lld. */
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", lld_bin);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = '\0'; snprintf(ctx->bprefix, sizeof(ctx->bprefix), "-B%s", dir); }
    }
    HlLinker *l = (HlLinker *)malloc(sizeof(HlLinker));
    if (!l) { free(ctx); return NULL; }
    l->vtable = &lld_vtable;
    l->ctx    = ctx;
    return l;
}
