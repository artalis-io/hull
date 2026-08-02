/*
 * compiler.h — C compiler vtable for hull build
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
     * Link objects + libraries → output binary. Returns 0 on success.
     * objs: NULL-terminated array of .o paths.
     * libs: NULL-terminated array of -lfoo flags or /path/to/lib.a paths.
     */
    int (*link)(HlCompiler *c, const char *output,
                const char **objs, const char **libs);
    /* Free compiler resources. */
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
static inline int hl_compiler_link(HlCompiler *c,
    const char *out, const char **objs, const char **libs)
    { return c->vtable->link(c, out, objs, libs); }
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
 * hull_exe (may be NULL) is currently unused (was the retired tcc tool's
 * resolution seed); kept for call-site stability.
 * Returns NULL if nothing is found.
 */
HlCompiler *hl_compiler_select(const char *explicit_cc, const char *hull_exe);

#endif /* HL_COMPILER_H */
