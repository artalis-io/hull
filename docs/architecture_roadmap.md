# Hull Architecture Roadmap

Post-v0.1.0 cohesion + coupling refactor backlog. Derived from a full audit of `src/hull/` and `include/hull/` (≈ 45k LOC, 110 files; vendored libraries excluded).

**TL;DR:** Hull's architecture is fundamentally sound — capability boundary is intact, dispatcher is table-driven, vtables exist for runtime / db / compiler / gpu. The findings below name smells worth scheduling, not active bugs.

## Module map

Top-level Hull modules and where they call.

| Module | Responsibility | Calls into | Called from |
|--------|----------------|------------|-------------|
| `main.c` | CLI parse → server boot → event loop. 1308 lines, 11 phases | `app_context`, `manifest`, `sandbox`, `signature`, `static`, `agent_api`, `commands/dispatch`, `cap/*`, `vfs`, Keel | (entry point) |
| `app_context.c` | Bundle: DB + VFS + runtime init/load/free. 406 lines | `cap/db_backend`, `migrate`, `vfs`, `runtime/lua`, `runtime/js`, `cap/wasm` | `main.c`, `commands/test.c`, `agent_lib.c`, `commands/mcp.c` |
| `agent_lib.c` | Agent introspection: routes, db_schema, db_query, test, request, status, errors, context, migrate, deploy. 1353 lines | `app_context`, `cap/db`, `cap/tool`, `runtime/lua`, `runtime/js`, `migrate`, Keel | `commands/agent.c`, `commands/mcp.c`, `agent_api.c` |
| `manifest.c` | Parse `app.manifest({…})` from Lua registry OR JS globalThis. Both impls in one file. 459 lines | `cap/alloc`, Lua/JS C API | vtable consumers |
| `sandbox.c` | OS sandbox enforcement. Per-OS dispatch for Linux pledge, OpenBSD native, macOS Seatbelt, Cosmo. Reads `HlManifest` directly. 677 lines | `HlManifest` fields, `HlToolUnveilCtx` | `main.c`, `cap/tool` |
| `commands/dispatch.c` | Table-driven subcommand dispatch + global flag parsing. 99 lines | every `commands/*.c` | `main.c` |
| `runtime/lua/runtime.c` | VM lifecycle + sandbox + alloc + module loader + dispatch + WebSocket handlers + SSE handler + route wiring + middleware + timers + vtable. **1713 lines (god module)** | Lua C API, Keel, `manifest`, `cap/*` | `app_context`, `tool.c`, vtable dispatch |
| `runtime/js/runtime.c` | Same as Lua, for QuickJS. **2245 lines (god module)** | QuickJS C API, Keel, `manifest`, `cap/*` | `app_context`, vtable dispatch |

## High-priority findings

### Critical (architectural layering breaks)

**C1. `cap/test_lua.c` + `cap/test_js.c` are runtime bindings sitting in `cap/`.**
The cap layer is supposed to be the "C enforcement boundary — no runtime knowledge" (per `CLAUDE.md`). But `cap/test_lua.c` is 404 lines of Lua bindings (`l_test_call`, `l_test_get`, etc.) and includes `hull/runtime/lua.h` — a layering inversion.
**Refactor:** Move shared dispatch helper to stay in `cap/test.c`. Move `cap/test_lua.c` → `runtime/lua/mod_test.c`; `cap/test_js.c` → `runtime/js/mod_test.c`. Update Makefile.

**C2. `cap/tool.c` mixes cap layer with Lua bindings (1035 lines, two purposes).**
Top half is the cap (unveil/spawn/check/copy/mkdir/rmdir, ~520 lines, no Lua). Bottom half (~520 lines) is Lua bindings + compiler-vtable wrappers.
**Refactor:** Split into `cap/tool.c` (pure C, no Lua) and `runtime/lua/mod_tool.c`. Drops cap/tool.c from 1035 → ~520 lines.

**C3. `runtime/{lua,js}/runtime.c` are god modules (1713 + 2245 lines).**
Each does VM init + sandbox + custom allocator + module loader + console + interrupt/gas hook + dispatch + free + timer trampoline + WebSocket handlers + SSE handler + route wiring + middleware bridge + vtable wrappers — all in one file.
**Refactor:** Per runtime, split into 5–6 files:
- `runtime.c` — VM lifecycle + sandbox + allocator + vtable (≤ 500 lines)
- `dispatch.c` — `hl_*_dispatch()`, middleware bridges (~250 lines)
- `routes.c` — wire_routes, wire_routes_server, route tracking (~400 lines)
- `timers.c` — timer trampoline, daily computation (~150 lines)
- `ws.c` — WebSocket handlers (~200 lines)
- `sse.c` — SSE handler (~150 lines)

### High (duplication / leaky abstractions)

**H1. `agent_lib.c` duplicates the parallel "Lua path / JS path" pattern in five places.**
`agent_routes_lua`/`_js`, `agent_test_lua_ctx`/`_js_ctx`, etc. Root cause: `HlRuntimeVtable` doesn't expose route enumeration or test execution.
**Refactor:** Extend the vtable with `enumerate_routes`, `enumerate_middleware`, `run_test_file`. Then agent funcs become single-path.

**H2. `commands/test.c` duplicates the agent_test runner.**
Same control flow as `agent_test_lua_ctx` / `agent_test_js_ctx`, differs only in output format (stdout vs JSON).
**Refactor:** After H1, unify into one `hl_test_runner_run(ctx, opts, writer)` with pluggable writer.

**H3. `manifest.c` interleaves Lua and JS extractors (459 lines, two #ifdef blocks).**
Both implementations share helpers but the actual extractors are completely separate code paths gated by `#ifdef HL_ENABLE_LUA` / `HL_ENABLE_JS`.
**Refactor:** Keep `manifest.c` with shared HlManifest + helpers + free + validation. Split extractors out: `manifest_lua.c`, `manifest_js.c`. Each #ifdef-conditional source goes in only when its runtime is enabled.

**H4. `sandbox.c` reads `HlManifest` fields directly throughout.**
References `manifest->fs_read`, `hosts_count`, `gpu_devices`, etc. Adding a manifest field is a manifest + sandbox change.
**Refactor:** Introduce `HlSandboxPolicy` — a pre-resolved struct sandbox reads, independent of manifest layout. `main.c` builds the policy from manifest. Sandbox stops being a manifest consumer. Low effort, big decoupling win.

**H5. `cap/db_udf.c` reaches across the `HlDbBackend` vtable to grab raw `sqlite3 *`.**
`mod_db.c:718, 843` (Lua) and `mod_db.c:809, 967` (JS) both do `sqlite3 *raw = hl_db_sqlite_raw(handle)` then call `sqlite3_create_function_v2()` directly.
**Refactor:** Either (a) add `backend->register_udf(scalar, agg)` to the vtable so non-SQLite backends can return "unsupported"; or (b) explicitly document UDF as a SQLite-only feature with a fail-fast. Prefer (a).

**H6. `migrate.h` takes raw `sqlite3 *`, bypassing the db_backend vtable.**
`int hl_migrate_run(sqlite3 *db, const HlVfs *vfs);`. App_context extracts the raw pointer to call this.
**Refactor:** Change `hl_migrate_run` to take `HlDbHandle *`. Removes `sqlite3` forward-decl from `app_context.h`.

### Medium

**M1. `HlLua` / `HlJS` are fully exposed structs.** Every internal field is in the public header. Adding a struct field is an ABI break. → Move to internal headers post-v0.1.0; expose only opaque typedefs.

**M2. `limits.h` is a god-constants header (17+ consumers).** WASM/GPU constants force recompiles of unrelated TUs. → Split into `limits/core.h`, `limits/runtime.h`, `limits/wasm.h`, `limits/gpu.h`.

**M3. `app_context.c` has 5× `is_lua` ladders.** Lifecycle (init, load_app, free) isn't reached via the vtable. → Add a runtime-factory registration so ladders collapse to one call.

**M4. `main.c::hl_serve_wire_and_start()` is 291 lines.** Manifest → caps wiring → TLS → sandbox → routes → static → agent API → run. Extract: `wire_caps`, `wire_routes`, `run`.

**M5. `agent_lib.c` (1353 lines) does five unrelated agent operations.** Split into `agent/{routes,test,db,request,context,deploy}.c`.

**M6. `cap/audit.c` has process-wide `extern int hl_audit_enabled`.** Inconsistent with the rest of the cap layer; tolerable for now (debug flag).

**M7. `cap/http_async.c` has a static module-internal global allocator.** Module-scoped, so OK — but no per-runtime isolation if two contexts ever ran in one process.

**M8. `app_context.h` forward-declares `sqlite3`.** Tied to H6; resolves automatically once migrate is on the vtable.

**M9. Per-runtime `worker_db.c` files duplicate most logic.** 224 lines (Lua) + 273 lines (JS) of mostly parallel binding code. Same idea as H1.

**M10. `lua_get_buffer` / `js_get_buffer` parallel implementations.** Pattern is identical; could share helpers (e.g., a "validate buffer constraints" function).

## Architectural roadmap

Bundled and ordered after the pre-v0.1.0 weed-through. Effort: S = < 1 day, M = 1–3 days, L = 3+ days.

### Scheduled before v0.1.0

| # | Refactor | Effort | Status | Rationale |
|---|----------|:------:|--------|-----------|
| **A** | **C3** — Split each `runtime/*/runtime.c` into 5–6 files (`runtime.c`, `dispatch.c`, `routes.c`, `timers.c`, `ws.c`, `sse.c`) | M | ✅ done | God modules (1713 + 2245 lines) → focused files. Single largest QoL improvement; the split happens **before** v0.1.0 so the file layout doesn't shift after the stability commitment. |
| **B** | **M2** — Split `limits.h` per subsystem (`limits/core.h`, `limits/runtime.h`, `limits/wasm.h`, `limits/gpu.h`); `limits.h` kept as umbrella for back-compat; consumers narrowed where each only touches one subsystem | S | ✅ done | Touching a WASM constant no longer rebuilds GPU / runtime TUs (and vice versa). |
| **C** | **H4** — `HlSandboxPolicy` decouples sandbox from manifest. New struct + `hl_sandbox_policy_from_manifest()` builder; `sandbox.c` no longer reads `manifest->X` fields directly. | S | ✅ done | Manifest format can evolve in 1 layer (extraction) instead of 2 (extraction + sandbox). Removed 14 cross-layer reach-arounds. |
| **D** | **H6 + M8 + H5** — Complete the db_backend abstraction. `hl_migrate_run/status` take `HlDbHandle *` instead of `sqlite3 *`; `app_context.h` drops the `sqlite3` forward-decl + adds `hl_app_context_db_handle()`; `hl_cap_db_udf_register_wasm/unregister` take `HlDbHandle *` and contain the SQLite reach-around inside `db_udf.c`. New `hl_db_sqlite_wrap/unwrap` helper for code paths still opening SQLite directly. | S | ✅ done | Public migrate/UDF APIs are backend-agnostic; the only remaining raw `sqlite3 *` reach-arounds are confined to `db_udf.c`'s Lua/JS scalar/aggregate-UDF callback path, which is structurally tied to SQLite's xFunc/xStep ABI. |
| — | **Release signing**: Ed25519 over `hull.sha256` manifest; embedded `HL_RELEASE_PUBKEY_HEX`; `hull sign-release` + `hull verify-release` commands; `hull update` enforces signature when pubkey is configured; release workflow signs via repo secret. Design: `docs/release_signing.md`. | M | ✅ code done; awaiting real release key | Steps 1-8 of the design are landed. Step 9 (generate key, commit pubkey, set GitHub secret) is operational and happens at release time. |

### Scheduled after v0.1.0

Recommended sequence — every item is small/medium, no dependencies between groups except where noted.

| # | Refactor | Effort | Rationale |
|---|----------|:------:|-----------|
| ✅ **E** | **C1** — Move `cap/test_{lua,js}.c` → `runtime/{lua,js}/mod_test.c`. Split `cap/test.h` into a pure-C `cap/test.h` (HlTestResult + `hl_cap_test_dispatch`) and a new `runtime/test.h` for the per-runtime bindings. Functions renamed `hl_cap_test_*_{lua,js}` → `hl_{lua,js}_test_*`. | S | Done. `grep -r "hull/runtime" src/hull/cap/` now returns zero — the cap layer is genuinely runtime-free. |
| ✅ **F** | **C2** — Split `cap/tool.c` Lua bindings into `runtime/lua/mod_tool.c`. Pure-C unveil + spawn + filesystem helpers stay in `cap/tool.c` (1035 → 519 lines). New `include/hull/runtime/tool.h` declares the Lua `tool` global; entry points renamed `hl_cap_tool_{register,expose_compiler}` → `hl_lua_tool_*`. `cap/tool.h` no longer mentions `lua_State`. | S | Done. After E + F, `grep -r 'hull/runtime\|"lua.h"\|"quickjs.h"' src/hull/cap/ include/hull/cap/` matches only comments — the cap layer has zero runtime knowledge. |
| ✅ **G** | **H3** — Split `manifest.c` → `manifest.c` (shared helpers + `hl_manifest_free`) + `manifest_lua.c` + `manifest_js.c`. New private `manifest_internal.h` declares the shared helpers (`hl_manifest_strdup`, `hl_manifest_str_free`, `hl_manifest_csp_is_valid`) so the extractors can share them without duplication. The previous `MANIFEST_{LUA,JS}_OBJ` Makefile variants compiled with `-DHL_ENABLE_{JS,LUA}` filtered out; with the split that's no longer needed — each per-runtime `.c` `#ifdef`-guards itself and compiles to empty when its runtime is disabled. | S | Done. Mirrors the runtime/{lua,js} layout we already have for `test` (E) and `tool` (F). Adding a Lua-only manifest field is now a `manifest_lua.c` change; adding a JS-only one is a `manifest_js.c` change. |
| ✅ **H** | **M4** — Extract phases from `main.c::hl_serve_wire_and_start` into 5 named helpers (`wire_caps`, `apply_sandbox`, `wire_routes`, `run`, `teardown_after_serve`) + a small `undo_caps` for shared error-path cleanup. The orchestrator shrinks to ~15 lines. | S | Done. 296-line function → orchestrator + 5 cohesive phases. Each phase has a single responsibility and a single error contract. |
| 🟡 **I** | **H1 + H2 + M5** — Agent surface cleanup. Progress: **step 1 done** — `enumerate_routes` + `enumerate_middleware` added to `HlRuntimeVtable`; `agent_routes_lua`/`_js` (~150 lines × 2) collapsed to a single 30-line vtable-driven impl in `agent_lib.c`. **Remaining**: step 2 — `run_test_file` vtable method + unify the test runner under a pluggable writer (H2). Step 3 — split `agent_lib.c` (1253 lines) into `agent/{routes,test,db,request,context,deploy}.c` (M5). | M | First pair of Lua/JS sibling functions eliminated; pattern set for the remaining two pairs in `agent_lib.c`. |
| **J** | **M1** — Mark `HlLua`/`HlJS` internals private; introduce `internal.h` for ops like `HlLuaWorkerDispatchOp` | M | Long-term opaque-context migration. Tier-4 docstrings in headers are good enough for most of v0.x; do the move when a third-party consumer asks. |

### Deferred (no current need; tracked for visibility)

| # | Refactor | Effort | Reason for deferral |
|---|----------|:------:|---------------------|
| **K** | **M3** — Runtime factory registration; collapse `is_lua` ladder in `app_context.c` | M | Justifies a "third runtime" (e.g. WASM-as-orchestrator) — but there's no third runtime planned. Reconsider when one is. |
| **L** | **M9 + M10** — Share worker_db / get_buffer code between runtimes | M | Once item I lands, the remaining duplication is small and not painful. Reconsider if it becomes painful. |

## Things that look OK

- **Capability boundary integrity.** No runtime module reaches around `hl_cap_*` for sqlite3 / mbedTLS / open / read / write (verified by grep). The H5/H6 reach-arounds are the only exceptions, both confined to SQLite UDF + migrate.
- **`commands/dispatch.c` is genuinely clean.** All 20 commands use `HlCommandEnv`. No globals grubbing.
- **`HlAppContext` is well-scoped.** Bundles DB+VFS+runtime properly; supports both pure-compute (no_db) and deferred load. Not a kitchen sink — its responsibility is exactly the init bundle.
- **`main.c` is well-phased.** Despite 1308 lines, it's split into 11 named phases with documented preconditions.
- **All vtables are opaque-pointer style** with inline wrappers — `HlRuntimeVtable`, `HlCompilerVtable`, `HlDbBackend`, `HlGpuBackend`, `HlImageCodec` — and the pattern is consistent.
- **`vfs.c`** — small, focused, single responsibility. 7+ consumers, clean.
- **`signature.c`** — single concern (Ed25519 read/verify on VFS entries).
- **No circular includes.** Forward-declarations used consistently.
