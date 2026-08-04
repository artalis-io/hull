/*
 * runtime/factory.c — Registry for HlRuntimeFactory
 *
 * Builds a static array of factories from the per-runtime externs
 * (one for each enabled runtime), and exposes lookup functions.
 *
 * Architectural roadmap item K (M3).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/factory.h"

#include <stddef.h>
#include <string.h>

/* The base runtime registry is intentionally EMPTY. A runtime-less base
 * resolves every runtime through the composable hl_runtime_feature_factories()
 * hook: filled by a produced app's generated feature registry (its one runtime)
 * or the hull toolchain registry (both). Keeping this array empty is what lets
 * the base reference NO concrete runtime symbol (hl_lua_factory /
 * hl_js_factory), so a slim single-runtime app links only its runtime and the
 * other interpreter dead-strips. The runtime factory descriptors travel with
 * their runtime (runtime/<rt>/factory.c), reached only via the hook. */
static const HlRuntimeFactory *const g_factories[] = { NULL };
static const size_t g_factory_count = 0;

const HlRuntimeFactory *const *hl_runtime_factories(size_t *count)
{
    if (count) *count = g_factory_count;
    return g_factories;
}

/*
 * hl_runtime_feature_factories() is the composable seam: a produced app links a
 * STRONG override (its generated feature registry) returning its runtime's
 * factory; the hull toolchain links the toolchain registry (both runtimes); a
 * unit-test binary that inits runtimes directly links the explicit empty default
 * in runtime/factory_none.c.
 *
 * There is deliberately NO weak default here. A weak default sitting in this
 * always-linked TU would be BOUND FIRST when this file is pulled from the base
 * archive, SHADOWING a strong override that lives in another archive member (the
 * archive-resolution rule: a member is only pulled to satisfy an UNDEFINED
 * symbol; a weak def already satisfies it). That is exactly the class of bug
 * that shipped an empty runtime VFS on the composition-exempt cosmo base (see
 * hl_stdlib_feature_entries in stdlib_feature.c). Leaving the symbol UNDEFINED in
 * the base turns "a link target forgot to provide the override" from a silent
 * runtime failure into a link-time "undefined reference" error. See
 * docs/runtime_feature_phase1.md.
 */

const HlRuntimeFactory *hl_runtime_factory_for_extension(const char *ext)
{
    if (!ext) return NULL;
    /* Tolerate "lua" or ".lua". The factory entry_extension uses ".lua". */
    if (ext[0] != '.') {
        /* Build a tiny temp buffer; the longest extension we accept is
         * something like ".wasm" (6 bytes incl. dot + NUL). */
        char buf[16];
        size_t n = strlen(ext);
        if (n + 1 >= sizeof(buf)) return NULL;
        buf[0] = '.';
        memcpy(buf + 1, ext, n + 1);
        return hl_runtime_factory_for_extension(buf);
    }

    /* Compile-time base factories first. */
    for (size_t i = 0; i < g_factory_count; i++) {
        const HlRuntimeFactory *f = g_factories[i];
        if (f && f->entry_extension && strcmp(f->entry_extension, ext) == 0)
            return f;
    }
    /* Then any runtime composed as a feature (empty in a base build; a
     * generated registry fills this under `--with=<runtime>`). Two immutable
     * sources, no merged/mutable dispatch state. */
    size_t fcount = 0;
    const HlRuntimeFactory *const *feats = hl_runtime_feature_factories(&fcount);
    for (size_t i = 0; i < fcount; i++) {
        const HlRuntimeFactory *f = feats ? feats[i] : NULL;
        if (f && f->entry_extension && strcmp(f->entry_extension, ext) == 0)
            return f;
    }
    return NULL;
}

const HlRuntimeFactory *hl_runtime_factory_for_filename(const char *filename)
{
    if (!filename) return NULL;
    const char *dot = strrchr(filename, '.');
    if (!dot) return NULL;
    return hl_runtime_factory_for_extension(dot);
}
