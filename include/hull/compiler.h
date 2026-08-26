/*
 * compiler.h - C compiler vtable for hull build
 *
 * Abstracts compile (.c → .o) and link (objs+libs → binary) so the
 * system cc backend + the compiler-free emit path drive hull build.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMPILER_H
#define HL_COMPILER_H

#include <stdlib.h>

typedef struct HlCompiler HlCompiler;

typedef struct {
    /* Short name: "cc", "gcc", "clang", etc. Never NULL, never freed. */
    const char *(*name)(HlCompiler *c);
    /* 1 if compiler is usable, 0 otherwise. */
    int         (*is_available)(HlCompiler *c);
    /* malloc'd first line of version output. May be NULL. Caller frees. */
    char       *(*version)(HlCompiler *c);
    /*
     * Compile src (.c) to obj (.o). Returns 0 on success.
     * include_dir: if non-NULL, adds -I include_dir to invocation.
     */
    int (*compile)(HlCompiler *c, const char *src, const char *obj,
                   const char *include_dir);
    /*
     * Free compiler resources. (Linking is NOT a compiler responsibility -
     * it goes through HlLinkerVtable / hl_linker_link, so a single code path
     * drives the `cc -o out objs libs` invocation for both the compile and the
     * emit build paths. See linker.h.)
     */
    void (*destroy)(HlCompiler *c);
} HlCompilerVtable;

struct HlCompiler {
    const HlCompilerVtable *vtable;
    void                   *ctx;
};

/* Convenience inlines */
static inline const char *hl_compiler_name(HlCompiler *c)
    { return c->vtable->name(c); }
static inline int hl_compiler_is_available(HlCompiler *c)
    { return c->vtable->is_available(c); }
static inline char *hl_compiler_version(HlCompiler *c)
    { return c->vtable->version(c); }
static inline int hl_compiler_compile(HlCompiler *c,
    const char *src, const char *obj, const char *inc)
    { return c->vtable->compile(c, src, obj, inc); }
static inline void hl_compiler_destroy(HlCompiler *c)
    { if (c) { c->vtable->destroy(c); free(c); } }

/*
 * Create a system compiler backend wrapping cc_path.
 * cc_path must not be NULL. Returns NULL on allocation failure.
 */
HlCompiler *hl_compiler_system_new(const char *cc_path);

/*
 * Auto-select a compiler (the emit path - obj_emit - is the default build
 * path now; this feeds the --compiler / --with / cosmo fallback):
 *   "system" → find the first system compiler in PATH
 *   other    → hl_compiler_system_new(explicit_cc) (a path or PATH name)
 *   NULL     → cc/gcc/clang from PATH (cosmocc on a cosmo hull)
 * Returns NULL if nothing is found.
 */
HlCompiler *hl_compiler_select(const char *explicit_cc);

/*
 * Resolve a native toolchain driver (the cc/gcc/clang - or cosmocc on a cosmo
 * hull - that acts as the compile AND default-link driver). Writes the resolved
 * invocation (a PATH name or absolute path) into out and returns 0; returns -1
 * if none is available. Availability is probed via `<cand> --version`.
 *
 * Shared by hl_compiler_select and the linker's hl_linker_select so the
 * candidate list + cosmo resolution live in exactly one place.
 */
int hl_driver_resolve_native(char *out, size_t outsz);

#endif /* HL_COMPILER_H */
