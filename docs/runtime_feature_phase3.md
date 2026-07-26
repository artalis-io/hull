# Runtime feature - Phase 3 epic: the runtime-less base (object repartition)

The end-state step of "runtimes as composable features": move the Lua and JS
runtimes (VM + runtime objects + their stdlib + their manifest extractor) into
signed feature archives `libhull_feature-lua.a` / `libhull_feature-js.a`, make
the base platform lib **runtime-agnostic**, and force-load both archives into the
distributed `hull` toolchain. A produced `hull build app.lua` then links the
runtime-agnostic base + only the `lua` archive and exports **zero QuickJS
symbols**.

Prereqs (shipped): Phase 1 (factory registry + `HlRuntimeKind` vtable
discriminator), Phase 2 Changes 1+2 (the stdlib-VFS composable seam +
`hl_stdlib_{lua,js}_entries[]` split + the toolchain registry filling the strong
`hl_stdlib_feature_entries()` hook). See
[runtime_feature_phase2.md](runtime_feature_phase2.md).

## The corrected framing: repartition, not a vtable refactor

An earlier sketch (phase2 doc, Change 3) assumed the runtime-entangled base code
had to be decoupled with new vtable methods. Recon disproved that:

- **The app hot path is already vtable/factory-clean.** `serve.c`, `main.c`,
  `app_context.c` contain **zero** direct concrete-VM calls (`lua_State` /
  `JS_Eval` / `luaL_*`); all runtime interaction goes through the factory +
  `HlRuntimeVtable` from Phase 1. Verified by grep.
- **`build.lua` links `libhull_platform.a` as a plain archive** (not
  `--whole-archive`), so a produced app **dead-strips** every platform-lib member
  it does not reference. Agent/command/tool objects a running app never calls are
  already dropped.

So the slim is **not** blocked by the *presence* of runtime-entangled code in the
archive - it is blocked only where a **referenced** base object names a
**non-composed** runtime's symbol. Concretely: `app_context.o` is pulled (the app
calls `hl_app_context_init`), and it defines `hl_app_context_js()` which
references `extern hl_js_factory`; in a lua-only app that symbol is undefined ->
link error. That is a *relocation* problem, not a vtable problem.

The work is therefore **object repartitioning** into three buckets by *who runs
the code*, plus two small code relocations. No new vtable methods.

## The three buckets

| Bucket | Compiled with | Linked into | Contents |
|--------|---------------|-------------|----------|
| **archive** `libhull_feature-<rt>.a` | `-DHL_ENABLE_<RT>` (that runtime only) | the app that composes it, and `hull` (force-load) | `runtime/<rt>/*.o` (incl. `<rt>_factory.o`, dispatch, bytecode/template caches, worker_db, `mod_*`), vendored VM (`LUA_OBJS`/`QJS_OBJS`), `manifest_<rt>.o` (the app-startup manifest extractor), `stdlib_<rt>_registry.o` (`hl_stdlib_<rt>_entries[]`) |
| **toolchain-only** | `-DHL_ENABLE_LUA -DHL_ENABLE_JS` (both) | **only** `hull` (force-loaded; **not** in `libhull_platform.a`) | `agent/*.o` introspection bodies, the Lua tool VM (`tool.o`, `tool_orchestration.o`), `manifest_extract_file.o` (`hull manifest`), the CLI dispatch + `cmd_*.o`, `commands/test.o`, and the relocated `hl_app_context_lua/js` accessors |
| **base / agnostic** | neither runtime macro | `libhull_platform.a` (app **and** `hull`) | `HULL_CORE_OBJS`, `serve.o`, `main.o` (lib side), `app_context.o` (minus accessors), the request dispatch, `runtime/factory.o` (empty `g_factories[]` + weak hooks), `stdlib_feature.o`, `static.o`, `migrate.o` |

Key property: **no source is compiled twice.** Base objects compile once with no
runtime macro (used by app + toolchain); archive objects once with their one
macro; toolchain-only objects once with both macros (linked only into `hull`,
where both archives resolve their concrete-VM references).

### Why the buckets are correct

- **A deployed app runs its runtime, never `hull agent`.** `hull agent` /
  `hull manifest` / `hull test` / `hull doctor` are toolchain subcommands invoked
  against an app dir or dev server; they are not compiled into `./myapp`. So the
  agent introspection bodies (the bulk of the VM entanglement) are toolchain-only.
- **The manifest extractor is different** - a running app calls
  `rt->vt->extract_manifest` at startup (that is `manifest_<rt>.o`), so it must
  ship in the runtime's archive, not be toolchain-only.
- **The factory registry stays in the base** compiled macro-less, so `g_factories[]`
  is empty and runtimes are resolved *only* through `hl_runtime_feature_factories()`
  - filled by the composed app's generated registry, or the toolchain registry.

## Change 3a - relocate the runtime accessors (DONE, still dual base)

Land while the base is still dual so it is behavior-preserving and independently
testable.

**Relocate the runtime accessors.** Moved `hl_app_context_is_lua()` /
`hl_app_context_lua()` / `hl_app_context_js()` (and the `HlLua`/`HlJS`
forward-decls they need) out of `app_context.c` into a new **toolchain-only** TU
`app_context_runtime.c`, compiled with both macros. `app_context.c` now exposes
only an agnostic `hl_app_context_factory()` getter the accessors build on (so the
new TU never needs the opaque `HlAppContext` layout). Only `agent/*` (toolchain)
calls the accessors. After this, `app_context.o` references **no** concrete
runtime symbol (`nm` confirms: it exports only `hl_app_context_factory`; the three
accessors live in `app_context_runtime.o`). `make test` 60/60 green,
`hull agent overview`/`eval` work.

`app_context_runtime.o` is wired beside `app_context.o` in the link lists for
now; Change 3b moves it into `TOOLCHAIN_ONLY_OBJS` (out of the produced-app lib).

**`serve.c` entry auto-detection moved to Change 3b.** `auto_detect_entry()`
`#ifdef`s the `"app.js"` / `"app.lua"` probes on `HL_ENABLE_JS/LUA` and is
entangled with the runtime-validation block (`#ifndef HL_ENABLE_JS/LUA` at
serve.c ~807) that rejects an uncompiled runtime. It also probes **JS-first**
while `app_context.c::resolve_entry_and_runtime` discovers **Lua-first** - a
pre-existing order inconsistency in the both-entries-present case. So a
"byte-identical" factory-driven rewrite is not clean in isolation; do it in 3b
together with the validation rework, against the actual macro-less base.

## Change 3b - `HULL_CORE_OBJS` + the bucket split in the Makefile

Introduce `HULL_CORE_OBJS` (the runtime-free kernel shared with `libhull.a`; see
phase2 doc Change 3) and derive:

```makefile
LIBHULL_OBJS   := $(EMBED_OBJ) $(HULL_CORE_OBJS)                  # embedder SDK
FRAMEWORK_OBJS := $(SERVE_OBJ) $(APP_CONTEXT_OBJ) $(STATIC_OBJ) \ # agnostic app-runner
                  $(MIGRATE_OBJ) $(RUNTIME_FACTORY_OBJ) $(STDLIB_FEATURE_OBJ) \
                  $(STDLIB_REGISTRY_O) $(RUNTIME_CACHE_COMMON_OBJ) ...
PLATFORM_OBJS  := $(HULL_CORE_OBJS) $(FRAMEWORK_OBJS)             # runtime-less base
TOOLCHAIN_ONLY_OBJS := $(CMD_OBJS) $(AGENT_LIB_OBJ) $(AGENT_API_OBJ) \
                  $(TOOL_OBJ) $(TOOL_ORCH_OBJ) $(APP_CONTEXT_RT_OBJ) \
                  $(MANIFEST_EXTRACT_FILE_OBJ) $(CAP_TEST_OBJ) ...  # compiled w/ both macros
```

Base default flips to **both runtimes off** for the platform-lib compile
(`HL_ENABLE_LUA ?= 0`, `HL_ENABLE_JS ?= 0`); `PLATFORM_OBJS` no longer contains
`RT_OBJS` / `VEND_OBJS` / `manifest_<rt>` / `stdlib_<rt>_registry` /
`stdlib_toolchain_registry`. Those move to the archives and the toolchain
registry respectively.

## Change 3c - the archives (DONE, additive, base still dual)

`make feature-lua` / `feature-js`, mirroring `feature-tui` (whole-archive; a
runtime has no single anchor symbol so the compose must whole-archive /
`-force_load` it). Built additively while the base is still dual: the objects
already exist from the normal build, so `make feature-lua feature-js` just `ar`s
them; the base build and `make test` are unaffected (verified: 60/60, default
build unchanged). The tui bridge (`lua_rt_mod_tui.o` / `js_mod_tui.o`) is
`filter-out`-excluded - it belongs to `libhull_feature-tui.a`.

`nm` verification (the archive-boundary proof before the base flip):
- lua archive (1.8M) defines `hl_lua_factory`, `hl_lua_vtable`,
  `hl_stdlib_lua_entries`, `hl_manifest_extract_lua`, 697 Lua-VM symbols, and
  **0** JS / QuickJS symbols.
- js archive (2.2M) defines the symmetric set + 172 QuickJS symbols and **0** Lua
  symbols.
- **Neither** archive defines `hl_stdlib_feature_entries` /
  `hl_runtime_feature_factories` - those stay weak-in-base, filled by the
  generated toolchain/app registry (3d / 3e).

The as-built rule (via `FEATURE_LUA_OBJS` / `FEATURE_JS_OBJS`):

```makefile
$(BUILDDIR)/libhull_feature-lua.a: $(LUA_RT_OBJS) $(LUA_OBJS) $(BUILDDIR)/manifest_lua.o \
        $(STDLIB_LUA_REGISTRY_O) | $(BUILDDIR)
	@rm -f $@ ; $(AR) rcs $@ $^
$(BUILDDIR)/libhull_feature-js.a:  $(JS_RT_OBJS)  $(QJS_OBJS) $(BUILDDIR)/manifest_js.o \
        $(STDLIB_JS_REGISTRY_O)  | $(BUILDDIR)
	@rm -f $@ ; $(AR) rcs $@ $^
```

Each runtime object compiled with `-DHL_ENABLE_<RT>` (per-object CFLAGS, like the
tui feature objects get `-DHL_ENABLE_TUI`).

## As shipped: the slim without force-load or a physical partition (DONE)

The realized 3b/3d/3e is **simpler** than the force-load design below. Because
`build.lua` links `libhull_platform.a` as a plain archive, the produced app
**dead-strips** any member it does not reference. So the slim needed only:

1. **Empty `g_factories`** + hook-driven resolution (committed): the base
   references no concrete runtime symbol; the app's generated registry names its
   one factory + stdlib; `hull`'s generated registry names both.
2. **Weak-DEFINITION stubs** for the handful of toolchain-only symbols that
   *app-linked base objects* reference but never reach at runtime, placed in
   **`app_runner.o`** (which the `hull` toolchain does not link -- it uses
   `hull_main`, not `hl_app_run`). The three edges found by `nm`:
   `hl_lua_tool_register` + `hl_lua_tool_register_orchestration`
   (`lua_rt_runtime.c`'s tool-mode branch -> `mod_tool.o` -> the JS manifest
   extractor -> QuickJS) and `hl_agent_api_register` (`serve.c` under `--agent`
   -> `agent_api.o` -> the agent bodies -> both VMs). With the app resolving
   these to no-ops, `mod_tool.o` / `agent_api.o` are never pulled, so the tool
   VM, agent, and **the other interpreter** all dead-strip. `hull` links the
   real strong definitions directly (weak defs are portable, unlike the weak
   *references* that Mach-O does not honor -- verified).

**Result (measured):** a `hull build app.lua` binary has **0** QuickJS symbols
(4.9 MB -> 4.3 MB); a `hull build app.js` binary has **0** Lua-VM symbols; both
serve 200; the `hull` toolchain runs both runtimes; `make test` 60/60.

No `HL_LUA/JS_TOOLCHAIN` force-load, no `TOOLCHAIN_ONLY_OBJS` object move, and no
CFLAGS refactor were needed -- the archive dead-strip + weak stubs subsume them.
The `feature-lua/js` archives (3c) remain for the published-feature / cosmo /
custom path. If a future app-linked base object gains a new reference into
toolchain-only code, add a matching weak stub in `app_runner.c`.

The original force-load design is kept below for context.

## Change 3d - the `hull` toolchain force-loads both

Mirror `HL_TUI_TOOLCHAIN` (Makefile ~1101-1143 + the `hull` link line): add
`HL_LUA_TOOLCHAIN ?= 1` / `HL_JS_TOOLCHAIN ?= 1` (native; cosmo compiles both in
the fat archive as today). When set, the `hull` link:

- `-Wl,-force_load,libhull_feature-lua.a` (+ js) on macho /
  `--whole-archive ... --no-whole-archive` on GNU ld,
- links `TOOLCHAIN_ONLY_OBJS` (compiled with both macros), and
- links the **generated toolchain registry** providing the strong
  `hl_runtime_feature_factories()` (`{ &hl_lua_factory, &hl_js_factory }`) **and**
  the strong `hl_stdlib_feature_entries()` (lua ++ js) - extend the Change-2
  `stdlib_toolchain_registry` to emit *both* hooks.

The base weak defaults (`runtime/factory.c`'s `hl_runtime_feature_factories`,
`stdlib_feature.c`'s `hl_stdlib_feature_entries`) are overridden by that strong
registry, exactly as an app's generated `feature_registry.c` overrides them.

## Change 3e - build.lua: FEATURE_SPECS rows + auto-inference

`FEATURE_SPECS` gains `lua` / `js` rows. They are whole-archive features (like
`tui`) that fill **two** hooks (factory + stdlib entries), so the `by_hook`
codegen must (a) support a feature contributing to more than one hook and (b)
support an **array-symbol** hook (`hl_stdlib_<rt>_entries`, `HlEntry[]`) alongside
the existing pointer-array-of-structs hook. `hull build` **auto-infers** the
runtime from the entry extension (`app.lua` -> `--with=lua`, `app.js` ->
`--with=js`); an explicit `--with=` still works and both compose = dual app. An
`app.js` built with only `lua` composed is a build-time error with a fix-it.

`FEATURES[]` in `commands/feature.c` gains `lua` / `js` rows (published for the
three native arches; embedded + auto-composed, not install-on-demand).

## Compilation-model gotchas to get right

- **`app_context.o` must be macro-less and accessor-free** (Change 3a) or a slim
  app link-fails on the non-composed `hl_<other>_factory`. This is the load-bearing
  relocation.
- **Toolchain-only objects compiled with both macros** reference concrete VM
  symbols; they resolve only because `hull` force-loads both archives. Never put
  them in `libhull_platform.a` (a macro-less base would compile their VM bodies
  out; a both-macro copy in the app would drag both runtimes in).
- **`doctor.c` / `version.c` / `sbom.c` runtime-name strings** currently `#ifdef`
  a compile-time string. Either make them toolchain-only, or (cleaner) derive the
  name list from the factory registry at runtime so they stay agnostic. Prefer the
  latter where trivial.
- **cosmo** compiles both runtimes into the fat APE (no slim); `HL_*_TOOLCHAIN`
  is native-only, matching tui.

## Phasing (each step keeps a green, dual `hull`)

1. **3a** - the accessor relocation, base still dual. Byte-identical. (Done.)
   The `serve.c` entry-detection + runtime-validation rework rides with 3b (it is
   only exercised once the base is macro-less).
2. **3c** - build the archives; base still dual; assert the archives contain the
   expected symbols (`nm`). No behavior change yet.
3. **3b + 3d** - flip the base runtime-less + force-load both archives + toolchain
   registry (both hooks). Now `hull` links the runtimes *via* the archives. Full
   `make test` + e2e must stay green and dual.
4. **3e** - FEATURE_SPECS rows, dual-hook codegen, auto-inference, `FEATURES[]`.
   Now `hull build app.lua` produces the slim.

## Tests

- **nm slim invariant:** `hull build app.lua` -> zero `JS_*` / QuickJS symbols;
  `app.js` -> zero `lua_*` Lua-VM symbols. Wire into `e2e_feature_runtime.sh`.
- **stdlib-travels:** a lua-only app's embedded VFS has no `hull:*` entry (and no
  JS stdlib bytes); symmetric for js.
- **Dual toolchain unchanged:** default `make` `hull` runs `app.lua` + `app.js`
  (`hull dev/test`, `hull agent eval` for both); `make test` (`test_lua` +
  `test_js`) green.
- **Rejection:** `hull build app.js` with only `--with=lua` fails with the fix-it.
- **libhull still builds** (`make libhull`, shares `HULL_CORE_OBJS`).

## Open questions to resolve during implementation

- Does a produced app pull `dispatch.o` / any `cmd_*.o` at all (does the built app
  binary answer `./myapp version`)? If yes, those commands must be agnostic or the
  app main must not reference them. Confirm via `nm` on a current built example.
- Exact membership of `TOOLCHAIN_ONLY_OBJS` vs `FRAMEWORK_OBJS` for the
  borderline files (`sbom`, `doctor`, `version`, `commands/mcp`, `commands/dev`) -
  classify each by whether a produced app can reach it.
- `libhull.a` (embedder SDK) already excludes runtimes; confirm the
  `HULL_CORE_OBJS` extraction leaves it byte-identical.

## Non-goals

Cosmo runtime slims (fat APE embeds both = full); a `hull feature install lua`
install-on-demand path (runtimes are embedded + auto-composed); WASM/WAMR slim
(separate epic); a third runtime.
