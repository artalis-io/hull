/*
 * linker_zig.c — `zig cc` linker backend (toolchain-free + cross-compile)
 *
 * zig bundles clang + lld + per-target crt/libc/headers for ~40 targets, so a
 * single `zig cc --target=<triple>` links for any (os, arch, abi) from any
 * host - no system compiler, no system linker, no crt/libc floor to source.
 * It collapses the whole toolchain-free + cross-compile problem into one tool
 * (docs/toolchain_free_build.md). Selected with `--linker=zig`; the target
 * triple (when cross-building) rides in on HlLinkTarget.triple.
 *
 * The compiler-free emitter is untouched: zig only LINKS the emitted
 * app_registry.o (+ the app_main / platform archives), it does not compile it.
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
    char zig[PATH_MAX];   /* the `zig` executable */
} ZigCtx;

static const char *zig_name(HlLinker *l) { (void)l; return "zig"; }

static int zig_is_available(HlLinker *l)
{
    ZigCtx *ctx = (ZigCtx *)l->ctx;
    if (!ctx->zig[0]) return 0;
    const char *argv[] = { ctx->zig, "version", NULL };
    char *out = hl_tool_spawn_read(argv, NULL);
    if (!out) return 0;
    free(out);
    return 1;
}

static int zig_link(HlLinker *l, const char *output,
                    const char **objs, const char **libs,
                    const HlLinkTarget *tgt)
{
    ZigCtx *ctx = (ZigCtx *)l->ctx;
    const char *argv[HL_LINKER_MAX_ARGS];
    char targ[128] = {0};
    int n = 0;
    argv[n++] = ctx->zig;
    argv[n++] = "cc";
    /* Cross target: `--target=<arch>-<os>-<abi>` (e.g. x86_64-linux-gnu). When
     * tgt/triple is NULL, zig builds for the native host. */
    if (tgt && tgt->triple && tgt->triple[0]) {
        snprintf(targ, sizeof targ, "--target=%s", tgt->triple);
        argv[n++] = targ;
    }
    /* Dead-strip unreferenced sections so an unused archive member (e.g.
     * tool.o's hull_tool, whose refs live in the tool-VM-only mod_tool.o) is
     * dropped rather than left dangling. Apple's ld64 / gnu ld do this by
     * default; zig's lld does not, so ask explicitly, per target format:
     * Mach-O uses -dead_strip, ELF/PE use --gc-sections. */
    if (tgt && tgt->format == HL_OBJ_MACHO)
        argv[n++] = "-Wl,-dead_strip";
    else
        argv[n++] = "-Wl,--gc-sections";
    argv[n++] = "-o"; argv[n++] = output;
    for (int i = 0; objs && objs[i] && n < HL_LINKER_MAX_ARGS - 2; i++)
        argv[n++] = objs[i];
    for (int i = 0; libs && libs[i] && n < HL_LINKER_MAX_ARGS - 2; i++)
        argv[n++] = libs[i];
    argv[n] = NULL;

    /* Point zig's cache at the build tmpdir (under /tmp, which the tool-mode
     * sandbox unveils rwcx) so a SANDBOXED link doesn't fail AccessDenied
     * writing zig's default ~/.cache/zig / ./.zig-cache. A cross-musl link
     * compiles zig's bundled crt/libc glue, so it DOES write a cache (unlike a
     * native-glibc link, which is why --linker=zig worked before this). The
     * build's objects live in that tmpdir, so derive it from objs[0]
     * (<tmpdir>/app_main.o); fall back to /tmp. Mirrors the compile-side
     * redirect in build.lua. (#4c) */
    char dbuf[PATH_MAX];
    const char *base = "/tmp";
    if (objs && objs[0]) {
        snprintf(dbuf, sizeof dbuf, "%s", objs[0]);
        char *slash = strrchr(dbuf, '/');
        if (slash) { *slash = '\0'; base = dbuf; }
    }
    char gcache[PATH_MAX + 64], lcache[PATH_MAX + 64];
    snprintf(gcache, sizeof gcache, "ZIG_GLOBAL_CACHE_DIR=%s/zig-cache", base);
    snprintf(lcache, sizeof lcache, "ZIG_LOCAL_CACHE_DIR=%s/zig-cache", base);
    const char *envadd[] = { gcache, lcache, NULL };
    return hl_tool_spawn_env(argv, envadd) == 0 ? 0 : -1;
}

static void zig_destroy(HlLinker *l)
{
    free(l->ctx);
    /* caller frees l itself via hl_linker_destroy macro */
}

static const HlLinkerVtable zig_vtable = {
    zig_name, zig_is_available, zig_link, zig_destroy
};

/*
 * Create the zig backend wrapping zig_path (the `zig` executable). zig_path
 * may be NULL, yielding an unavailable backend. See docs/toolchain_free_build.md.
 */
HlLinker *hl_linker_zig_new(const char *zig_path)
{
    ZigCtx *ctx = (ZigCtx *)calloc(1, sizeof(ZigCtx));
    if (!ctx) return NULL;
    if (zig_path) snprintf(ctx->zig, sizeof(ctx->zig), "%s", zig_path);
    HlLinker *l = (HlLinker *)malloc(sizeof(HlLinker));
    if (!l) { free(ctx); return NULL; }
    l->vtable = &zig_vtable;
    l->ctx    = ctx;
    return l;
}
