# HTTP as a composable feature — Phase 1 (the seam)

**Status:** design + Phase A. Tracks **issue #114**.
**Prereq:** the runtime-featurify epic (#113, shipped) — runtime-less base +
compose-one-runtime. This epic is its sibling: it does for HTTP what #113 did
for the runtimes.

## Why

Since #113 the native base is runtime-less and a produced app composes exactly
one **full-config** runtime archive. That archive's web-module bindings
reference HTTP caps + Keel, so composing a runtime onto a **reduced** flavor
(server-only / client-only / pure-compute) can't link — `hull build
--flavor=<reduced>` and the composed-runtime `--with=tui` path currently **fail
closed** (guards in `build.lua`, pointing here). Making HTTP a composable
feature with a weak seam lets a reduced flavor **omit** the web bindings, so the
one runtime archive composes onto any flavor — restoring the M+N-composes-M×N
orthogonality the runtime epic promised. The same per-runtime-bridge seam also
re-enables `--with=tui`.

## The coupling map (what the seam must cut)

`grep` of the Lua/JS runtime objects for HTTP/Keel/ws/sse symbols splits the
runtime→HTTP coupling into two kinds:

**A. Purely-web objects** (exist only to serve HTTP; move wholesale behind the
seam / into the feature):
- `mod_http_client`, `mod_http_server`, `mod_ws_client`, `mod_ws_server`,
  `mod_smtp`, `mod_sse` — the web module bindings.
- `sse.c` (SSE stream glue), the ws registry glue in `ws.c`.

**B. Core objects with HTTP references** (can't just move; the *reference* must
be decoupled):
- `dispatch.c` / `bindings.c` / `async.c` → `kl_response*` (building an HTTP
  response). Core request-dispatch code, but only reached on the serve path.
- `mod_app.c` / `mod_request.c` / `routes.c` → `kl_server_*` / `kl_router_*`
  (route registration + wiring to Keel).
- `runtime.c` → `hl_ws_*` (ws registry teardown in the runtime lifecycle).

Kind A is the easy half. **Kind B is the crux**: the runtime *core* touches
Keel's response/router API, so even a pure-compute app (which never serves)
links those symbols today. The seam has to route Kind B through a hook too, so a
no-HTTP base satisfies it with a no-op.

The runtime-agnostic HTTP **cap** layer that becomes the feature's C core:
`cap/http.c`, `cap/http_async.c`, `cap/ws.c`, `cap/smtp.c`, `cap/body.c` (+ the
`hl_ws_registry_*` state), plus all of Keel (`kl_server_*`/`kl_ws_*`/`kl_sse_*`
/ `kl_response*` / `kl_router_*`).

## The seam (weak hooks)

Mirrors #113's `hl_runtime_feature_factories` pattern — weak default in the
base, strong override when HTTP is composed:

```c
/* base: weak no-op defaults */
int  hl_http_feature_present(void);                 /* 0 when HTTP not composed */
void hl_lua_register_http_modules(lua_State *L);    /* registers http/ws/sse/smtp */
void hl_js_register_http_modules(JSContext *ctx);   /* … */
```

- `hl_{lua,js}_register_modules` (core) drops its `#ifdef HL_ENABLE_HTTP_*`
  blocks and instead calls `hl_{lua,js}_register_http_modules(L)` — a weak
  no-op by default, strongly overridden by the composed HTTP feature.
- Route wiring (`routes.c`, `mod_app.c`) and the serve path guard on
  `hl_http_feature_present()`; the Kind-B `kl_*` references move behind
  the hook (or into small feature-side shims the hook installs).

## Phased plan (each phase independently green)

- **Phase A — the seam, NO behavior change (this PR).** Introduce the weak
  hooks; move the web-module registration + the Kind-B references behind them;
  the base still compiles HTTP in and provides the strong override, so behavior
  is **byte-identical**. This is the de-risking refactor (exactly like #113's
  Phase 1). Verify: `make test` + full e2e green; a `grep`/link assertion that
  the core runtime objects no longer *directly* reference `kl_server_*` /
  `kl_response*` / `hl_ws_*` (they go through the hook).
- **Phase B — extract the runtime-agnostic HTTP core** (caps + Keel + smtp +
  compress) into `libhull_feature-http.a` behind the seam; base becomes
  HTTP-core-less; the feature fills `hl_http_feature_present`.
- **Phase C — per-runtime web bindings** compose behind the seam (the
  `(runtime × http)` cell: `mod_http_*`, `mod_ws_*`, `mod_sse`, `sse`, the Kind-B
  shims), materialized only when HTTP is composed. This is where the tui
  per-runtime-bridge pattern is reused.
- **Phase D — publish + wire + default-compose.** `make feature-http`, release
  jobs, embed in `hull`, auto-compose for any app that isn't pure-compute; then
  **remove the fail-closed guards** in `build.lua` and **re-enable** the
  reduced-flavor × runtime and tui e2e (`e2e_build_flavor.sh` step 3,
  `e2e_feature_tui.sh` — currently asserting the #114 fail-closed message).

## Kind-B decoupling: CLEAN SPLIT (decided)

Rather than weak-reference the `kl_*` symbols, Phase A physically extracts the
HTTP-touching functions so the core-runtime objects hold **zero** HTTP
references. The grep shows the refs are cohesive, so the split is bounded:

- `runtime.c` (1 ref, `hl_ws_registry_free` teardown) → guard behind
  `hl_http_feature_present()`.
- `async.c` (3) + `dispatch.c` (6) — the identical 500-error `kl_response_*`
  blocks → one shared `hl_{lua,js}_http_error_response()` helper that lives on
  the HTTP side of the seam.
- `bindings.c` (17, the `res:status/header/json/html/redirect/...` response
  helpers) — the bulk → extract the `res:*` binding group into
  `bindings_response.c`; their registration goes through
  `hl_{lua,js}_register_http_modules` (a no-HTTP base registers no `res:*`,
  which is correct — an app with no request handlers never has a `res`).
- `routes.c` / `mod_app.c` / `mod_request.c` (route + ws wiring) — already
  serve-only; grouped on the HTTP side.

In Phase A these extracted files still compile into the base (byte-identical
behavior); Phase C relocates them into the per-runtime HTTP binding unit. The
JS side mirrors the Lua split file-for-file.

## Open questions
- **HTTP-core: one feature or per-runtime?** The caps + Keel are
  runtime-agnostic (one `http` feature). The web *bindings* are per-runtime. So
  the composed unit is likely `http-core` (agnostic) + `http-<rt>` (bindings),
  or one `http` feature carrying both bridges like `tui`. Decide in Phase B/C.
- **Default-composed vs opt-in.** HTTP is on-by-default today; the feature must
  auto-compose for any non-pure-compute app so `hull build myapp` stays
  zero-config. Selection rides `--flavor=auto` + the module resolver.

## Non-goals

Changing app-facing HTTP APIs (`app.get`, `http.fetch`, ws/sse) — they stay
identical. Cosmo (dual base, HTTP compiled in). Retiring the `HL_ENABLE_HTTP_*`
flags — they remain the base's compile-time switch; the feature is the
*distribution* unit layered on top.

---

## Phase C — implementation design (measured)

Phase B landed: the base is HTTP-core-less (`libhull_platform.a` carries 0 http
caps; `libhull_feature-http.a` carries them) and every full-flavor app composes
the http core. But the per-runtime **web bindings** (routes, dispatch, the
`res:*` helpers, ws/sse/http-client/smtp module bindings) still ride the
*runtime* feature archive, so a genuinely HTTP-free app (a CLI / pure-compute
`app.main`) still links them (and, transitively, Keel). Phase C splits them out.

### Archive split (per runtime `rt ∈ {lua, js}`)

- `libhull_feature-<rt>.a` — **pure runtime**: VM + core bindings (`mod_app`,
  `mod_fs`, `mod_db`, `mod_crypto`, `mod_compute`, `mod_gpu`, `mod_time`,
  `mod_env`, `mod_log`, `mod_image`, `mod_blob`, `mod_buffer`, `mod_mime`,
  `mod_template`, `mod_worker`, `worker_db`, `async`, `bytecode_cache`,
  `template_cache`, `factory`, `modules`, `runtime`) + manifest + stdlib
  registry. ~23-24 objects.
- `libhull_feature-http-<rt>.a` (**new**) — **web bindings**: exactly the set
  the existing `HL_ENABLE_HTTP_SERVER=0` + `HL_ENABLE_HTTP_CLIENT=0` source
  filters already enumerate — `routes`, `dispatch`, `bindings`,
  `bindings_response`, `http_register`, `sse`, `ws`, `timers`, `mod_request`,
  `mod_test`, `mod_http_client`, `mod_http_server`, `mod_ws_server`,
  `mod_ws_client`, `mod_sse`, `mod_smtp`. 16 objects.

Reusing the filter list is deliberate: it is the already-tested definition of
"what is HTTP" per runtime.

### The coupling to cut (nm-measured, not guessed)

Partitioning the runtime objects into {pure} and {web} and diffing undefined vs
defined Hull symbols shows the pure set references the web set at exactly these
edges (Lua; JS is symmetric + one extra):

| pure object | web symbol(s) referenced | already a seam? |
|---|---|---|
| `modules.o` | `hl_lua_register_http_modules` | yes (Phase A) |
| `async.o` | `hl_lua_http_error_response` | yes (Phase A) |
| `async.o` | `hl_lua_timer_reschedule` | **cut** |
| `runtime.o` | `hl_lua_wire_routes`, `hl_lua_wire_routes_server` | **cut** |
| `runtime.o` | `hl_lua_test_register`, `_run`, `_clear` (`+ _free` on JS) | **cut** |

**Base → web edges: zero** (the base reaches HTTP only via the runtime vtable
+ the Phase-A seam). So the whole Phase C coupling surface is 6 edges (Lua) /
7 (JS), each with exactly one caller (`runtime.o` or `async.o`) and its
definition in one web object (`routes.c` / `mod_test.c` / `timers.c`).

### Cutting the edges: weak real-signature stubs (low churn)

Each edge is a link-time reference in the *pure* archive to a symbol *defined*
in the *web* archive. Rather than change call sites or move wrapper bodies
(invasive), provide **weak no-op defaults** with the real signatures in a
per-runtime base TU (`runtime/{lua,js}/http_weakstub.c`, compiled into
`libhull_platform.a`), and **whole-archive** the web-bindings archive at compose
so its strong definitions override. Precedent: `serve.c` (base) already includes
`runtime/lua.h`, so a base TU with these prototypes is not a new dependency.

- HTTP app: pure + web (whole-archived) + http-core composed → strong defs win.
- HTTP-free app: pure only → weak no-ops satisfy the link; the code paths that
  would call them (serve / test / timer-fire) are never reached.

`runtime.c`, `async.c`, `routes.c`, `mod_test.c`, `timers.c` stay **unchanged**;
the split is purely an archive-partition + weak-default question. This is the
same weak-symbol mechanism as the existing `cap/http_feature.c` seam, just with
real signatures (the base can see the prototypes) instead of `void*`.

### Compose, embed, make (mirror Phase B)

- Makefile: define `FEATURE_HTTP_<RT>_OBJS` (the 16 web objects), drop them from
  `FEATURE_<RT>_OBJS` (pure), add `libhull_feature-http-<rt>.a` targets, embed
  them (extend `embedded_http.h`) and add to `RUNTIME_FEATURE_LIBS` so `make`
  builds them. Add the two `http_weakstub.o` to the base `PLATFORM_OBJS`.
- `build.lua` / `feature_compose.lua` / `eject.lua`: resolve + whole-archive
  `libhull_feature-http-<rt>.a` (embedded-first ladder) next to the http core.

### Two sub-steps

- **C1** — the split + weak stubs, composing the web archive **always** (like
  Phase B composes the http core always). Pure refactor, behavior-identical.
  Extra check: an HTTP-free app composing *only* the pure runtime links via the
  weak stubs.
- **C2** — gate the http-core + web-bindings compose on the app actually
  declaring HTTP (any `hull/http-*`, `hull/web/*`, ws/sse/smtp/email module),
  so a genuinely HTTP-free app skips web + http-core + Keel. This is the
  behavior change that delivers the size/authority win; it also unblocks Phase D
  (reduced-flavor × runtime, tui × runtime).

### C2 gating — a measured caveat (found during C1)

C2 gates the http-core + web-bindings compose on the resolved manifest's HTTP
caps (`hl_module_set_required_caps & HL_MOD_CAP_HTTP`). The signal is reliable:
`app.get`/`app.post`/… are **module-conditional decorations** — they are `nil`
unless the app declares `hull/http-server` (verified: an undeclared `app.get`
fails app load with "attempt to call a nil value (field 'get')"), so an app that
serves HTTP always declares an HTTP module.

But gating the *feature compose* alone does not fully strip HTTP from an
HTTP-free binary: **Keel is bundled inside `libhull_platform.a`** (the base), and
`serve.o` (also in the base, on a full/HTTP_SERVER=1 build) references
`kl_server_*`, so the linker still pulls Keel + mbedTLS into an HTTP-free app.
Fully dropping Keel/mbedTLS from a genuinely HTTP-free binary additionally
requires decoupling `serve.o` from Keel (a base→Keel Kind-B edge) — the same
work that unblocks the reduced-flavor × runtime compose. So C2 (skip the http
core + web bindings) and that serve/Keel decoupling belong together, tracked
into Phase D, rather than C2 being a quick gate on top of C1.
