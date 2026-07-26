# Runtime feature - Phase 2 design (runtime-less base, both runtimes as features)

Phase 2 of "Runtimes as composable features" (see
[roadmap_next.md](roadmap_next.md) "Runtimes as composable features
(runtime-less base + `lua` / `js`)"). Phase 1
([runtime_feature_phase1.md](runtime_feature_phase1.md)) made the factory
registry composable and decoupled base code from concrete runtime symbols, both
runtimes still compiled into the base.

**Goal:** reach the **end state** directly - a **runtime-less base** plus `lua`
and `js` as two composable feature archives, big-bang (both extracted in one
change series rather than the staged "JS-on-a-Lua-base" intermediate). A produced
app links the runtime-less base + exactly one runtime archive (auto-inferred from
the entry extension); the distributed `hull` toolchain embeds **both** archives so
`hull dev/test/build` runs and composes either runtime.

This supersedes the roadmap's staged Phase 2 (JS-only) / Phase 3 (Lua-only) split:
we go straight to both.

## End state, concretely

| Artifact | Runtimes linked | How |
|----------|-----------------|-----|
| `libhull_platform.a` (base) | **none** | `HL_ENABLE_LUA=0 HL_ENABLE_JS=0` for the platform lib; runtime-agnostic infra only |
| `libhull_feature-lua-<arch>.a` | Lua | Lua runtime objs + Lua vendored VM + Lua stdlib VFS entries + `hl_lua_factory` |
| `libhull_feature-js-<arch>.a` | JS | QuickJS runtime objs + QuickJS + JS stdlib VFS entries + `hl_js_factory` |
| `hull` toolchain binary | **both** | runtime-less base + force-load both archives + a generated toolchain registry (mirrors `HL_TUI_TOOLCHAIN`) |
| produced app (`hull build app.lua`) | Lua only | runtime-less base + `--with=lua` (auto) |
| produced app (`hull build app.js`) | JS only | runtime-less base + `--with=js` (auto) |
| cosmo `hull` (APE) | both | fat APE embeds both (no runtime slim on cosmo) |

The two invariants that define "done": a `hull build app.lua` binary exports **zero
QuickJS symbols**, and its embedded VFS holds **no `hull:*` JS module entries** (and
symmetrically for `app.js` vs Lua). See Tests.

## What stays in the base vs travels with a runtime

The vtable is already clean (Phase 1). The real entanglement is the stdlib VFS and
the per-runtime infra objects. Classification:

**Base (runtime-agnostic):**
- `runtime/factory.c` (registry + the two weak hooks), `app_context.c`, `serve.c`,
  `main.c`, `manifest.c` (the runtime-neutral half), `module_resolver.c`,
  `runtime_cache_common.o` (arch/endian tag, hex, blob_store singleton - shared).
- `agent/*.c` (already decoupled via `HlRuntimeKind` in Phase 1).
- The **runtime-agnostic** stdlib VFS entries: `context/*.md`, `static/**`,
  `templates/**` (`CONTEXT_FILES`, `STDLIB_STATIC_FILES`, `STDLIB_TPL_FILES`).

**Travels into `libhull_feature-<rt>.a`:**
- `runtime/<rt>/*.c` -> `LUA_RT_OBJS` / `JS_RT_OBJS`, the vendored VM
  (`LUA_OBJS` / `QJS_OBJS`), the per-runtime `manifest_<rt>.o`, the per-runtime
  bytecode/template caches.
- `hl_<rt>_factory` (`runtime/<rt>/factory.c`) and `hl_<rt>_vtable`
  (`runtime/<rt>/runtime.c`) - runtime-local, already the plan.
- That runtime's stdlib VFS entries: `STDLIB_LUA_FILES` (`hull.X`) with the Lua
  archive; `STDLIB_JS_FILES` (`hull:X`) with the JS archive.

## Change 1 - the stdlib/VFS composable seam (weak hook + merge-at-init)

Today `hl_stdlib_entries[]` (generated `stdlib_registry.c`, in `PLATFORM_OBJS`)
unifies five sources into one sorted array consumed by `app_context.c:230`,
`tool.c:197`, `agent/context.c:23`. Split it into a **base** array plus a
**feature** hook, mirroring `hl_runtime_feature_factories` exactly.

Base keeps the runtime-agnostic three; each runtime archive carries its own array.

New weak hook (add next to the factory hook, in `runtime/factory.c` or a small
`stdlib_feature.c`; always linked so the symbol resolves):

```c
/* runtime-agnostic base build: no runtime-owned stdlib composed */
__attribute__((weak))
const HlEntry *hl_stdlib_feature_entries(size_t *count)
{
    if (count) *count = 0;
    return NULL;
}
```

`hl_stdlib_entries[]` (base) now holds only `context/` + `static/` + `templates/`
(still NUL-sentinel-terminated for existing count logic). Each feature archive
exports a plain array symbol:

```c
const HlEntry hl_stdlib_lua_entries[] = { /* hull.X ... */ , {0,0,0} };  /* lua archive */
const HlEntry hl_stdlib_js_entries[]  = { /* hull:X ... */ , {0,0,0} };  /* js archive */
```

The generated `feature_registry.c` (app compose) and the generated toolchain
registry (Change 4) define the **strong** `hl_stdlib_feature_entries()` returning
the composed runtime's entries (concatenated when both are composed).

**Merge-at-init** (the chosen seam, no multi-segment VFS, no mutable dispatch
state): `hl_vfs_init` still takes one array, so introduce a builder that merges
base + feature into one freshly-allocated sorted array, once, at
`hl_app_context_init` / `tool.c` / `agent/context.c` setup:

```c
/* new: vfs_init_composed(vfs, base_entries, feat_entries, feat_count)
 * concatenates, LC_ALL=C-equivalent strcmp sort, calls hl_vfs_init.
 * Read-only after init; freed at context teardown. */
```

Because the VFS is built once per process and never mutated after, this respects
the sealed-table / CFI invariant (security.md 4b) - identical to how the factory
seam iterates two immutable sources. The merged array is owned by the context and
freed in its `_free`.

Caveat to handle: `hl_vfs_init` debug-asserts sorted order (`LC_ALL=C strcmp`), so
the merge must sort by the same total order the Makefile's `LC_ALL=C sort` uses.
Sort the concatenation with `strcmp` on `.name`; base and feature inputs are each
already sorted, so a merge (or a `qsort` for simplicity) suffices.

## Change 2 - the two runtime feature archives

New Makefile targets `feature-lua` / `feature-js`, mirroring `feature-tui`
(whole-archive, spread strong overrides - a runtime has no single anchor symbol so
the compose link must whole-archive / `-force_load` it):

```makefile
feature-lua: $(BUILDDIR)/libhull_feature-lua.a
$(BUILDDIR)/libhull_feature-lua.a: $(LUA_RT_OBJS) $(LUA_OBJS) $(BUILDDIR)/manifest_lua.o \
        $(LUA_CACHE_OBJS) $(STDLIB_LUA_REGISTRY_O) | $(BUILDDIR)
	@rm -f $@ ; $(AR) rcs $@ $^
	@echo "built $@ ($$(du -h $@ | cut -f1))"

feature-js: $(BUILDDIR)/libhull_feature-js.a
$(BUILDDIR)/libhull_feature-js.a: $(JS_RT_OBJS) $(QJS_OBJS) $(BUILDDIR)/manifest_js.o \
        $(JS_CACHE_OBJS) $(STDLIB_JS_REGISTRY_O) | $(BUILDDIR)
	@rm -f $@ ; $(AR) rcs $@ $^
	@echo "built $@ ($$(du -h $@ | cut -f1))"
```

Each archive bundles its runtime's stdlib as a **separate** generated registry
object (`STDLIB_LUA_REGISTRY_O` defining `hl_stdlib_lua_entries[]`,
`STDLIB_JS_REGISTRY_O` defining `hl_stdlib_js_entries[]`). Split the current
`stdlib_registry.c` codegen into three emitters:
- base: `hl_stdlib_entries[]` = context + static + templates (stays in base).
- lua: `hl_stdlib_lua_entries[]` = `STDLIB_LUA_FILES`.
- js: `hl_stdlib_js_entries[]` = `STDLIB_JS_FILES`.

Objects compiled with the runtime's `-DHL_ENABLE_<RT>` (like the tui feature objects
get `-DHL_ENABLE_TUI`). The config-sentinel clean-on-flag-flip already covers this.

## Change 3 - the runtime-less base (share the kernel via `HULL_CORE_OBJS`)

> **Superseded (2026-07-26): re-scoped as its own epic.** Recon during Change 2
> found this section under-scoped the problem. "Flip the base runtime-less" is
> not a flag flip: ~15 base TUs carry runtime-specific code (direct
> `lua_State` / `JSContext` calls) gated by `#ifdef HL_ENABLE_LUA/JS`. But the
> app **hot path** (`serve` / `app_context` / `main` / dispatch) is *already*
> vtable/factory-clean (verified: zero direct VM calls) from Phase 1, so **no
> vtable-method refactor is needed**. The entangled code splits by *who runs it*:
> (a) toolchain-only tooling (`agent/*` introspection bodies, the Lua tool VM,
> `hull manifest` / `doctor` / `version` runtime bits) - force-loaded into the
> `hull` binary, excluded from the produced-app link; (b) runtime-startup code
> the app needs (`manifest_lua.c` / `manifest_js.c` extractors) - travels into
> that runtime's archive; (c) runtime-agnostic base - stays. So the real Change 3
> is **object repartitioning** (base vs archive vs toolchain-only), plus
> relocating a couple of thin toolchain-only accessors (`hl_app_context_lua/js`,
> used only by agent) so a slim app never references an unlinked `hl_js_factory`.
> The `HULL_CORE_OBJS` DRY refactor below still applies. Full plan:
> `docs/runtime_feature_phase3.md` (to be written). The text below is the
> original, now-corrected sketch, kept for context.

Base platform lib default flips to **both runtimes off**: `HL_ENABLE_LUA ?= 0`,
`HL_ENABLE_JS ?= 0` for the platform-lib build. `PLATFORM_OBJS` drops `$(RT_OBJS)`
and `$(VEND_OBJS)` (they become archive-only), keeps `RUNTIME_CACHE_COMMON_OBJ`,
`RUNTIME_FACTORY_OBJ`, and the base `STDLIB_REGISTRY_O`. The base weak
`hl_runtime_feature_factories` and `hl_stdlib_feature_entries` both return empty, so
a bare base links but runs no app (exactly like the GPU base ships only the generic
dispatch layer, and the TUI-free base).

`manifest.o` stays base; `manifest_lua.o` / `manifest_js.o` move to their archives.
`cap_test_dispatch.o` (shared) stays base.

### DRY the kernel: `HULL_CORE_OBJS` (shared by libhull + the runtime-less base)

Today `LIBHULL_OBJS` (Makefile 2788, the no-framework embedder SDK) and
`PLATFORM_OBJS` (2432, the app-runner base) are **two independent long list
assemblies** over the same shared object variables (`$(CAP_OBJS)`, `$(VFS_OBJ)`,
`$(MODULE_OBJ)`, `$(SANDBOX_OBJ)`, `$(WAMR_OBJS)`, `$(MBEDTLS_OBJS)`,
`$(SQLITE_OBJ)`, `$(SBOM_OBJ)`, ...). The `.o` files are compiled once and both
lists point at them, so there is no duplicated *source* - but the overlapping list
assembly is a real maintenance smell (a new core object must be added to both).

Factor the runtime-free kernel both share into one named variable and derive both
artifacts from it (option (a) from the design discussion - share the kernel, keep
the two shells):

```makefile
# The runtime-free, framework-free kernel: cap layer, workers, manifest-core,
# module registry/resolver, sandbox, sig/release/sbom, vfs/cache/blob/tls/crypto
# plumbing, migrate, and the vendored libs. Compiled once; the single audit
# point for "what is in Hull's trust base regardless of shell".
HULL_CORE_OBJS := $(CAP_OBJS) $(ALLOC_OBJ) $(ASYNC_OBJ) $(COMPRESS_OBJ) $(MINIZ_OBJ) \
    $(WORKER_DB_OBJ) $(WORKER_WASM_OBJ) $(WORKER_GPU_OBJ) $(MANIFEST_CORE_OBJ) \
    $(MODULE_OBJ) $(ASYNC_BACKEND_OBJS) $(NET_BACKEND_OBJS) $(SANDBOX_OBJ) \
    $(SIG_OBJ) $(RELEASE_OBJ) $(RELEASE_IO_OBJ) $(TOOLS_INSTALL_OBJ) \
    $(PLATFORM_SIG_OBJ) $(EMBEDDED_PLATFORM_SIG_OBJ) $(MIGRATE_OBJ) \
    $(VFS_OBJ) $(PATH_NORM_OBJ) $(THREAD_AFFINITY_OBJ) $(CACHE_DIR_OBJ) \
    $(BLOB_STORE_OBJ) $(CACHE_REGISTRY_OBJ) $(CACERT_OBJ) $(TLS_CLIENT_OBJ) $(CSP_OBJ) \
    $(SH_SEAL_ARENA_OBJ) $(SBOM_OBJ) $(WAMR_OBJS) $(MBEDTLS_OBJS) \
    $(SQLITE_OBJ) $(LOG_OBJ) $(LOG_LOCK_OBJ) $(SH_ARENA_OBJ) $(SH_JSON_OBJ) \
    $(TWEETNACL_OBJ) $(STB_OBJ) $(PLEDGE_OBJS)

# Embedder SDK: kernel + the hl_embed_* ABI shim, host owns main().
LIBHULL_OBJS := $(EMBED_OBJ) $(HULL_CORE_OBJS)

# App-runner base: kernel + the framework (serve/app_context/factory/static/
# stdlib/agent/commands/main), runtime-less. A runtime archive composes back in.
FRAMEWORK_OBJS := $(CAP_TOOL_OBJ) $(CAP_TEST_OBJ) $(CMD_OBJS) $(SANDBOX_TOOL_OBJ) \
    $(TEST_RUNNER_OBJ) $(RUNTIME_FACTORY_OBJ) $(STATIC_OBJ) $(APP_CONTEXT_OBJ) \
    $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) $(MAIN_OBJ) $(SERVE_OBJ) $(TOOL_OBJ) \
    $(BUILD_ASSET_STUB_OBJ) $(STDLIB_REGISTRY_O) $(RUNTIME_CACHE_COMMON_OBJ)
PLATFORM_OBJS := $(HULL_CORE_OBJS) $(FRAMEWORK_OBJS)
```

The two artifacts stay distinct (different trust contracts: `libhull` exposes the
stable `hull/embed.h` ABI + host-owns-main, the base owns `main` and uses internal
headers), but the shared trust base is now expressed once. Landed as part of
Change 3 because that is when `PLATFORM_OBJS` is rewritten to drop the runtimes
anyway - the refactor and the runtime-drop are the same edit. `EMBED_OBJ` stays
libhull-only; the framework objects stay base-only; neither leaks into the other.

Reconcile the exact membership against the current `LIBHULL_OBJS` /
`PLATFORM_OBJS` lines when implementing (e.g. `MANIFEST_CORE_OBJ` vs the base's
`MANIFEST_OBJ`, and any object currently in exactly one list) so the split is
behavior-preserving object-for-object.

## Change 4 - the `hull` toolchain force-loads both

Mirror `HL_TUI_TOOLCHAIN` (Makefile 1101-1143, and the `hull` link line
2764-2766). Add `HL_LUA_TOOLCHAIN ?= 1` and `HL_JS_TOOLCHAIN ?= 1` (native; cosmo
compiles both in the fat archive as today). When set, the `hull` link:
- force-loads `libhull_feature-lua.a` and `libhull_feature-js.a`
  (`-Wl,-force_load,...` on macho / `--whole-archive` on GNU ld), and
- links a **generated toolchain registry** object that provides the strong
  `hl_runtime_feature_factories()` (returning `{ &hl_lua_factory, &hl_js_factory }`)
  and strong `hl_stdlib_feature_entries()` (returning lua ++ js entries).

The toolchain registry is the static analog of build.lua's per-app codegen; emit it
from a tiny Makefile rule (or a checked-in `runtime_toolchain_registry.c` guarded by
`HL_LUA_TOOLCHAIN`/`HL_JS_TOOLCHAIN`). This is why the base hooks are weak: the
toolchain's strong registry overrides them, the same way an app's generated
`feature_registry.c` does.

## Change 5 - build.lua: FEATURE_SPECS rows, compose, auto-inference

`FEATURE_SPECS` (build.lua ~1310) gains `lua` and `js` rows. They are
**whole-archive** features (like `tui`) but they also fill **two** hooks (factory +
stdlib entries), so the `by_hook` codegen must emit both `hl_runtime_feature_factories`
(over `HlRuntimeFactory` / `hl_<rt>_factory`) and `hl_stdlib_feature_entries` (over
`HlEntry[]` / `hl_stdlib_<rt>_entries`). Extend the codegen to support a
feature contributing to more than one hook, and to support an **array-symbol** hook
(entries) in addition to the existing **pointer-array-of-vtables** hook:

```lua
lua = { whole_archive = true, cxx = false, libs = { darwin = {}, other = {} },
        factory = "hl_lua_factory", stdlib_entries = "hl_stdlib_lua_entries" },
js  = { whole_archive = true, cxx = false, libs = { darwin = {}, other = {} },
        factory = "hl_js_factory",  stdlib_entries = "hl_stdlib_js_entries" },
```

**Auto-inference:** `hull build` infers the runtime from the entry extension
(`app.lua` -> `--with=lua`, `app.js` -> `--with=js`) - the single unambiguous
signal - and composes it with no `--with` needed. An explicit
`--with=lua`/`--with=js` still works (and both compose = dual-runtime app). An
`app.js` with only `lua` composed is a **build-time error** with a fix-it; auto
inference prevents it on the happy path. `--flavor=auto` composes on top (orthogonal
to the HTTP axis).

## Change 6 - `hull feature` registry rows

`FEATURES[]` (feature.c ~59) gains `lua` and `js` rows (published for all three
native arches). Note these are **embedded + auto-composed**, not
install-on-demand: `hull feature list` shows them as `embedded`, and there is no
expectation to `hull feature install lua`. (Same asymmetry the roadmap calls out:
a runtime is mandatory-exactly-one, so it is embedded in `hull`, unlike DuckDB.)
The rows exist so the archives are release-published + signed for source/custom
builds and for symmetry with the other features.

## Change 7 - app compose link path

`build.lua`'s link step already resolves + re-verifies feature archives and links
the platform lib. With a runtime-less base, the composed runtime archive must be
whole-archived (spread strong overrides, no anchor). The produced app = base
platform lib (runtime-less) + `--whole-archive libhull_feature-<rt>.a` + generated
`feature_registry.c` (strong hooks). Verify the flavor validation still holds:
runtime is orthogonal to HTTP flavor.

## Ordering within Phase 2 (each step compiles)

Big-bang end state, but staged so every commit builds the toolchain green:

1. **Change 1** (VFS seam) with both runtimes still base-resident: split the
   registry codegen into base + per-runtime arrays, wire `hl_stdlib_feature_entries`
   weak hook + merge-at-init, and have the *current* both-runtimes build feed the
   two per-runtime arrays through the hook via a temporary in-base strong registry.
   `make test` + e2e byte-identical. De-risks the VFS split alone.
2. **Changes 2 + 4** together: build the two feature archives; add the toolchain
   force-load + generated toolchain registry; keep base dual for now. The `hull`
   toolchain now links the runtimes *via* the archives, proving the archive +
   force-load + hook path. Still runs everything.
3. **Change 3**: flip the base default to runtime-less; base now depends on the
   toolchain force-load to run. `make` (default) still yields a both-runtime `hull`.
4. **Changes 5 + 6 + 7**: FEATURE_SPECS rows, dual-hook codegen, auto-inference,
   FEATURES[] rows, app compose whole-archive. Now `hull build app.lua` produces the
   slim.

## Tests / verification

- **nm slim invariant:** `hull build app.lua` -> `nm` shows zero `JS_*` / QuickJS
  symbols; `hull build app.js` -> zero `lua_*` / Lua VM symbols. Wire into
  `e2e_feature_runtime.sh`.
- **stdlib-travels:** a lua-only app's embedded VFS has no `hull:*` entry (and no
  JS stdlib bytes); symmetric for js-only. Assert via `hull inspect` / a VFS dump.
- **Toolchain still dual:** default `make` `hull` runs both `app.lua` and `app.js`
  (`hull dev/test`); `make test` (`test_lua` + `test_js`) unchanged.
- **Rejection:** `hull build app.js` with only `--with=lua` fails with the fix-it.
- **Merge-at-init:** unit test that `vfs_init_composed` produces a sorted array and
  finds both a base entry (`context/...`) and a feature entry (`hull.json` /
  `hull:json`); assert `hl_vfs_find` still O(log n) (sorted-order debug assert
  passes).
- **CFI / sealed:** the merged VFS array is written once at init, never after -
  same guarantee as Phase 1's two-immutable-sources factory loop.

## Non-goals for Phase 2

Cosmo runtime slims (the universal APE embeds both = full); a `hull feature install
lua` install-on-demand UX (runtimes are embedded + auto-composed); WASM/WAMR slim
(separate epic, not extension-inferable); a third runtime (the seam accommodates it,
out of scope).
