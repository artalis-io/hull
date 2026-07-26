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

## Open questions

- **Kind-B decoupling shape.** Weak-reference the `kl_*` symbols in the core
  objects (link as 0 on a no-HTTP base, never called), or split the HTTP-touching
  functions out of the core objects into feature-side files? Weak refs are less
  churn; a clean split is more honest. Phase A should pick one and hold it.
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
