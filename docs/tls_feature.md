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
| `cap/crypto_hmac_mbedtls.c` | crypto (HMAC) | already a backend vtable + portable fallback |
| `cap/crypto_asym_mbedtls.c` | crypto (RSA/ECDSA/x509) | already a backend vtable + stub fallback |
| `shared/tls_client.c` | transport | KlTls blocking handshake for SMTP + Postgres + MySQL `sslmode` |
| `cacert.c` | transport | mbedTLS x509 parse of the embedded CA bundle |
| `serve.c` / `serve_cli.c` | transport | inbound TLS server setup (HTTPS serving) |
| `release_io.c` | **toolchain** | HTTPS client for `hull update` + the install commands |
| `commands/{update,flavor,tools,feature}.c` | **toolchain** | HTTPS download (via Keel client) |

Three concerns, and the split matters:

- **crypto** — already abstracted; convert selection to a weak hook.
- **transport** (tls_client, cacert-parse, serve TLS) — genuinely network TLS;
  moves to the feature with weak stubs left in the base.
- **toolchain HTTPS** (`hull update` / `flavor|tools|feature install`) — a
  property of the **hull binary**, not of apps. Hull stays TLS-full for its own
  commands (exactly like it stays image-full while the base is image-less). Only
  the **app-build base** goes TLS-less.

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

### Phase 0 — Audit + weak-hook scaffolding (no base change)
- Enumerate the exact `mbedtls_*` + `kl_tls_*` reference surface per TU (grep is
  in this doc; turn it into the seam header).
- Add `include/hull/tls_feature.h`: a weak `hl_crypto_tls_backends()` hook
  (returns the mbedTLS HMAC + asym backends when composed, NULL/portable in the
  base) + weak transport stubs for `tls_client` / `cacert`-parse. Additive and
  dormant (the strong mbedTLS defs still win while `HL_LINK_TLS=1`), mirroring
  the WASM Phase-0 `wasm_weakstub.c`.
- **Validate:** default build byte-identical behavior; `make test` green.

### Phase 1 — Crypto selection: `#ifdef` → weak hook
- Route `cap/crypto.c`'s HMAC + asym dispatch through `hl_crypto_tls_backends()`
  instead of the `HL_ENABLE_HTTP` / `HL_LINK_TLS` `#ifdef`. Base default =
  portable HMAC + stub asym; a strong override (in the TLS feature) swaps in
  mbedTLS.
- **Validate:** `test_crypto` green with and without the override linked; a
  pure-compute build still does HMAC via the portable backend.

### Phase 2 — Transport seam (TLS-less base links)
- Weak-stub `shared/tls_client.c` (KlTls handshake) + the `cacert.c` x509 parse +
  the `serve.c` TLS-server setup so the base links with mbedTLS absent. SMTP /
  Postgres / MySQL over TLS fail closed without the feature (a `sslmode=require`
  DSN on a TLS-less binary errors with a compose hint), plaintext still works.
- **Validate:** a `HL_LINK_TLS=0`-shaped base links; `nm base | grep
  mbedtls_ssl_handshake` → empty.

### Phase 3 — The TLS feature archive(s) + compose
- `libhull_feature-tls.a` = `MBEDTLS_OBJS` + `tls_client.o` + the cacert x509
  parse + `crypto_{hmac,asym}_mbedtls.o` + the strong `hl_crypto_tls_backends()`
  override. Embedded in hull (`embedded_tls.h`), whole-archived at compose.
- Move `libkeel.a` into the HTTP feature's compose (D4).
- Wire `needs_tls` (D2) into `tool.modules_resolve` + `build.lua` +
  `feature_compose.lua` + `build_assets.c`, mirroring image/wasm.
- **Validate:** new `tests/e2e_feature_tls.sh` — a plaintext-only app drops
  mbedTLS (`nm app | grep mbedtls_ssl_handshake` → empty, ~1 MB smaller); an
  HTTPS app composes TLS and a real handshake to `example.com` succeeds; both
  runtimes.

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

### Phase 5 — Release + signing
- Add the `tls` stem to `release.yml` (build + sign into the platform manifest,
  embed via `TRUST_FEATURE_LIBS`), landing it in the `platform_domain` of
  `package.sig.gethull.composed` (platform-key attested, runtime §5c FATAL) — the
  same trust chain as the runtime/HTTP/WASM/image archives
  ([docs/composed_feature_signing.md](composed_feature_signing.md)).

## Risks + open questions

- **R1 — Keel ↔ mbedTLS build coupling.** Keel is *built with* mbedTLS
  (`$(KEEL_LIB): $(MBEDTLS_OBJS)`) and links its own TLS half. Splitting "Keel
  the event loop" (already base-free via async/poll — not linked) from "Keel the
  HTTP+TLS server/client" (in the HTTP feature) needs care that the HTTP feature
  archive pulls Keel + mbedTLS together and the base pulls neither. Verify Keel's
  `libkeel.a` dead-strips cleanly when only the client or only the server half is
  referenced (it already claims to). **This is the one real unknown** — Phase 3.
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
