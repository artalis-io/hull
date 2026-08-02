/*
 * linker.h — linker vtable for the compiler-free `hull build`
 *
 * The object emitter (obj_emit.h) removes the need to COMPILE the app's
 * data object; the link step remains. This vtable abstracts it so the
 * default system driver (cc/ld) and later embedded linkers (lld/mold, for
 * a fully toolchain-free box) are interchangeable. It mirrors the link
 * half of HlCompilerVtable but is a separate, pluggable component: the
 * linker choice is orthogonal to the emitter (the emitter's output is a
 * standard relocatable object any linker consumes).
 *
 * See docs/compiler_free_build.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LINKER_H
#define HL_LINKER_H

#include <stdlib.h>

#include "hull/obj_emit.h"   /* HlObjFormat */

typedef struct HlLinker HlLinker;

/* What we are linking, so a cross/embedded linker knows how to invoke.
 * The system-cc + lld backends infer format/arch from the objects and ignore
 * this entirely; only zig reads it (format → the GC flag, triple → --target=).
 * The emitted objects already carry the arch, so no `arch` field is needed. */
typedef struct {
    HlObjFormat format;
    /* Full cross triple <arch>-<os>-<abi> (e.g. x86_64-linux-gnu). NULL/empty
     * = native host. Consumed by the zig backend's `--target=`; other backends
     * ignore it. Not owned. */
    const char *triple;
} HlLinkTarget;

typedef struct {
    /* Short name: "cc", "ld", "lld", "mold". Never NULL, never freed. */
    const char *(*name)(HlLinker *l);
    /* 1 if the linker is usable, 0 otherwise. */
    int         (*is_available)(HlLinker *l);
    /*
     * Link objects + libraries → output binary. Returns 0 on success.
     * objs / libs: NULL-terminated arrays (.o paths / -lfoo or lib.a paths).
     * tgt: may be NULL (native host default).
     */
    int (*link)(HlLinker *l, const char *output,
                const char **objs, const char **libs,
                const HlLinkTarget *tgt);
    void (*destroy)(HlLinker *l);
} HlLinkerVtable;

struct HlLinker {
    const HlLinkerVtable *vtable;
    void                 *ctx;
};

static inline const char *hl_linker_name(HlLinker *l)
    { return l->vtable->name(l); }
static inline int hl_linker_is_available(HlLinker *l)
    { return l->vtable->is_available(l); }
static inline int hl_linker_link(HlLinker *l, const char *out,
    const char **objs, const char **libs, const HlLinkTarget *tgt)
    { return l->vtable->link(l, out, objs, libs, tgt); }
static inline void hl_linker_destroy(HlLinker *l)
    { if (l) { l->vtable->destroy(l); free(l); } }

/*
 * Create the system linker backend, invoking cc_path as the link driver
 * (cc/gcc/clang resolves crt startup + libc). cc_path must not be NULL.
 * Returns NULL on allocation failure.
 */
HlLinker *hl_linker_system_new(const char *cc_path);

/*
 * Create the lld backend (Tier A: drives cc_path with `-B<dir(lld_bin)>
 * -fuse-ld=lld`). cc_path is the driver; lld_bin is a resolved
 * ld.lld / ld64.lld. Either may be NULL (yields an unavailable backend).
 * See docs/toolchain_free_build.md.
 */
HlLinker *hl_linker_lld_new(const char *cc_path, const char *lld_bin);

/*
 * Create the Tier B (direct static) lld backend: invokes ld_path (ld.lld)
 * directly with the static musl floor in libdir (crt1.o / crti.o / crtn.o /
 * libc.a) - no C compiler in the link. Either arg NULL yields an unavailable
 * backend. Requires musl-built app archives. See docs/toolchain_free_build.md.
 */
HlLinker *hl_linker_lld_direct_new(const char *ld_path, const char *libdir);

/*
 * Create the zig backend wrapping zig_path (the `zig` executable), which links
 * via `zig cc [--target=<triple>]` - toolchain-free (zig bundles clang + lld +
 * crt/libc) and cross-capable. zig_path may be NULL (unavailable backend).
 * See docs/toolchain_free_build.md.
 */
HlLinker *hl_linker_zig_new(const char *zig_path);

/*
 * Auto-select a linker:
 *   "system" / NULL → first cc/gcc/clang found in $PATH
 *   other           → hl_linker_system_new(explicit) (a cc/ld path)
 * hull_exe (may be NULL) is reserved for future embedded-linker resolution.
 * Returns NULL if nothing is found.
 */
HlLinker *hl_linker_select(const char *explicit_linker, const char *hull_exe);

#endif /* HL_LINKER_H */
