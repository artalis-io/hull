# Keel (the event loop) as a composable feature — Phase 4 of "flavors become presets"

**Goal.** Make Keel — the HTTP server *and* the event-loop/async backend it
provides — a **composed** part of the http feature rather than a base-resident
dependency, so a genuinely HTTP-free app links **zero** Keel. That is the last
thing standing between the composable base and the endgame: once Keel composes,
`pure-compute` is just "an app that composes neither HTTP nor TLS" (compose
nothing extra), so it collapses into a `build.lua` **preset** and the per-flavor
pre-built base matrix (`platform-pure-compute`, `hull flavor install
pure-compute`) can be deleted. This is the direct sequel to the TLS move (a2,
[docs/tls_feature.md](tls_feature.md)), which did the same for mbedTLS.

**Why a preset alone is not enough (the finding that scoped this).** After a2 +
#114 the native base drops the HTTP *caps* (cap/http, ws, smtp, body → the http
feature) and mbedTLS (→ the tls feature). But **Keel the library is still in the
base platform lib**, and the base is compiled `HL_ENABLE_HTTP_ANY=1`, so
`hl_async_backend()` returns the **Keel** event loop (`async_keel`) which
`app.main` / `compute.async` / timers use. Empirically, a no-http/no-tls compute
app built on today's composable base still links **164 Keel symbols**
(`kl_server_run`, `async_keel`, …). So "pure-compute = compose nothing" does not
shed Keel until Keel itself is composable.

## Key grounding facts (de-risk the core)

1. **The poll backend is a complete, Keel-free async + thread-pool
   implementation.** `src/hull/async/poll.c` implements the full `HlAsyncBackend`
   vtable *including* the worker pool (`poll_pool_create` / `poll_pool_free` /
   `poll_pool_submit`, lines ~655-864). So `compute.async` / `db.async` /
   `gpu.async` / `app.main`'s event loop already have a Keel-free provider — the
   compute data-plane does **not** need Keel. #113 proved this: a
   `HL_ENABLE_HTTP_ANY=0` build selects `async_poll`, drops `async/keel.c`, and
   does not link `libkeel.a` — it is a real CI flavor (`pure-compute`).

2. **The compute-path Keel coupling is tiny and already optional.** The base TUs
   that a compute app pulls reference almost no Keel: `shared/async.c` →
   `kl_async_complete` (1 call), `serve_cli.c` → `kl_allocator_default` (1 call);
   `worker_db.c` / `worker_wasm.c` reference the *type* `KlAsyncOp` and a param
   named `kl_op` (not a Keel call). These few primitives (allocator, async
   completion) either resolve Keel-free from the poll backend or are trivially
   shimmable.

3. **The bulk of the base's Keel coupling is HTTP-only** and belongs with the
   http feature anyway: `serve.c` (KlServer setup + routing), `static.c`
   (`kl_response_file` static serving), `test.c` / `test_runner.c` (the
   in-process HTTP test harness). `body.c` / `http.c` / `http_async.c` /
   `smtp.c` / `ws.c` already moved to the http feature in #114. So finishing the
   **serve.c/Keel decouple that #114 deferred** is the real work — and it moves
   these into the same feature that already owns the HTTP caps.

4. **The selector is one function.** `hl_async_backend()` (poll.c:891) is a
   compile-time `#ifdef HL_ENABLE_HTTP` switch today. Turning it into a weak hook
   (base default → `async_poll`; http feature strong override → `async_keel`) is
   the a2 pattern exactly (`hl_crypto_*_active_backend`), and is dormant /
   byte-identical on a full base.

## Coupling map — base TUs that reference `kl_*`, and their disposition

| Base TU | Keel use | Disposition |
|---|---|---|
| `async/poll.c` | none (the Keel-free backend) | **stays in base** — becomes the default |
| `async/keel.c` | the Keel event loop + pool | **→ http feature** (strong `hl_async_backend` override) |
| `shared/async.c` | `kl_async_complete` | base; route through the backend vtable or a weak shim |
| `serve_cli.c` | `kl_allocator_default` | base (app.main runner); use a Keel-free allocator default |
| `serve.c` | KlServer setup, routing, `kl_server_*` | **→ http feature** (the deferred #114 decouple) |
| `static.c` | `kl_response_file` static serving | **→ http feature** (only meaningful with a server) |
| `test.c`, `test_runner.c` | in-process HTTP dispatch | **→ http feature** (HTTP test harness) |
| `worker_db/wasm/gpu.c` | `KlAsyncOp` *type* only | base; type dep via header, no link |
| `release_io.c` | `kl_client_*` (HTTPS) | stays in the **hull binary** toolchain link (not the app base) via the a1 `hl_tls_*` seam pattern |
| `body/http/http_async/smtp/ws.c` | Keel HTTP | already in the http feature (#114) |
| `tls_client/tls_transport.c` | `kl_tls_*` | already in the tls feature (a2) |

## Phase plan

### Phase 4.1 — the async-backend weak seam (dormant)
Turn `hl_async_backend()` into a weak hook: base weak default returns
`&hl_async_backend_poll`; a strong override returning `&hl_async_backend_keel`
lives in the http feature's TU. Byte-identical on a full base (the strong
override always wins there, as today). Mirrors a1's `hl_tls_*` seam. No base
drop yet. **This is the first PR.**

### Phase 4.2 — move Keel into the http feature; base built Keel-free
- Build the base with `async/poll.c` only (no `async/keel.c`), and **do not
  merge `libkeel.a` into the base platform lib**. Move `async_keel.o` + `serve.c`
  + `static.c` + the HTTP test harness + `libkeel.a` into `libhull_feature-http.a`
  (they already sit next to the HTTP caps there).
- Finish the **serve.c decouple** #114 deferred: the base's `app.main` /
  serve-loop entry references a weak seam for "start the HTTP serve loop", whose
  strong impl (KlServer) is in the http feature. A compute app's `app.main`
  runs entirely on `async_poll`.
- Resolve the residual base primitives (`kl_allocator_default`,
  `kl_async_complete`): provide Keel-free equivalents in the base (the poll
  backend already has the machinery) so `serve_cli.c` / `shared/async.c` link
  without Keel.
- Gate: `HL_ENABLE_HTTP_ANY=0` already does most of this at compile time; the
  work is making it **composable** (weak seam + feature archive) rather than a
  separate compile, so ONE base composes Keel back for http apps.

### Phase 4.3 — `pure-compute` becomes a preset; drop the base matrix — **DONE**
- `--flavor=pure-compute` is now a **preset** in `BUILD_FLAVORS[]` with an EMPTY
  asset stem: it builds on the standard composable base and only validates that
  the app declares no HTTP/TLS caps (rejects any HTTP app at build time). A
  compute app on the release's SLIM app-base already links zero Keel/mbedTLS/
  SQLite, so the size payoff comes from the composable base, not a flavor lib.
- Deleted `platform-pure-compute` + `platform-cosmo-pure-compute` (Makefile),
  their release-matrix entries (the native `for f in slim` loop, the whole
  `build-platform-cosmo-flavors` job, the per-flavor SBOMs), and the
  `hull flavor install pure-compute` fetch path (`flavor.c` treats an empty-asset
  flavor as a preset: "nothing to install / builds on the default base").
- **Validated:** `e2e_build_flavor.sh` (rewritten) — unknown-flavor rejection,
  HTTP-app rejection under the preset, build+run, `--flavor=auto`, and the payoff:
  a compute app on a `HL_KEEL_FEATURE=1 HL_TLS_FEATURE=1` base links **0 `kl_*` +
  0 `mbedtls_ssl_handshake`**. Release-pipeline dry-run (hyphenated pre-release
  tag, see [[project_release_dryrun_prerelease_tag]]) still recommended before the
  next real release.

**Epic complete.** Every composable subsystem (runtime, HTTP, WASM, image, TLS,
Keel) now drops from the base and composes back per app; the distributed hull's
default app-base (SLIM) carries none of SQLite/mbedTLS/Keel, and `pure-compute`
is a validation preset rather than a pre-built artifact. "Flavors become feature
presets" is realized.

## Risks + open questions

- **R1 — the serve.c decouple (#114's deferred piece).** `serve.c` wires
  KlServer routing + middleware and is entangled with `app.main`'s lifecycle
  (the serve loop runs after `app.main` returns). The seam must let a compute
  app's `app.main` complete + exit with **no** KlServer reference, while an http
  app composes the serve loop back. This is the largest single piece; the a1
  serve TLS seam ([docs/tls_feature.md](tls_feature.md)) is the template (route
  the few entry points through a weak `hl_serve_*` hook).
- **R2 — cosmo stays full in-base.** A fat APE can't force-load a native feature
  archive, so the cosmo base keeps Keel + HTTP + TLS compiled in (as it already
  keeps mbedTLS + sqlite). Native-only, like every other composed feature.
- **R3 — worker-pool ownership.** `serve_cli.c` creates the pool via the backend
  vtable (`be->pool_create`) *before* the sandbox, and today `be` is
  `async_keel`. On a Keel-free base it is `async_poll` (which has its own pool) —
  verify the pre-sandbox rseq/thread ordering (docs `serve_cli.c` comment) holds
  with the poll pool.
- **R4 — the `platform_domain` signing.** Keel moving into `libhull_feature-http.a`
  changes that archive's bytes; it is already attested in the platform_domain
  (§5c FATAL), so no new signing surface — the existing `http` stem covers it.

See [docs/tls_feature.md](tls_feature.md) for the a2 template this mirrors and
[docs/composed_feature_signing.md](composed_feature_signing.md) for the trust
chain.
