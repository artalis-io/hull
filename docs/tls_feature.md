# TLS / mbedTLS as a composable feature (design + phase plan)

**Status:** proposed. Not yet implemented. This is the design doc; each phase
lands as its own PR.

## Goal

Extract the TLS stack (vendored mbedTLS + Hull's TLS transport glue) from the
native base into an **embedded, auto-composed feature**, exactly like the
runtimes, HTTP core, WASM, SQLite bridge, and image codec
([docs/image_feature.md](image_feature.md)). The native app-build base becomes
**TLS-less**; `hull build` composes the TLS feature back only when the app needs
it (HTTPS client/server, SMTP TLS, a network DB `sslmode`).

This is **the linchpin of "flavors become feature presets"**
([[project_image_feature_and_flavor_presets]] in memory,
[docs/features_and_flavors.md](features_and_flavors.md)). Today the only reason a
`--flavor=pure-compute` **base artifact** must exist (rather than being "compose
nothing extra") is that mbedTLS + Keel are base-resident and nothing but a
different compile drops them. Once TLS is a composed feature, `pure-compute` is
just the empty compose set, flavors collapse into `build.lua` presets, and the
release pipeline stops publishing per-flavor base libs (one minimal base per
arch + feature archives).

## Why this is tractable now (the two hard parts are already done)

The reason this looked scarier than the image slice is that mbedTLS is woven
through crypto, transport, and the toolchain. But two prerequisites already
shipped, which is what makes the rest mechanical:

1. **The event loop is already decoupled from Keel.** `src/hull/async/poll.c` is
   a Keel-free `HlAsyncBackend`; `src/hull/async/keel.c` is filtered out when
   `HL_ENABLE_HTTP_ANY=0` (Makefile ~1852), and `hl_async_backend()` selects the
   poll backend then. `src/hull/net/keel.c` (server-side request suspension) is
   server-only and drops to no-op stubs otherwise. So `compute.async`, timers,
   and `app.main` async already run **without Keel** — pure-compute proves it in
   CI today. TLS/HTTP does **not** own the event loop.
2. **Crypto is already behind backend vtables, with fallbacks.**
   - HMAC: `HlCryptoHmacBackend` — `hl_crypto_hmac_backend_mbedtls` vs
     `hl_crypto_hmac_backend_portable` (hand-rolled, HW-accelerated where
     available). Both symbols always exist; `cap/crypto.c` picks via
     `HL_HMAC_BACKEND` (`#ifdef HL_ENABLE_HTTP`).
   - Asym (RSA / ECDSA / x509, for JWT-RS256 + OAuth JWKS): `HlCryptoAsymBackend`
     — `hl_crypto_asym_backend_mbedtls` (real, under `HL_LINK_TLS`) vs a **stub**
     that returns "unsupported" and fails closed (the `#else` in
     `cap/crypto_asym_mbedtls.c`). Both compile.
   - Ed25519 / NaCl box / secretbox: TweetNaCl, always in the base, never mbedTLS.

   So crypto-core is **already mbedTLS-optional** at compile time. The featurify
   converts that compile-time selection to a weak-hook the composed feature fills
   (Phase 1) — it does not need a new abstraction.

3. **Keel isolates *all* its mbedTLS in one object.** `vendor/keel/src/tls_mbedtls.c`
   is the **only** Keel source that references mbedTLS; the event loop
   (`event_{epoll,kqueue,poll}.c`), server, client, router, and thread pool are
   entirely mbedTLS-free. So "split Keel from TLS" is not a rewrite — it is
   dead-stripping / relocating a single `tls_mbedtls.o` (see the R1 update below).

> **Status (2026-07-31).** Prerequisite 2 is now **done in the tree**: PR #140
> (HMAC) + #141 (asym) converted the crypto selection to the weak accessors
> `hl_crypto_hmac_active_backend()` / `hl_crypto_asym_active_backend()`
> (`include/hull/tls_feature.h`). crypto-core is fully mbedTLS-optional behind
> runtime hooks. Prerequisites 1 + 3 were already true. What remains is the
> **transport = Keel move** (Phases 2-3 below, now merged into one).

## Current coupling (the map)

`HL_LINK_TLS` is the existing all-or-nothing switch (Makefile ~454):

```
HL_LINK_TLS := 1  iff  (any HTTP half)  OR  HL_ENABLE_POSTGRES  OR  HL_ENABLE_MYSQL
              := 0  otherwise  (pure-compute: drops mbedTLS + Keel entirely)
```

When 1, `MBEDTLS_OBJS` (the vendored ~1 MB) lands in **both** the base
`PLATFORM_OBJS` and the `hull` binary, and `KEEL_LIB` is linked (Keel is *built
with* mbedTLS: `$(KEEL_LIB): $(MBEDTLS_OBJS)`).

**Who calls `mbedtls_*` directly (the consumer surface to seam):**

| TU | Concern | Notes |
|----|---------|-------|
| `cap/crypto_hmac_mbedtls.c` | crypto (HMAC) | **DONE (#140):** weak accessor + portable fallback |
| `cap/crypto_asym_mbedtls.c` | crypto (RSA/ECDSA/x509) | **DONE (#141):** weak accessor + base fail-closed stub |
| `shared/tls_client.c` | transport | KlTls blocking handshake for SMTP + Postgres + MySQL `sslmode`. **Not referenced by any base TU** — the smtp / pg / mysql consumers live in *features* and resolve it at compose via `--start-group`. |
| `serve.c` | transport (Keel) | uses Keel's `KlTlsConfig` for HTTPS serving → pulls Keel's `tls_mbedtls.o`. No direct `mbedtls_*` calls. |
| `release_io.c` + `commands/{update,flavor,tools,feature}.c` | **toolchain** (Keel) | HTTPS client for `hull update` / installs, via Keel's client → `tls_mbedtls.o`. |
| ~~`cacert.c`~~ | — | **Data only.** Just the embedded CA-bundle bytes + label; zero `mbedtls_*` calls (the x509 mention is a code comment). No seam needed. |

**The finding that reframes Phases 2-3.** Once crypto is decoupled (done), the
base's *entire* remaining mbedTLS coupling flows through **Keel**: `serve.c`
(HTTPS serving) and the toolchain HTTPS client both reach mbedTLS only via Keel's
`tls_mbedtls.o`; `cacert.c` is pure data; and `tls_client.c` is not referenced by
the base at all (its consumers are feature-resident). So there is **no
crypto-style series of dormant Hull-side transport seams** — "the transport seam"
*is* the Keel move. And because Keel isolates TLS in a single object (finding #3),
that move is a bounded relocation, not a rewrite.

The one remaining split that matters:

- **toolchain HTTPS** (`hull update` / `flavor|tools|feature install`) is a
  property of the **hull binary**, not of apps. Hull stays TLS-full for its own
  commands (exactly like it stays image-full while the base is image-less). Only
  the **app-build base** goes TLS-less. So the Keel move must keep Keel+mbedTLS on
  the `hull` link line while dropping them from the app-build `PLATFORM_OBJS`.

## Key design decisions

- **D1 — Toolchain stays TLS-full; only the app base is TLS-less.** The `hull`
  binary keeps mbedTLS + Keel for `hull update` and the install commands (the
  same asymmetry as image: hull is image-full, the base it *builds apps against*
  is image-less). This means `MBEDTLS_OBJS` stays on the `hull` link line and
  moves off `PLATFORM_OBJS`.

- **D2 — The gate mirrors `HL_LINK_TLS`, moved compile-time → compose-time.**
  `needs_tls = needs_http OR (a network DB backend is composed / a net DSN is
  present) OR needs_smtp`. Reuses the exact conditions `HL_LINK_TLS` already
  encodes. `needs_http` dominates in practice (a web app that calls `http.fetch`
  almost always hits `https://`), so TLS effectively **rides with the HTTP
  feature** — compose them together. The postgres/mysql net-DB and smtp signals
  are the same explicit `--with` / DSN signals those backend features already
  use.

- **D3 — Crypto backend selection: `#ifdef` → weak hook.** Replace the
  compile-time `HL_HMAC_BACKEND` / asym `#ifdef` with the gpu/tui-style pattern:
  the base ships the **portable HMAC + stub asym** as the active backend behind a
  weak `hl_crypto_tls_backends()` hook (returns "no mbedTLS"); the composed TLS
  feature provides a **strong override** returning the mbedTLS HMAC + asym
  backends. `crypto.*` in a TLS-less base keeps working (SHA/HMAC-portable/
  Ed25519); `crypto.asym_verify` fails closed until TLS is composed.

- **D4 — Keel rides with the HTTP feature, not TLS.** The event loop is already
  Keel-free in the base (async/poll). Keel's remaining role is the HTTP **server**
  + the HTTPS **client**, both of which are the HTTP feature's concern (#114
  moved the HTTP caps out already; Keel is the last HTTP-shaped thing still in
  the base). So `libkeel.a` moves into `libhull_feature-http.a`'s compose (or a
  sibling), gated by `needs_http`. TLS (mbedTLS) is a separate archive the HTTP
  feature depends on, and that the DB-net/smtp paths can pull independently.

- **D5 — asym-without-HTTP is the accepted edge.** An app that wants JWT-RS256 /
  OAuth-JWKS verification but declares no HTTP module (rare: OAuth needs
  `http.fetch` to the IdP anyway) hits the stub asym backend (fails closed). It
  composes TLS by declaring HTTP, adding a `--with=tls` escape hatch, or using
  Ed25519 (TweetNaCl, always present). Documented, not silently broken.

- **D6 — Cosmo stays TLS-full in-base.** A fat APE can't force-load native
  feature archives, so cosmo keeps mbedTLS + Keel compiled in (like the runtimes,
  HTTP, WASM, image). Native-only feature, as always.

## Phase plan

Each phase is independently valuable and independently landable.

### Phase 1 — Crypto selection: `#ifdef` → weak hooks — **DONE (#140 + #141)**
Shipped as per-backend weak accessors (`hl_crypto_hmac_active_backend()` +
`hl_crypto_asym_active_backend()` in `include/hull/tls_feature.h`) rather than one
combined `hl_crypto_tls_backends()` struct. The per-backend shape was forced by a
hard invariant that surfaced during #140: **`cap/crypto.o` must stay independent
of the asym TU** — some test link sets (`test_sbom`) pull `crypto.o` + the HMAC TU
without the asym TU, so a hook whose default referenced the asym symbol broke
those links. Each accessor's weak default references only its own file-local
symbol; the concrete mbedTLS backend is a **strong override in its own TU**. This
same `--start-group` / link-granularity rule governs the TLS archive below.
Dormant: HTTP builds still use mbedTLS, TLS-less builds fail closed.

### Phase 2+3 — The Keel move: base drops Keel+mbedTLS, HTTP feature composes them
The transport is **not** a series of dormant Hull-side weak stubs (see "The
finding that reframes Phases 2-3"): the base's remaining mbedTLS coupling is
Keel, so this is one deliberate relocation. Because Keel isolates TLS in a single
`tls_mbedtls.o`, it is bounded:

- **The seam** is `serve.c`'s `KlTlsConfig` usage (HTTPS serving). Route it
  through a weak `hl_tls_server_*` accessor (mirrors the crypto hooks) so a
  TLS-less base serves HTTP-only; the HTTP feature's strong override wires Keel's
  server TLS + pulls `tls_mbedtls.o` + mbedTLS.
- **`libhull_feature-tls.a`** = `MBEDTLS_OBJS` + `tls_client.o` +
  `crypto_{hmac,asym}_mbedtls.o` + the strong crypto/TLS overrides. Whole-archived
  at compose inside `--start-group` (the crypto link-granularity rule from #140).
- **Keel** (`libkeel.a`, minus `tls_mbedtls.o` from the base's view) rides with
  the HTTP feature (D4); its TLS object is pulled only when the TLS feature is
  composed. Verify Keel dead-strips `tls_mbedtls.o` cleanly when unreferenced.
- **Toolchain HTTPS stays on the `hull` link line** (D1): drop Keel+mbedTLS from
  the app-build `PLATFORM_OBJS`, keep them for the `hull` binary's own
  `update`/`install` commands. This is the part that needs the most care — today
  `release_io.o` + `CMD_OBJS` sit in `PLATFORM_OBJS`.
- Wire `needs_tls` (already exposed dormant from `tool.modules_resolve`) into
  `build.lua` + `feature_compose.lua` + `build_assets.c`, OR-ing in the network-DB
  signal, mirroring image/wasm.
- **Validate:** new `tests/e2e_feature_tls.sh` — a plaintext-only app drops
  mbedTLS (`nm app | grep mbedtls_ssl_handshake` → empty, ~1 MB smaller); an HTTPS
  app composes TLS and a real handshake to `example.com` succeeds; both runtimes;
  cosmo stays TLS-full in-base; `hull update` still works.

### Phase 4 — Redefine `pure-compute` as a preset; flavors → presets
- `pure-compute` becomes "compose neither HTTP nor TLS" — a `build.lua` preset,
  not a pre-built base artifact. Delete the `platform-pure-compute` build/publish
  path.
- Generalize: every `--flavor` becomes a named feature-set in `build.lua`
  (`full` = the common web set; `pure-compute` = `{runtime, wasm}`). Drop the
  per-flavor `platform-<flavor>` targets + their release matrix entries + `hull
  flavor install` for them.
- **Validate:** `e2e_build_flavor.sh` updated; release-pipeline dry-run
  (hyphenated pre-release tag, per [[project_release_dryrun_prerelease_tag]]).

### Phase 5 — Release + signing — **DONE (a2 part 3)**
- The `tls` stem is wired into `release.yml`: `feature-tls` is built + uploaded
  with the other embedded feature archives, hashed into the signed platform
  manifest (native-arch feature stems), SHA-verified in stage 3, and embedded
  via `TRUST_FEATURE_LIBS`. It lands in the `platform_domain` of
  `package.sig.gethull.composed` (platform-key attested, runtime §5c FATAL) —
  the same trust chain as the runtime/HTTP/WASM/image archives
  ([docs/composed_feature_signing.md](composed_feature_signing.md)).
- The embedded **app-build base** for a release is now the combined **SLIM**
  base (`platform-slim` = `HL_SQLITE_FEATURE=1 HL_TLS_FEATURE=1`), embedded via
  `HL_APP_BASE_SQLITELESS=1 HL_APP_BASE_TLSLESS=1`, replacing the SQLite-only
  `sqliteless` base. So a stock `hull build` drops **both** SQLite and mbedTLS
  and composes each back per app. Cosmo stays full (fat APE can't force-load).
  Validate via a dry-run pre-release tag before a real release (the shipped
  `HL_PLATFORM_PUBKEY_HEX` placeholder means §5c is skipped at runtime today, so
  this is future-correct; the produced-app size payoff is already live).

## Risks + open questions

- **R1 — Keel ↔ mbedTLS build coupling — DE-RISKED (was "the one real unknown").**
  Keel is *built with* mbedTLS (`$(KEEL_LIB): $(MBEDTLS_OBJS)`), but investigation
  (2026-07-31) shows **`vendor/keel/src/tls_mbedtls.c` is the only Keel TU that
  references mbedTLS** — the event loop, server, client, and router are all
  mbedTLS-free. So the split is a clean dead-strip: a base/link that references no
  `kl_tls_*` symbol pulls neither `tls_mbedtls.o` nor mbedTLS from `libkeel.a`, and
  the HTTP+TLS feature pulls both. The residual work is (a) confirming `libkeel.a`
  ships `tls_mbedtls.o` as a separately-strippable member (it should — one object,
  one archive member), and (b) routing `serve.c`'s `KlTlsConfig` use through a
  weak accessor so the base emits no `kl_tls_*` reference. Bounded, not a rewrite.
- **R1b — toolchain HTTPS in `PLATFORM_OBJS`.** `release_io.o` + `CMD_OBJS` (the
  `hull update`/install HTTPS client) currently sit in the app-build platform lib,
  so they'd force mbedTLS/Keel into the base. The Keel move must relocate them to
  the `hull`-binary link only (apps never call hull commands). Verify nothing in
  the app-build path references them (they should dead-strip today already).
- **R2 — mbedTLS is ~1 MB and referenced from many mbedTLS-internal TUs.** The
  archive is self-contained (it's a vendored lib), so whole-archiving it is fine;
  the risk is link ordering (`--start-group` with crypto-core which references
  the backends). Mirror the DB-feature `base_group` handling.
- **R3 — Signing surface grows.** One more attested archive; machinery exists.
- **Q1 — One `tls` archive or split `tls-transport` + `crypto-mbedtls`?** Leaning
  one archive (they always compose together in practice; asym-without-transport
  is the D5 edge). Revisit if a real "asym verify, no network" use case appears.
- **Q2 — Does `http.fetch` to a plaintext `http://` host justify a TLS-free HTTP
  build?** Almost never worth a separate flavor; TLS rides with HTTP (D2). The
  `--with`/DSN signals cover the DB/SMTP-without-HTTP cases.

## Payoff

- **One axis.** feature (additive) is the whole model; flavors are named presets.
- **Smaller minimal base.** A CLI / compute / embedder binary drops mbedTLS + Keel
  (~1 MB+) with no special flavor — it just composes nothing.
- **Simpler release.** One minimal base per arch + feature archives; no
  per-flavor base matrix, no `hull flavor install` for pure-compute.
- Marginal for typical web apps (they compose HTTP+TLS anyway) — the win is
  conceptual unity + the minimal/embedder/CLI footprint + pipeline simplicity.
