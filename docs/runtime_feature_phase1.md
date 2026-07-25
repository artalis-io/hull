# Runtime feature - Phase 1 design

Phase 1 of "Runtimes as composable features" (see
[roadmap_next.md](roadmap_next.md) "Runtimes as composable features
(runtime-less base + `lua` / `js`)").

**Goal:** make the runtime factory registry composable (weak hook + collector)
and decouple base code from concrete runtime symbols, **with zero behavior
change**. Both runtimes stay compiled into the base; the weak default returns
empty; selection stays byte-identical. This de-risks the seam before any runtime
object moves into an archive (Phase 2).

The seam already exists: `src/hull/runtime/factory.c` (architectural roadmap item
K) is a runtime factory registry, and `app_context.c` already selects the runtime
table-drivenly through it. Phase 1 makes that registry composable, mirroring
`hl_db_feature_backends` / `hl_gpu_feature_backends` exactly.

## Change 1 - the weak hook

New file `src/hull/runtime/factory_feature.c` (a direct analog of
`cap/gpu_feature.c`; always compiled so the symbol always resolves):

```c
#include "hull/runtime/factory.h"

__attribute__((weak))
const HlRuntimeFactory *const *hl_runtime_feature_factories(size_t *count)
{
    if (count) *count = 0;   /* base build: no runtime composed as a feature */
    return NULL;
}
```

Declaration added to `include/hull/runtime/factory.h`:

```c
const HlRuntimeFactory *const *hl_runtime_feature_factories(size_t *count);
```

Makefile: add `$(BUILDDIR)/runtime_factory_feature.o` next to
`RUNTIME_FACTORY_OBJ` in `PLATFORM_OBJS` and the `hull` link line. A composed
`--with=js` build supplies a **strong** override via the generated
`feature_registry.c` (Phase 2; `build.lua`'s existing `by_hook` codegen - add
`lua`/`js` rows to `FEATURE_SPECS` with `hook = "hl_runtime_feature_factories"`,
symbol `hl_<rt>_factory`, vtype `HlRuntimeFactory`).

## Change 2 - `factory.c` becomes base union features (two immutable sources)

`src/hull/runtime/factory.c` keeps `g_factories[]` (compile-time base) and
consults the hook. Both the **selection** primitive and the **discover-entry**
enumerate must see features. Iterate two immutable sources rather than caching a
merged array, to keep **no mutable dispatch state** (the sealed-table / CFI
invariant, security.md 4b):

```c
const HlRuntimeFactory *hl_runtime_factory_for_extension(const char *ext) {
    /* normalize "lua" -> ".lua" (unchanged) ... */
    for (size_t i = 0; i < g_factory_count; i++)                 /* base */
        if (matches(g_factories[i], ext)) return g_factories[i];
    size_t fc = 0;                                               /* features */
    const HlRuntimeFactory *const *f = hl_runtime_feature_factories(&fc);
    for (size_t i = 0; i < fc; i++)
        if (f && matches(f[i], ext)) return f[i];
    return NULL;
}
```

`hl_runtime_factory_for_filename()` is unchanged (delegates to
`_for_extension`). The one enumerate caller - `app_context.c`'s discover-entry
loop (the "try each registered factory's extension" pass) - iterates base then
features directly:

```c
/* base factories */                          /* then feature factories */
try(g_factories, g_factory_count, app_dir);   try(hl_runtime_feature_factories(&fc), fc, app_dir);
```

No merged array, so no writable dispatch state. `hl_runtime_factories()` stays
as-is (returns the base array) for any caller that only wants the compiled-in
set.

**Phase 1 behavior:** the weak default returns 0 features, so both loops see
exactly `{lua, js}` - identical to today.

## Change 3 - `HlRuntimeKind` discriminator (the real blocker)

`agent/*.c` is in `PLATFORM_OBJS` (the app-facing platform lib) and compares
`rt->vt == &hl_<rt>_vtable` in 17 sites. Each takes the address of a concrete
runtime symbol, so a runtime-slim base that does not link `hl_js_vtable` fails to
link (or survives only on fragile dead-object stripping). Decouple with a
discriminator carried in the vtable.

Add to `include/hull/runtime.h`:

```c
typedef enum { HL_RT_LUA = 1, HL_RT_JS = 2 } HlRuntimeKind;
```

Add one field to `HlRuntimeVtable` (after `name`):

```c
HlRuntimeKind kind;
```

Set it in each runtime's vtable literal (these live with the runtime, so they
move to the feature archive later - fine):

- `runtime/lua/runtime.c`: `hl_lua_vtable = { ..., .name = "lua", .kind = HL_RT_LUA, ... }`
- `runtime/js/runtime.c`:  `hl_js_vtable  = { ..., .name = "js",  .kind = HL_RT_JS,  ... }`

Then mechanically rewrite the 17 base sites
`rt->vt == &hl_<rt>_vtable` -> `rt->vt->kind == HL_RT_<RT>`:

| File | Lines | Sites |
|------|-------|-------|
| `agent/perf.c` | 39, 42 | 2 |
| `agent/eval.c` | 185, 192 | 2 |
| `agent/modules.c` | 94, 98 | 2 |
| `agent/routes.c` | 66, 67 | 2 |
| `agent/test.c` | 105, 106 | 2 |
| `agent/overview.c` | 257, 263 | 2 |
| `agent/manifest.c` | 185, 191 | 2 |
| `agent/template.c` | 175, 182 | 2 |
| `agent/capabilities.c` | 192 | 1 |

After this, no `PLATFORM_OBJS` non-runtime TU references a concrete runtime
symbol - the precondition for a slim link. Deliberately unchanged:

- `runtime/{lua,js}/factory.c`'s `&hl_<rt>_vtable` (the factory descriptor +
  `base.vt` assignment) are runtime-local: a runtime referencing its own vtable.
  They travel into the feature archive with the runtime.
- `runtime/{lua,js}/runtime.c` hold the vtable definitions themselves.
- `app_context.c` (a comment mentioning the comparisons) - update the comment to
  mention `kind`; no code there references the symbols.

## Ordering within Phase 1

Change 3 is independent and can land first (pure substitution, immediately
testable). Changes 1 + 2 land together. No dependency between them.

## Tests / verification (proves "no behavior change")

- **Unit** (extend `test_*` for the factory / runtime): selection of `.lua` /
  `.js` still returns the right factory through the base-union-feature path with
  an empty feature set; assert `hl_lua_vtable.kind == HL_RT_LUA` and
  `hl_js_vtable.kind == HL_RT_JS`.
- **Invariant assertion** (the Change-3 guarantee): a
  `grep -rn '== &hl_\(lua\|js\)_vtable' src/hull/agent/` returns nothing. Wire it
  as a tiny CI / test check so a regression cannot creep back.
- **Byte-identical behavior:** `make test` + full `make e2e` green unchanged;
  `test_lua` / `test_js` unchanged.
- **Optional:** `nm build/hull | grep hl_.*_vtable` is identical before/after
  (both runtimes stay base-resident in Phase 1).

## Explicit non-goals for Phase 1

No archive extraction, no stdlib-into-feature move, no `HL_ENABLE_JS=0` base, no
`--with` inference, no release / publish changes. Those are Phases 2-4. Phase 1
is purely: hook + collector + `kind` decoupling, both runtimes still in the base.
