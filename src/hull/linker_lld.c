/*
 * linker_lld.c — LLVM lld linker backend (Tier A + Tier B)
 *
 * Toolchain-free axis (docs/toolchain_free_build.md). Two modes behind one
 * HlLinkerVtable:
 *
 *   Tier A (cc-driven): `cc -B<lld_dir> -fuse-ld=lld` - cc supplies the crt +
 *   libc floor, lld does the link. Needs a system cc. Selected with
 *   `--linker=lld`.
 *
 *   Tier B (direct static): invoke `ld.lld -static` directly against a musl
 *   floor (crt1.o + crti.o + crtn.o + libc.a) with NO C compiler in the link.
 *   Produces a fully static binary. Selected with `--linker=lld-static`.
 *   Requires the app's archives (libhull_platform.a + feature libs) to be
 *   musl-built - i.e. a musl/Alpine hull; a glibc-built .a will not static-link
 *   against musl.
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
#include <glob.h>

#define HL_LINKER_MAX_ARGS 256

typedef struct {
    int  direct;              /* 0 = Tier A (cc-driven), 1 = Tier B (direct) */
    /* Tier A: */
    char cc[PATH_MAX];        /* the driver (cc/gcc/clang) */
    char bprefix[PATH_MAX];   /* "-B<dir>" where <dir> holds ld.lld / ld64.lld */
    /* Tier B: */
    char ld[PATH_MAX];        /* the ld.lld executable (ELF flavor) */
    char libdir[PATH_MAX];    /* dir holding crt1.o / crti.o / crtn.o / libc.a */
    char crt1[PATH_MAX], crti[PATH_MAX], crtn[PATH_MAX], ldash[PATH_MAX];
    char libgcc[PATH_MAX];    /* the compiler runtime (libgcc.a / compiler-rt) */
} LldCtx;

static const char *lld_name(HlLinker *l)
    { return ((LldCtx *)l->ctx)->direct ? "lld-static" : "lld"; }

static int lld_is_available(HlLinker *l)
{
    LldCtx *ctx = (LldCtx *)l->ctx;
    if (ctx->direct)
        return ctx->ld[0] != '\0' && ctx->libdir[0] != '\0';
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
    (void)tgt;  /* the objects already carry the target format/arch */

    if (ctx->direct) {
        /* ld.lld -static -o out crt1 crti <objs> --start-group <libs> -lc
         * --end-group -L<libdir> crtn. */
        argv[n++] = ctx->ld;
        argv[n++] = "-static";
        argv[n++] = "-o"; argv[n++] = output;
        argv[n++] = ctx->crt1;
        argv[n++] = ctx->crti;
        for (int i = 0; objs && objs[i] && n < HL_LINKER_MAX_ARGS - 8; i++)
            argv[n++] = objs[i];
        argv[n++] = "--start-group";
        /* build.lua emits linker flags in the compiler-driver convention
         * (-Wl,X) because every OTHER backend is a driver (cc, `zig cc`). We
         * invoke ld.lld directly, so translate each lib:
         *   - strip a leading "-Wl," (driver escape) to the raw linker flag;
         *   - drop --start-group / --end-group (build.lua wraps the composed
         *     feature archives in their own group; we already wrap everything
         *     in one outer group here, and ld.lld rejects NESTED groups - a
         *     single group over all archives + libc resolves every circular
         *     dep anyway).
         * --whole-archive / --no-whole-archive pass through unchanged inside
         * the group. ELF -Wl, flags are single-token; the comma'd
         * -Wl,-force_load,<a> form is macOS/ld64-only and never reaches this
         * ELF-only path. */
        for (int i = 0; libs && libs[i] && n < HL_LINKER_MAX_ARGS - 8; i++) {
            const char *a = libs[i];
            if (strncmp(a, "-Wl,", 4) == 0) a += 4;
            if (strcmp(a, "--start-group") == 0 || strcmp(a, "--end-group") == 0)
                continue;
            argv[n++] = a;
        }
        /* The compiler runtime (libgcc / compiler-rt) supplies soft-float and
         * other builtins (e.g. 128-bit long double __multf3) that a cc/zig-cc
         * driver would auto-link but a raw ld.lld does not. Inside the group so
         * it resolves against libc + the app archives. */
        if (ctx->libgcc[0]) argv[n++] = ctx->libgcc;
        argv[n++] = "-lc";
        argv[n++] = "--end-group";
        argv[n++] = ctx->ldash;   /* -L<libdir> */
        argv[n++] = ctx->crtn;
        argv[n] = NULL;
        return hl_tool_spawn(argv) == 0 ? 0 : -1;
    }

    /* Tier A: cc drives, lld links, cc supplies the floor. */
    argv[n++] = ctx->cc;
    argv[n++] = ctx->bprefix;       /* -B<lld_dir>: cc finds ld.lld here */
    argv[n++] = "-fuse-ld=lld";
    argv[n++] = "-o"; argv[n++] = output;
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
 * Tier A: cc_path is the driver; lld_bin is a resolved ld.lld / ld64.lld (its
 * directory becomes the -B prefix). Either may be NULL (unavailable backend).
 */
HlLinker *hl_linker_lld_new(const char *cc_path, const char *lld_bin)
{
    LldCtx *ctx = (LldCtx *)calloc(1, sizeof(LldCtx));
    if (!ctx) return NULL;
    if (cc_path) snprintf(ctx->cc, sizeof(ctx->cc), "%s", cc_path);
    if (lld_bin && *lld_bin) {
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

/*
 * Tier B: invoke ld_path (ld.lld) directly with the static musl floor in
 * libdir (crt1.o / crti.o / crtn.o / libc.a). Either arg NULL -> unavailable.
 */
HlLinker *hl_linker_lld_direct_new(const char *ld_path, const char *libdir)
{
    LldCtx *ctx = (LldCtx *)calloc(1, sizeof(LldCtx));
    if (!ctx) return NULL;
    ctx->direct = 1;
    if (ld_path) snprintf(ctx->ld, sizeof(ctx->ld), "%s", ld_path);
    if (libdir && *libdir) {
        snprintf(ctx->libdir, sizeof(ctx->libdir), "%s", libdir);
        snprintf(ctx->crt1, sizeof(ctx->crt1), "%s/crt1.o", libdir);
        snprintf(ctx->crti, sizeof(ctx->crti), "%s/crti.o", libdir);
        snprintf(ctx->crtn, sizeof(ctx->crtn), "%s/crtn.o", libdir);
        snprintf(ctx->ldash, sizeof(ctx->ldash), "-L%s", libdir);
    }
    /* Discover the compiler runtime (libgcc.a) - it supplies builtins (soft-
     * float __multf3, etc.) the app archives reference, and a raw ld.lld does
     * not auto-link it. Resolution order, cc-free where possible:
     *   1. HULL_LIBGCC override (explicit, or a hand-assembled floor).
     *   2. <libdir>/libgcc.a - an installed self-contained floor bundle
     *      (`hull tools install libc-musl-<arch>`) drops it beside crt/libc, so
     *      a bare box with no gcc still resolves it.
     *   3. glob the system gcc runtime dir - no compiler spawn.
     *   4. `cc -print-libgcc-file-name` - last resort, the only cc-needing path.
     * So Tier B is fully self-contained on any box that has the bundle OR a
     * system libgcc; cc is consulted only if neither is present. */
    {
        const char *env = getenv("HULL_LIBGCC");
        char cand[PATH_MAX];
        if (env && *env) {
            snprintf(ctx->libgcc, sizeof(ctx->libgcc), "%s", env);
        } else if (libdir && *libdir &&
                   snprintf(cand, sizeof(cand), "%s/libgcc.a", libdir) > 0 &&
                   access(cand, R_OK) == 0) {
            snprintf(ctx->libgcc, sizeof(ctx->libgcc), "%s", cand);
        } else {
            glob_t g;
            if (glob("/usr/lib/gcc/*/*/libgcc.a", 0, NULL, &g) == 0 &&
                g.gl_pathc > 0) {
                snprintf(ctx->libgcc, sizeof(ctx->libgcc), "%s", g.gl_pathv[0]);
            }
            globfree(&g);
            if (!ctx->libgcc[0]) {
                const char *q[] = { "cc", "-print-libgcc-file-name", NULL };
                char *lg = hl_tool_spawn_read(q, NULL);
                if (lg) {
                    char *nl = strchr(lg, '\n'); if (nl) *nl = '\0';
                    if (lg[0] && access(lg, R_OK) == 0)
                        snprintf(ctx->libgcc, sizeof(ctx->libgcc), "%s", lg);
                    free(lg);
                }
            }
        }
    }
    HlLinker *l = (HlLinker *)malloc(sizeof(HlLinker));
    if (!l) { free(ctx); return NULL; }
    l->vtable = &lld_vtable;
    l->ctx    = ctx;
    return l;
}
