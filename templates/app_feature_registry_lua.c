/*
 * app_feature_registry_lua.c - bundled per-runtime feature registry for the
 * compiler-free `hull build` (docs/compiler_free_build.md).
 *
 * INVARIANT per runtime: this is the exact C that
 * feature_compose.gen_app_registry_c("lua") generates. It fills the base's
 * weak hl_stdlib_feature_entries() / hl_runtime_feature_factories() seams
 * with strong overrides pointing at the composed libhull_feature-lua.a. It
 * depends only on the runtime, never on the app, so it is pre-compiled at
 * Hull release time and bundled, one blob per (rt, format, arch). Keep in
 * sync with gen_app_registry_c (tests/e2e_compiler_free.sh asserts a match).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
typedef __SIZE_TYPE__ size_t;
typedef struct { const char *n; const unsigned char *d; unsigned int l; } HlEntry;
extern const HlEntry hl_stdlib_lua_entries[];
static const HlEntry *const HL_STDLIB_FEATS[] = { hl_stdlib_lua_entries };
const HlEntry *const *hl_stdlib_feature_entries(size_t *count) {
    if (count) *count = 1;
    return HL_STDLIB_FEATS;
}
typedef struct HlRuntimeFactory HlRuntimeFactory;
extern const HlRuntimeFactory hl_lua_factory;
static const HlRuntimeFactory *const HL_RT_FEATS[] = { &hl_lua_factory };
const HlRuntimeFactory *const *hl_runtime_feature_factories(size_t *count) {
    if (count) *count = 1;
    return HL_RT_FEATS;
}
