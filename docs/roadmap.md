# Hull. Roadmap

## What's Built

### Core Platform
- Dual-runtime support: Lua 5.4 + QuickJS (ES2023), one active per app
- Keel HTTP server (epoll/kqueue/io_uring/poll) with route params and middleware
- SQLite with WAL mode, parameterized queries, prepared statement cache, performance PRAGMAs
- Request body reading, multipart/form-data, chunked transfer-encoding
- WebSocket support (text, binary, ping/pong, close)
- HTTP/2 support (h2c upgrade)

### Capabilities (C enforcement layer)
- **Crypto:** SHA-256 (with SHA-NI runtime dispatch on Linux/Cosmo arm64 + x86_64), SHA-512, SHA-1, incremental SHA-256 hasher, HMAC-SHA256, HMAC-SHA512/256, HMAC-SHA1 (HOTP/TOTP), PBKDF2, base64url, random bytes, password hash/verify, Ed25519 (sign/verify/keypair), XSalsa20+Poly1305 secretbox, Curve25519 box, **asymmetric verify (RS256/384/512, PS256, ES256/384)** via mbedTLS, x509 → SPKI PEM extraction. HMAC backend behind a vtable (`HlHmacBackend`).
- **Filesystem:** Sandboxed read/write/exists/delete/mmap with path traversal rejection, symlink escape prevention via realpath
- **Database:** Query/exec with parameterized binding, batch transactions, statement cache, user-defined functions (Lua/JS/WASM). SQL dialect helpers behind `HlDbBackend` vtable (SQLite in-base; PostgreSQL + MySQL/MariaDB + DuckDB shipped as composable `--with=` features).
- **Blob storage:** Content-addressed blob store (`hl_blob_store_*`). Per-blob hard-link layout (free dedup), streaming writers + readers. Powers `hull/attachment@1`, runtime bytecode + template caches, compute AOT cache, signed tools install. CLI: `hull cache list|prune|clear|verify`; per-app isolation via `HULL_CACHE_DIR`.
- **MIME sniffer:** Magic-bytes + extension fallback (`cap/mime.c`).
- **HTTP client:** Outbound HTTP/HTTPS with host allowlist enforcement (mbedTLS), connection pooling, redirect following, async
- **Environment:** Allowlist-enforced env var access
- **Time:** now, now_ms, clock, date, datetime
- **WASM compute:** Module load/call/stream, gas metering, instance pooling, persistent instances, shared data segments, SIMD128, Memory64, AOT
- **GPU compute:** wgpu-native dispatch/pipeline, persistent buffers/textures, fire-and-forget, async, buffer copy, shader loading
- **Image:** stb_image decode/encode (PNG/JPEG/BMP), raw pixel buffers
- **WebSocket:** Server endpoints + client connections, broadcast, per-connection data
- **SSE:** Server-Sent Events with chunked transfer encoding
- **SMTP:** Outbound email with TLS, template support
- **Audit:** Structured JSON capability logging (zero overhead when disabled)

### Standard Library (Lua + JS)
- `hull.json`. Canonical JSON encode/decode (sorted keys for deterministic signatures)
- `hull.web.cookie`. Cookie parsing and serialization with secure defaults (incl. CRLF/NUL/`;` rejection on path/domain/value)
- `hull.web.middleware.session`. Server-side SQLite-backed sessions with sliding + absolute (24h default) expiry, device fingerprinting, `login_handler`/`logout_handler` factories
- `hull.jwt`. JWT sign/verify/decode. HS256 + **RS256/384/512 + PS256 + ES256/384** (dispatched by alg with allowlist enforcement BEFORE key resolution to defeat alg-confusion). No `alg=none`; constant-time HMAC compare; numeric `exp`/`nbf` enforcement.
- **`hull.web.auth-flows`. End-to-end auth flows: registration / email verification / login / password reset / magic link / email change / optional TOTP 2FA. HMAC-signed envelope tokens, per-recipient email-storm rate limit, `public_origin` / `trusted_hosts` URL-origin gate. 13 audit rounds.**
- **`hull.web.middleware.totp`. RFC 6238 TOTP. Dual-row enrollment, multi-key at-rest encryption with lazy + batch rekey, per-user + opt-in per-IP brute-force lockout, auto-daily pending-row prune.**
- **`hull.web.middleware.oauth`. OIDC Authorization Code + PKCE. Google + Microsoft Entra presets (Microsoft `common`/`organizations`/`consumers` auto-pattern). HMAC-signed state cookie binds (provider, state, nonce, PKCE verifier, return_to). `on_login` / `on_logout` callbacks.**
- **`hull.web.middleware.audit-log`. Append-only sign-in / auth-event log with per-device fingerprint (HMAC-salted UA + IP-prefix). `is_new_device(user_id, req)`, `list_devices(user_id)`, auto-daily cleanup, `cleanup_status() -> "scheduled" | "external" | "missing"`, paged + mutex-guarded `recompute_fingerprints()` for salt rotation.**
- **`hull.web.auth-health`. Probes for session / audit-log / pwned / TOTP / RBAC. `auth_health.check({include_counts = false})`. `auth_health.routes(app, {auth_check})` mounts `/admin/auth-status` behind a required gate (strict-true admit). Backs `hull agent auth-status`.**
- **`hull.web.pwned`. HIBP k-anonymity check + 80KB embedded SecLists-10K offline blocklist. Single-flight cache, fail-open on outage with one-shot warn.**
- **`hull.qrcode`. Pure Lua/JS QR Code generator (ISO/IEC 18004). SVG output with color allowlist on `opts.dark` / `opts.light`.**
- **`hull.attachment`. File-attachment store backed by `hull/blob@1`. Multipart-part ingestion, content-addressed dedup, refcount GC, paired `hull/web/attachment-serve@1` for Content-Type + ETag + Range serving.**
- **`hull.blob`. Streaming content-addressed blob storage (writer / reader / hard-link layout). Powers attachments, runtime caches, signed tools install.**
- **`hull.mime`. MIME type sniffer (magic-bytes + extension fallback).**
- `hull.web.flash`. Session-backed one-shot user notifications + HTMX `HX-Trigger: {flash:...}` events
- `hull.web.pagination`. Offset-based pagination with windowed-links nav structure
- `hull.web.middleware.csrf`. Stateless CSRF tokens via HMAC-SHA256, per-form-pair cap, body-size cap
- `hull.web.middleware.auth`. Authentication middleware factories (session auth, JWT Bearer auth)
- `hull.web.middleware.logger`. Request logging with logfmt output and auto-assigned request IDs
- `hull.web.middleware.transaction`. Wraps handlers in SQLite BEGIN IMMEDIATE..COMMIT
- `hull.web.middleware.idempotency`. Idempotency-Key middleware with response caching, header allowlist+denylist, HTML response replay
- `hull.web.middleware.outbox`. Transactional outbox for reliable webhook/HTTP delivery with exponential backoff
- `hull.web.middleware.inbox`. Inbox deduplication for incoming events/webhooks
- `hull.web.middleware.csp`. Content-Security-Policy middleware with per-request nonce (htmx / strict profiles)
- `hull.validate`. Declarative input validation with schema rules
- `hull.web.form`. URL-encoded form body parsing
- `hull.i18n`. Internationalization with locale detection, message bundles, formatting helpers
- `hull.template`. Compile-once render-many HTML template engine with inheritance, includes, filters, auto-escaping
- `hull.csv`. CSV parse/encode (RFC 4180)
- `hull.search`. Full-text search (SQLite FTS5)
- `hull.web.middleware.rbac`. Role-based access control
- `hull.web.middleware.cors`. CORS headers + preflight handling
- `hull.web.middleware.ratelimit`. In-memory rate limiting with configurable windows
- `hull.web.htmx`. HTMX server-side helpers (fragment vs page render, HX-Trigger, csrf.refresh)
- Static file serving. Convention-based (`static/` → `/static/*`), MIME detection, ETag/304, embedded in builds, zero-copy sendfile in dev

### Background Work
- `app.every(ms, fn)`. Repeating interval timers with full async support (sleep, HTTP fetch, DB)
- `app.daily("HH:MM", fn, opts)`. Daily wall-clock timers (UTC or local)
- Self-cancellation via `return false`, error-resilient (logs + reschedules), one-invocation-at-a-time guard
- Detached async mode. Timer callbacks use `kl_timer_add` instead of `kl_async_suspend` for connectionless operation

### Build & Deployment
- `hull build`. Compile Lua/JS apps into standalone binaries (auto-AOT for WASM, embeds all assets)
- `hull new`. Project scaffolding with example routes and tests
- `hull init [--profile htmx]`. Initialize a project in-place; `--profile htmx` scaffolds a full HTMX + Pico app
- `hull dev`. Development server with hot reload
- `hull test`. In-process test runner (no TCP, memory SQLite, both runtimes)
- `hull deploy` (deployment config generator (Dockerfile, systemd, fly.toml)) manifest-aware
- `hull eject`. Export to standalone Makefile project
- `hull inspect`. Display capabilities and signature status (works on built JS apps too post-v0.3.0)
- `hull verify`. Dual-layer Ed25519 signature verification
- `hull verify-self`. One-command running-binary verification (manifest + signature + SHA-256 of running binary)
- `hull verify-release`. Verify an Ed25519 release-manifest signature (offline)
- `hull sign-release`. Sign a release manifest (release authority only)
- `hull keygen`. Ed25519 keypair generation
- `hull sign-platform`. Sign platform libraries with per-arch hashes
- `hull manifest`. Extract and print manifest as JSON (works on built JS apps too post-v0.3.0)
- `hull migrate`. SQL migration runner (auto-run on startup, embedded in builds)
- `hull migrate new` / `hull migrate status`. Migration scaffolding and status
- `hull sbom`. SBOM output in four formats (human / JSON / CycloneDX 1.5 / SPDX 2.3), includes binary SHA-256, published as signed release artifacts
- `hull modules available|list|explain`. Module-registry introspection (grouped: Intrinsic / Core / Web / Web middleware)
- `hull cache list|prune|clear|verify`. Runtime cache management (Lua/JS bytecode, Lua/JS template, compute AOT, tools store)
- `hull tools install|list|uninstall`. Side-loaded optional tools + toolchain bundles (`wamrc`, `zig`, musl static floors, `cosmocc`) routed through the signed `blob_store` / release trust chain
- `hull update [--check]`. Self-update via signed release manifest (Ed25519 + Sigstore/Rekor + SLSA verification)
- `hull doctor [--json]`. Environment + distribution readiness check
- `hull agent`. ~25 machine-readable subcommands (routes, db schema/query, request, status, errors, test, context, migrate, deploy, sbom, tools, overview, modules, auth-status, etc.)
- `hull agent auth-status`. Health probe for the auth stack (session / audit-log / pwned / TOTP / RBAC)
- `hull mcp`. Stdio MCP server wrapping agent core
- `hull compute`. WASM module management (new / build / test / check / refresh-header)
- `hull check`. Full validation (clean + ASan + test + e2e)
- Multi-arch Cosmopolitan APE builds (`make platform-cosmo`)
- Self-build reproducibility chain (hull → hull2 → hull3)
- macOS + Linux reproducible-build CI gate

### Security
- Kernel sandbox: pledge/unveil on Linux (seccomp-bpf + landlock) and Cosmopolitan
- Manifest-driven capability declaration and enforcement
- **Three-tier trust chain end-to-end verifiable three independent ways:** (a) Ed25519 chain (gethull keys, platform + app + release layers), (b) Sigstore + Rekor transparency log per release (`cosign verify-blob`), (c) SLSA build-provenance attestation per binary (`gh attestation verify`)
- Platform canary with integrity hash
- Browser verifier (offline, zero-dependency HTML tool)
- Runtime startup verification (`--verify-sig`)
- Shell-free tool mode with compiler allowlist
- Lua sandbox (removed io/os/load, memory limit, custom allocator, instruction-count gas)
- QuickJS sandbox (removed eval/std/os, memory limit, instruction-count gas metering)
- **Auth stack hardened across 13 iterative audit rounds.** Host-header injection closed, host:port + IPv6 + comma-XFF normalized for the URL-origin gate, per-recipient email-storm rate limit, TOTP per-user + per-IP lockout (XFF opt-in), OIDC state cookie HMAC-bound to (provider, state, nonce, PKCE verifier, return_to), audit-log fingerprint salt mandatory, strict-allowlist user sanitization with optional callback.

### CI/CD
- Linux, macOS, Cosmopolitan APE builds
- ASan + UBSan, MSan + UBSan sanitizer runs
- Static analysis (scan-build + cppcheck)
- Code coverage
- E2E tests for all examples in both runtimes + template engine tests + stdlib middleware tests + deploy config tests
- Sandbox violation tests (Linux + Cosmo)
- Benchmarks (Lua vs QuickJS, DB vs non-DB routes, WASM interpreter vs AOT, GPU vs CPU)

## Roadmap

### Extension taxonomy and near-term targets

Every new capability ships as one of four units. Pick the wrong one and you
either bloat the base or build a signed archive where a Lua module would do.
The full decision procedure lives in `CLAUDE.md` ("Extension taxonomy: feature
vs flavor vs tool vs stdlib") and the rationale in
[features_and_flavors.md](features_and_flavors.md); the short version:

| Unit | Rule of thumb | Ships as | Examples |
|------|---------------|----------|----------|
| **stdlib** | pure Lua/JS over caps we already ship; no new C, no new authority | always in base | jwt, csrf, template, an S3/SigV4 client |
| **feature** | large optional C subsystem / new authority, **off by default** (additive) | `libhull_feature-<name>.a`, `--with=` | duckdb, postgres, mysql, gpu, tui |
| **flavor** | preset that validates the app against a slimmer cap set; the base already composes (subtractive) | build.lua preset on the default base (no per-flavor lib since Phase 4.3), `--flavor=` | pure-compute |
| **tool** | a companion **program** (or toolchain bundle) Hull spawns at build time (never linked) | `hull-<tool>-<platform>` / `.tar` bundle, `hull tools install` | wamrc, zig, musl static floors, cosmocc |

**First yes wins:** separate program → tool; buildable on existing caps → stdlib;
new vendored C / new authority off by default → feature; turning a default off →
flavor.

**Near-term candidates, classified:**

| Candidate | Class | Status / rationale |
|-----------|-------|--------------------|
| **Redis / Valkey client** | **feature** | Highest-leverage gap: shared cache / session / rate-limit / pub-sub state across instances. Pure-C RESP3 wire client (the pg/mysql playbook: codec + shared `tls_client` + tiny archive), off by default, new authority. NOT a `HlDbBackend` (RESP is not SQL) - it opens the first non-SQL connection-feature seam (`hl_kv_feature_backend` + a `hull/kv` module). Full spec: [roadmap_next.md](roadmap_next.md) "Redis / Valkey connection feature." |
| **wasm-opt (Binaryen)** | **tool** | Build-time WASM→WASM optimizer; heavy LLVM/C++, version-coupled - mirrors `wamrc` exactly. Value is narrower than wamrc (Hull already runs `clang -O2 -flto`, and AOT re-lowers through LLVM); real payoff is the interpreter-fallback path and non-clang toolchains (Rust / AssemblyScript / TinyGo). |
| **Image codecs (stb_image)** | **feature** (auto-composed, like runtime/http/wasm) | ✅ **SHIPPED (#138).** `cap/image.c` is on by default for web apps, but the native base is image-less and composes `libhull_feature-image.a` + the per-runtime bridge back only when the app declares `hull/image` (the `needs_image` gate), so a compute app drops ~146 KB of stb. Auto-composed + embedded in `hull` (never `hull feature install`); the subtractive `HL_ENABLE_IMAGE=0` knob still drops it entirely. See [image_feature.md](image_feature.md). |
| **Drop the unused Lua/JS runtime** | **feature** (runtime-less base + `lua`/`js`, auto-composed) | ✅ **SHIPPED (#113, 2026-07-26).** Slim a single-runtime app to one interpreter. Delivered as composable runtime *features* on a runtime-agnostic base (the GPU-backend pattern): a runtime archive composes with any flavor, is auto-composed from the entry extension, and is embedded in `hull` (never `hull feature install`). ~0.5-1 MB. The M+N-composes-M×N orthogonality holds for **reduced** flavors too: **#114 (HTTP as a composable feature)** split the runtime's web bindings behind a weak seam, so `--flavor=pure-compute` × runtime and `--with=tui` × runtime now compose cleanly (a reduced base simply omits the web bindings; proven by `tests/e2e_build_flavor.sh` + `tests/e2e_feature_tui.sh`). WASM slim is the same model (**#118**, shipped), gated on a module + UDF two-signal check. Full spec: [roadmap_next.md](roadmap_next.md) "Runtimes as composable features." |
| **`hull/query` builder + `hull/query/schema`** | **stdlib** | Designed + requested; the C keystone already shipped (`HlDbDialect` descriptor + `conn.dialect`, commit `234c8dc`). A thin, injection-safe compile-to-`(sql, params)` layer over `conn.query` (values bound, identifiers dialect-quoted), NOT an ORM. Resolves 5 DB-API-review findings (portable upsert / `RETURNING` / async terminals / raw composition / dialect leakage). Cheaper than any feature (no C, no release pipeline; offline `to_sql()` matrix tests). See [roadmap_next.md](roadmap_next.md) "Out of scope" note under the backend-onboarding checklist. |
| **Object storage / S3, most integration clients** | **stdlib** | Buildable on `http.fetch` + `crypto` (SigV4). No C archive. The canonical "reach for stdlib before a feature" case. |

**WASM featurified (#118, docs/wasm_feature.md).** The WASM *interpreter* (WAMR,
~256 KB) was the last "kept core" candidate; it is now an embedded, auto-composed
feature (like the runtimes / HTTP core) — the native base is compute-less and a
compute-free app links zero WAMR. It stays *embedded + auto-composed*, not
`hull feature install`, because compute is a pillar and the `db.udf` coupling
needs the two-signal `needs_wasm` gate rather than a manual `--with`. Its heavy
half, the AOT *compiler* `wamrc`, remains correctly a tool.

**Deliberately kept core (never a feature):** `crypto` / SHA / HMAC (the trust
substrate under signatures, sessions, JWT), and the default SQLite backend.

### Next. Distribution: hull.com Downloadable Tool

The goal: a single downloadable Cosmopolitan APE binary that users install and immediately use to create, build, test, run, and deploy Hull applications. Zero dependencies.

#### Current State

The core pipeline works end-to-end:

```
hull new myapp          ✅  scaffold project
hull dev app.lua        ✅  hot-reload dev server
hull test myapp/        ✅  in-process test runner
hull build myapp/       ✅  compile to standalone binary
hull deploy dockerfile  ✅  deployment config generator
hull keygen             ✅  Ed25519 keypair
hull verify             ✅  signature verification
make CC=cosmocc         ✅  APE binary builds
EMBED_PLATFORM=cosmo    ✅  platform archives embedded in hull binary
make self-build         ✅  reproducible build chain verified
```

The missing piece at the time: `hull build` shelled out to `cc` to compile generated C code, so users needed gcc/clang/cosmocc installed. The phases below address that gap and the surrounding distribution story. **Since closed:** `hull build` is now **compiler-free by default** (it emits `app_registry.o` directly via the object emitter and links, no C compiler needed), and the toolchain-free axis side-loads a linker/toolchain on demand (`hull tools install zig` / `cosmocc` / musl static floors), so a stock install builds an app with zero pre-installed toolchain - see [docs/compiler_free_build.md](compiler_free_build.md) and [docs/toolchain_free_build.md](toolchain_free_build.md).

#### Phase D1: Version + Release Pipeline. **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| `hull version` command | **Done** | Pure C; `--json` for machine-readable output (version/runtime/platform/build) |
| Git tag → version string | **Done** | `git describe --tags` at build time → `HL_VERSION` define; `VERSION` file override |
| GitHub Actions release workflow | **Done** | `.github/workflows/release.yml`, triggered on `v*` tag push |
| Release artifacts | **Done** | `hull-cosmo` (APE), `hull-linux-x86_64`, `hull-darwin-arm64`, `hull.sha256` |
| Native release artifacts | **Done** | Linux x86_64 + macOS arm64 native; cosmo APE covers Linux arm64, macOS x86_64, BSDs, Windows |

#### Phase D2: Install Script + First-Run Experience. **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| Install script (`curl -fsSL .../install.sh \| sh`) | **Done** | POSIX, ~250 LOC: detect OS/arch, fetch latest release, SHA-256 verify, install to `~/.local/bin/hull` |
| `hull init` (in-place project init) | **Done** | Like `git init`; idempotent; auto-detects existing runtime; Lua tool module |
| First-run welcome + doctor | **Done** | `hull doctor` reports platform embed, compiler availability, CA bundle status; `--json` mode |
| Shell completions | **Done** | bash, zsh, fish; covers every subcommand and flag in `completions/` |

#### Phase D3: Zero-Dependency Builds (Embedded TCC). **Done, later superseded**

> **Superseded.** The embedded-TinyCC backend has since been fully retired.
> `hull build` is now **compiler-free by default**: it emits `app_registry.o`
> directly and links, with the system compiler as the `--with=`/cosmo fallback.
> See [docs/compiler_free_build.md](compiler_free_build.md). The table below is
> the historical record of the (now-removed) embedded-TCC phase.

| Feature | Status | Notes |
|---------|--------|-------|
| Vendor tcc | **Done (removed)** | Was a `vendor/tcc` submodule (mob branch), built by Makefile, embedded via xxd into `libhull_platform.a`; retired |
| `hull build` auto-selects compiler | **Done (superseded)** | `HlCompilerVtable`: was TCC backend (compile) + system cc/gcc/clang (link); now the emit path is default, system compiler is the fallback |
| `hull build --compiler=tcc\|system\|<path>` | **Done (superseded)** | `--compiler=tcc` now just names a user's own `tcc` on `$PATH` as a plain system compiler; tcc is no longer Hull-provided |
| Linux native + cosmo coverage | **Done (removed)** | Was verified end-to-end on Linux CI in `e2e_tcc.sh`; that suite is gone |
| `hull toolchain install` | **Superseded** | Toolchains ship as side-loaded tools now: `hull tools install cosmocc` (trimmed cosmocc + busybox bundle, v0.10.0) / `zig` / musl static floors, all on the signed `hull tools` trust chain. `make fetch-cosmocc` remains for from-source builds. |

#### Phase D4: Embedded CA Bundle. **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| Embed Mozilla CA bundle | **Done** | `vendor/cacert/cacert.pem` (curl.se, SHA-256 verified); xxd'd into `libhull_platform.a` so built apps inherit it |
| Auto-detect system CA store | **Done** | Resolution order: `--skip-ca-bundle` → `--ca-bundle PATH` → system paths → embedded → fail |
| `hull <app> --ca-bundle=PATH` | **Done** | Custom override via runtime flag |
| CA bundle update mechanism | **Done** | `make fetch-ca-bundle` pulls + verifies; `hull doctor` reports the embedded bundle's update date |
| New Keel API: `kl_tls_mbedtls_client_ctx_create_from_buf` | **Done** | Released in Keel v1.1.0. TLS client from in-memory PEM/DER bundle |
| Pledge + unveil for outbound HTTPS | **Done** | Linux: `dns netlink unix` promises + unveiled `/etc/resolv.conf` (and family); polyfill's `kPledgeDns` extended with `sendmmsg`/`recvmmsg`/`sendmsg`/`recvmsg` for glibc 2.32+ |

#### Phase D5: Self-Update. **Done** (signature verification deferred)

| Feature | Status | Notes |
|---------|--------|-------|
| `hull update` | **Done** | Pure C in `src/hull/commands/update.c` (~330 LOC). Keel client + embedded CA bundle + mbedTLS SHA-256 + atomic `rename(2)` |
| `hull update --check` | **Done** | Compare current `HL_VERSION` with latest release tag, no install |
| `hull update --channel=stable\|beta` | Deferred | Flag removed pre-v0.1.0; will be added back when a real beta channel exists |
| `hull update --repo=ORG/NAME` | **Done** | Override the default repo (`artalis-io/hull`); used in CI tests against `cli/cli` |
| Signature verification on update | Deferred | Releases are not yet Ed25519-signed; `hull keygen` + `hull verify` already exist for when they are |

#### Phase D6: hull.com + Documentation Site

| Feature | Status | Effort | Notes |
|---------|--------|--------|-------|
| Landing page | Planned | External | Single-page site with install command, what/why/how |
| Documentation site | Planned | External | Generated from CLAUDE.md + AGENTS.md + examples |
| `hull.com/install` endpoint | Planned | 1h | Serves install script with latest version URL |
| `hull.com/download` endpoint | Planned | 1h | Redirects to latest GitHub release asset |
| Package manager entries | Planned | 4h | `brew install hull`, AUR package, Scoop manifest |

#### Distribution Architecture

```
Developer machine                           hull.com / GitHub Releases
─────────────────                           ────────────────────────────

$ curl hull.com/install | sh     ──────►   install.sh (detect OS/arch)
                                                │
                                                ▼
                                           hull (Cosmo APE binary)
                                             ├── libhull_platform.a (x86_64 + aarch64, embedded)
                                             ├── obj emitter (compiler-free build; no bundled compiler)
                                             ├── CA bundle (embedded)
                                             ├── Lua 5.4 + QuickJS (in platform lib)
                                             ├── SQLite + mbedTLS (in platform lib)
                                             └── hull.sig (Ed25519 signature)

$ hull new myapp                ──────►   myapp/
$ hull dev myapp/app.lua        ──────►   http://localhost:3000 (hot reload)
$ hull build myapp/             ──────►   myapp/build/app (standalone binary)
$ hull deploy dockerfile myapp/ ──────►   Dockerfile + .dockerignore
```

One download. Zero dependencies. Full lifecycle.

### Standard Library (Complete)

| Feature | Status | Notes |
|---------|--------|-------|
| CORS middleware | **Done** | `hull.web.middleware.cors`. Configurable origins, preflight handling |
| Template engine (`{{ }}` HTML templates) | **Done** | `hull.template`. Inheritance, includes, filters, compiled & cached |
| Input validation (schema-based) | **Done** | `hull.validate`. Declarative field validation |
| Rate limiting middleware | **Done** | `hull.web.middleware.ratelimit`. Sliding window, per-key |
| Static file serving (`/static/*` convention) | **Done** | MIME detection, ETag/304, embedded in builds, zero-copy sendfile in dev |
| i18n (locale detection + translations) | **Done** | `hull.i18n`. Locale detection, message bundles, format helpers |
| Request logging middleware | **Done** | `hull.web.middleware.logger`. Logfmt output, request IDs |
| Transaction middleware | **Done** | `hull.web.middleware.transaction`. BEGIN IMMEDIATE..COMMIT wrappers |
| Idempotency-Key middleware | **Done** | `hull.web.middleware.idempotency`. Response caching, fingerprinting, 409 on mismatch |
| Transactional outbox | **Done** | `hull.web.middleware.outbox`. Reliable delivery with exponential backoff |
| Inbox deduplication | **Done** | `hull.web.middleware.inbox`. Incoming event dedup with TTL |
| CSV encode/decode (RFC 4180) | **Done** | `hull.csv`. RFC 4180 parse/encode |
| FTS5 search wrapper | **Done** | `hull.search`. Full-text search backed by SQLite FTS5 |
| RBAC (role-based access control) | **Done** | `hull.web.middleware.rbac`. Role/permission middleware |
| Email (SMTP / API) | **Done** | Outbound SMTP via C capability + stdlib |
| Health + readiness endpoints | **Done** | `hull.web.middleware.health`. Liveness + readiness with custom checks |
| ETag response helpers | **Done** | `hull.web.middleware.etag`. Compute + compare + 304 |
| Deployment config generator | **Done** | `hull deploy`. Dockerfile, systemd, fly.toml from manifest |
| License key system | Planned | Ed25519 offline verification for commercial distribution |

### Package & Module System

First-party stdlib modules are declared by the app and gated by the runtime.
"Nothing exists unless declared". Language runtimes provide pure computation;
access to side-effect modules (crypto, db, http, fs, …) is opt-in via the
manifest's `modules = {...}` array of canonical specs
(`"vendor/name@version"`, e.g. `"hull/crypto@1"`). Imports use the standard
`require()` / `import` forms with any local binding name. The foundation
(phases 1–2c plus header-dep tracking) is shipped; what follows is the rest
of the package-system roadmap in implementation order.

This is *not* npm/pip/cargo. It is capability-aware dependency declaration
for sealed Hull apps. No remote registry, no runtime install, no dynamic
discovery, no transitive third-party deps. Runtime code cannot install,
fetch, or load packages; the resolved module set is fixed at build/startup.

#### Foundation. **Done** (phases 1, 2a, 2b, 2c)

| Feature | Status | Notes |
|---------|--------|-------|
| Canonical module registry (`HlModuleSpec` table) | **Done** | 40 first-party modules sorted by name; `api_major`, `intrinsic`, `pure`, `required_caps`, `deps`. O(log n) binary search. |
| `HlManifest.modules` declaration + extractor | **Done** | Lua + JS strict `modules = { name = "ver" }` parsing; distinguishes "missing key" from "declared empty". |
| Resolver | **Done** | Validates unknown names, version mismatches, missing manifest caps (fs/hosts/env), missing compile-time subsystems (DB/WASM/GPU), undeclared explicit deps. Auto-seeds intrinsic core (`app`, `log`, `json`). |
| Lua `require()` gating | **Done** | Custom require checks the resolved set; bridges to native modules via `LUA_LOADED_TABLE`. |
| JS `import` gating | **Done** | Stdlib `.js` modules gated in the loader hook; native C modules gated inside each `js_*_module_init` callback via `hl_js_check_module_declared`. |
| WASM compute/GPU gating | **Done** | Cache revocation when `modules.compute` / `modules.gpu` not declared. |
| Globals removed for declarable modules (Lua) | **Done** | Only `app`, `log`, and the `hull` namespace remain as globals. Everything else is `require("hull.X")`. |
| `image` converted to `hull:image` ES module | **Done** | No more `globalThis.image`; flows through the same gate as every other native module. |
| Stdlib middleware migration | **Done** | All 14 stdlib `.lua` files use `require` for their dependencies. |
| Example app migration | **Done** | 23 Lua + 21 JS examples migrated to `modules = {...}` + explicit `require`/`import`. |
| Test harness compatibility | **Done** | `test_lua.c` / `test_js.c` install legacy globals once at init so inline test snippets keep working. |
| Header-dep tracking in Makefile | **Done** | `-MMD -MP` + `-include build/**/*.d`. Touching a header invalidates only the right `.o` files. (Was previously the source of mid-phase SIGSEGVs.) |

#### Visibility & UX. **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| `hull modules list [app_dir]` | **Done** | Declared modules from the app's manifest as JSON. |
| `hull modules available [--json]` | **Done** | Full registry dump (40 entries) with deps, required caps, intrinsic flag. |
| `hull modules explain <name>` | **Done** | Pretty-print one spec. |
| `hull modules analyze [app_dir]` | **Done** | Static `require`/`import` scanner. Flags undeclared imports and unused declarations. Soft-fails on app-load errors so app-level filesystem fallbacks (e.g. `require("./locales/en.json")`) don't break the scan. |
| `hull check` integration | **Done** | Runs `modules list` + `modules analyze` before tests + verify. Manifest errors surface at check-time rather than first server startup. |
| `hull doctor` module subsystems section | **Done** | Doctor reports which `HL_ENABLE_*` subsystems are linked in. |
| `hull agent modules [app_dir]` | **Done** | Emits `{declared, intrinsic, build_caps, registry_count}` JSON. `hull agent manifest` also includes a `modules` block. |
| Record resolved set in `package.sig` | **Done** | `modules_resolved` field included in the canonical reconstruction in `verify.lua`. Covered by the existing Ed25519 app-layer signature. "This binary was built with these modules" is now a signed, tamper-evident claim. |
| `hull init` / `hull new` generate `modules = {...}` | **Done** | Templates produce array-form module declarations matching what they actually use. |
| Documentation (`docs/security.md` §5b + `CLAUDE.md` + `README.md` + `AGENTS.md`) | **Done** | Module Declaration sections added to all four; failure-mode table, registry overview, CLI/agent pointers. |

#### Correctness coverage. **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| Static import/require analyzer | **Done** | `hull modules analyze`. Lua + JS state-machine scanners that skip comments and string literals; auto-seeds intrinsic core; integrated into `hull check`. |
| Gate non-server entry points | **Done** | Every consumer that runs user app code now opts in via `HlAppContextOpts.gate_modules = 1`: `hull test` (`test.c`), `hull agent` warm context (`agent.c`), all 7 `hull mcp` context openings (`mcp.c`). `hull dev`/serve is gated through `main.c::hl_serve_wire_caps`. Verified by `make e2e-examples` (280/280 passing under the gated test runner). |

#### Error-message UX. **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| "Did you mean …" suggestions on gate errors | **Done** | `hl_module_registry_suggest()` (length-scaled Levenshtein, threshold 1/2/3 for ≤4/5–8/≥9-char inputs) wired into three error sites: resolver `unknown module 'X' in app.manifest.modules`, Lua `module not found: hull.X`, JS `unknown hull module: hull:X`. Each emits the suggested canonical name in the syntax the caller used. |

At this point the Package & Module System is feature-complete for v1; everything below is explicitly deferred.

#### Future. Out of v1 (explicitly deferred)

| Feature | Status | Notes |
|---------|--------|-------|
| Third-party packages (`acme/widgets@1`) | Deferred | Vendor-prefix design + fetching + verification + trust model. The registry already supports the `vendor/name` form, so adding a second namespace is a small step; the broader infrastructure is a separate project. |
| Bundle stripping for `hull build` | Deferred | Physically remove undeclared stdlib bytes. Smaller binaries + reduced ROP surface, but adds a per-build compile step and complicates the platform-embed path. Runtime gating is the boundary in v1. |
| Per-opcode WASM `host_call` gating | Deferred | Manifest design supports it (`required_caps` bitmask has room); current gate admits all opcodes together when `hull/compute@1` is declared. |
| CBOR / additional pure codecs in intrinsic core | Deferred | Pure-stdlib expansion; orthogonal to the module system. |
| **Demote `hull/log` and `hull/json` to declared modules** | **Done** | The intrinsic core is now `hull/app` alone. Apps that call `log.X` or `json.X` directly must declare `"hull/log@1"` and/or `"hull/json@1"` in `manifest.modules`. The `res:json(...)` response helper and the runtime's internal JSON marshalling continue to work without any declaration. They bypass user-visible imports at the C layer via a registry-stashed decoder. |
| Lockfile for resolved-set pinning | Deferred | The "resolved set in `manifest.bin`" item above covers tamper detection. A separate lockfile is overkill for v1's no-third-party model. |
| `hull add <package>` install command | Deferred | The existing "Module/package ecosystem" row in *Future. Advanced Features* covers this. Out of scope until third-party packages exist. |

### CLI / Compute-Only Mode

`HL_ENABLE_HTTP=0` builds Hull without Keel, routing, or middleware.
producing a pure CLI / compute runtime with the full module surface
(`db`, `crypto`, `compute`, `gpu`, `image`, `fs`, etc.). Apps register
`app.main(fn)` instead of `app.get/post/…`; lifecycle is *load → resolve
modules → migrate → main → exit-with-rc*, mirroring `int main()` /
`def main()` from any other language. The default hull binary
(`HL_ENABLE_HTTP=1`) runs both modes, so a CLI app written today works
on any future stripped-down distribution.

Full design + lifecycle + phased implementation: [docs/cli_mode.md](cli_mode.md).

| # | Feature | Status | Effort | Notes |
|---|---------|--------|--------|-------|
| 1   | `app.main(fn)` registration + lifecycle | **Done** |. | Bindings, mode detection, conflict error, return-value → exit code coercion. Works on HTTP=1 builds. |
| 1.5 | Event-loop integration for async-in-main | **Done** |. | Lua coroutine wrap + cli_main_co detection in async resume; JS Promise + `.then` callback; both call `kl_server_stop` on terminal state. |
| 2   | `hull run` command + scaffolding | **Done** |. | `run` token stripped in `hull_main` and falls through to serve. `--` separator for app args. `hull new --cli` + `hull init` mode detection (auto-detected from `app.main(` presence). |
| 3a  | HTTP foundation (no behavior change) | **Done** |. | `HL_MOD_CAP_HTTP` bit, registry markings on http/ws/server/smtp/middleware/*, resolver + cap_label updates, `hull doctor` row + `--json` subsystems object, Makefile flag, `hull_serve` extracted from `main.c` into `src/hull/serve.c`. |
| 3b  | `HL_ENABLE_HTTP=0` build flag | **Done** |. | `make HL_ENABLE_HTTP=0` produces a working CLI-only binary. Drops HTTP cap files (http/http_async/ws/body/smtp/static), route/middleware/ws/sse/timers + runtime support files (routes/dispatch/sse/ws/timers/bindings), the agent_api in-process server, stdlib middleware embedding, `hull dev`. Replaces `serve.c` with `serve_cli.c` (load → resolve modules → sandbox → migrate → app.main → exit). At this phase Keel stayed linked (its async primitives backed `hull.sleep` / compute.async / etc); Phase 3d below routed those through `HlAsyncBackend` and fully unlinks Keel. Post-3d, `HL_ENABLE_HTTP=0` on arm64 Darwin measures ~5.8 MB vs ~6.5 MB default (~767 KB smaller, no libkeel.a). |
| 3c  | Sandbox narrowing | **Done** |. | `HlSandboxPolicy.network_inbound`: defaults to 1; serve.c sets to 0 when `rt->vt->has_main(rt)` is true. Pledge's `inet` promise is dropped entirely on CLI apps with no outbound either; macOS Seatbelt's `network-inbound`/`network-bind` rules become conditional on the same. |
| 3d  | Async + Net backend vtables | **Done** |. | Shipped across Phases 3d-1..3d-5 (+ two c-audit passes). **Defined `HlAsyncBackend` + `HlNetBackend` vtables** (mirror of `HlDbBackend`; both `const` -> `.rodata`). Keel is the default impl (`async/keel.c` + `net/keel.c`); a poll(2)/pthread async backend (`async/poll.c`) ships for `HL_ENABLE_HTTP_ANY=0` builds, **fully unlinking Keel** (`KEEL_LIB` empty) and enabling async-in-main on CLI builds. Consumers reach async/suspend/complete + net only through `hl_async_backend()` / `hl_net_backend()`. Future backends (libuv, io_uring, sandboxed mini-server) slot into the same interface. Full design: [docs/backend_vtables.md](backend_vtables.md). |
| 4   | Polish & documentation | **Mostly done** |. | `hull agent manifest` mode field, `examples/hello_cli/`, `tests/e2e_cli.sh`, AGENTS/CLAUDE doc markings for HTTP-build modules. Remaining: `hull build` mode validation (needs Phase 3b), more sample CLI apps. |
| 5   | `hull build --flavor=full\|pure-compute` (+ `--flavor=auto`) | **Done** (v0.5.0) |. | Builds a flavored app binary; `--flavor=auto` infers the minimal flavor from the app's declared modules. Validates the app manifest against the TARGET flavor caps: a module needing a dropped subsystem is rejected at build time rather than link time. (Shipped with `server-only`/`client-only` too; both later removed, #114.) |
| 6   | Signed published per-flavor platform libs + `hull flavor install <flavor>` / `hull flavor list` | **Done** (v0.5.0 native, v0.6.0 cosmo) |. | Fetches + Ed25519/SHA-256-verifies the per-flavor lib(s) into `~/.hull/platform/`; `hull build --flavor` uses the cache. Native `libhull_platform-<flavor>-<arch>.a` + cosmo dual-arch `libhull_platform-<flavor>.{x86_64,aarch64}-cosmo.a`, all covered by the signed `hull.sha256`. |
| 7   | Build-time platform-lib re-verify | **Done** |. | Closes the install->build TOCTOU for flavored builds. `hull flavor install` caches the signed manifest (`hull.sha256` + `.sig`) in `~/.hull/platform/`; `hull build --flavor` re-verifies any cache-sourced lib offline before linking (`hl_release_io_verify_local_asset` / `tool.platform_verify`): the manifest signature is re-checked against the EMBEDDED release pubkey (not the writable cache dir), then the lib's SHA-256 is matched to the signed manifest. Tampered lib or swapped/absent manifest fails the build with a reinstall hint; locally-built libs are trusted as-is. See [docs/build_flavors.md](build_flavors.md). |
| 8   | **libhull: no-runtime embedding flavor** (L-1..L-5) | **Done** | . | `make libhull` -> `libhull.a`, the runtime-free core (no Lua/JS) a native C/Rust/Zig host links to drive the two-phase sandbox + capability layer + WASM/GPU + signed-artifact machinery via the stable `<hull/embed.h>` ABI. L-1 archive + `sandbox_tool.c` split; L-2 `hl_embed_*` ABI; L-3 sealed per-call `base_dir` (RO `sh_seal_arena`) + fail-closed seal ordering + fork/SIGSEGV death test; L-4 release-signed archive (native + dual-arch cosmo) + scoped SBOM (`hull sbom --subject=libhull`); L-5 Rust + Zig reference embedders (`examples/embed_{c,rust,zig}`) with CI jobs. Full design + use-cases + trust boundary: [docs/libhull_flavor.md](libhull_flavor.md). |

The **build-flavor epic is shipped** (phases 5 and 6 above): `hull build
--flavor` produces a flavored app binary against a signed, per-flavor
platform library installed via `hull flavor install`, with target-flavor
manifest validation at build time. Full design + the four flavor
definitions live in [docs/build_flavors.md](build_flavors.md).

### Agent Platform. AI-Native Development Tooling

Hull treats agentic coding environments (Claude Code, Codex, OpenCode, Cursor, Ollama-based harnesses) as first-class citizens. The agent platform provides machine-readable tooling, dynamic context management, and a structured feedback loop so AI agents can rapidly prototype, test, and deploy Hull applications.

**Architecture:**

```
┌──────────────────────────────────────────────────────────────┐
│                     Agent Environments                        │
│  Claude Code │ Codex │ OpenCode │ Cursor │ Ollama+harness    │
└──────┬───────┴───┬───┴────┬─────┴────┬───┴────┬──────────────┘
       │           │        │          │        │
       │      ┌────┴────┐   │    ┌─────┴─────┐  │
       │      │ MCP srv │   │    │.cursorrules│  │
       │      │ (stdio/ │   │    │ codex.md   │  │
       │      │  SSE)   │   │    └────────────┘  │
       │      └────┬────┘   │                    │
       │           │        │                    │
  ┌────┴───────────┴────────┴────────────────────┴──────┐
  │              hull agent <subcommand> --json          │  CLI layer
  └──────────────────────┬───────────────────────────────┘
                         │
  ┌──────────────────────┴───────────────────────────────┐
  │                 Agent Core (C library)                │
  │                                                      │
  │  context()    routes()     request()    render()      │
  │  db_schema()  db_query()   logs()       errors()     │
  │  test()       status()     scaffold()   build()      │
  │  migrate()    monitor()    manifest()                │
  └──────────────────────┬───────────────────────────────┘
                         │
  ┌──────────────────────┴───────────────────────────────┐
  │              Hull Runtime (existing)                  │
  │  hull dev │ hull test │ hull build │ SQLite │ Lua/JS  │
  └──────────────────────────────────────────────────────┘
```

Common agent core with dual interface: CLI JSON mode for frontier models, MCP server for mid-range local models. Zero logic duplication. Both call the same C functions.

#### Phase 1: Foundation (agent core + CLI). Done

| Feature | Status | Notes |
|---------|--------|-------|
| `hull agent status` | **Done** | Dev server state, PID, port, last reload result, uptime |
| `hull agent errors` | **Done** | Structured errors from `.hull/last_error.json` sidecar |
| `hull agent routes` | **Done** | List registered routes as JSON (method, pattern, middleware) |
| `hull agent request` | **Done** | HTTP request to dev server with JSON response |
| `hull agent db schema` | **Done** | Introspect current DB tables, columns, types, PKs |
| `hull agent db query` | **Done** | Read-only query on dev DB with JSON output |
| `hull agent test` | **Done** | Structured test results (passed, failed, failure details) |
| `hull dev --agent` | **Done** | Write structured errors/status to `.hull/` sidecar files |
| `AGENTS.md` | **Done** | Comprehensive agent development guide |

#### Phase 2: Context + Render. Done

Dynamic context system. `hull agent context` assembles task-relevant documentation on demand, sized for the model's context window.

| Feature | Status | Notes |
|---------|--------|-------|
| `hull agent context --task=T --level=L` | **Done** | 12 domains (auth, db, middleware, templates, routing, testing, build, deploy, search, i18n, webhooks, validation) × 3 levels |
| `stdlib/context/*.md` | **Done** | Per-domain knowledge files with `<!-- minimal -->` / `<!-- compact -->` / `<!-- full -->` markers |
| `hull agent migrate` | **Done** | Migration status as JSON |
| `hull agent deploy` | **Done** | Deployment readiness analysis as JSON |
| `hull render` | Planned | Offline template rendering without running server |
| `--model-size` auto-selection | Planned | Auto-select context level based on model size (7B→minimal, 70B→compact, frontier→full) |

Context levels:

| Level | Size | Target Models |
|-------|------|---------------|
| `minimal` | ~1K tokens | Small local (7–14B): API signatures, one-liner patterns |
| `compact` | ~4K tokens | Mid-range local (30–70B): signatures + patterns + gotchas + one example |
| `full` | ~12K tokens | Frontier (Claude, GPT-4): comprehensive with multiple examples, edge cases |

#### Phase 3: MCP Server + Agent Configs. Partial

| Feature | Status | Notes |
|---------|--------|-------|
| `hull mcp` | **Done** | stdio MCP server wrapping agent core, warm context (shared `HlAppContext`) |
| `hull mcp serve --sse` | Planned | SSE transport for network-accessible agents |
| `.cursorrules` | Planned | Cursor/Windsurf agent rules |
| `codex.md` | Planned | Codex-specific instructions |
| `.opencode.yml` | Planned | OpenCode config with MCP server reference |
| Updated `CLAUDE.md` | **Done** | Full API reference, agent commands, conventions |
| Updated `AGENTS.md` | **Done** | Agent development guide with hull agent commands, patterns, stdlib |

#### Phase 4: Lifecycle + Monitoring. Partial

| Feature | Status | Notes |
|---------|--------|-------|
| `hull agent migrate` | **Done** | Structured migration status as JSON |
| `hull agent deploy` | **Done** | Deployment readiness analysis as JSON |
| `hull agent scaffold` | Planned | Project scaffolding from templates with structured output |
| `hull agent build` | Planned | Structured build output (binary path, size, platform) |
| `/_hull/agent/*` endpoints | Planned | Opt-in diagnostic endpoints in deployed apps (health, schema, logs, errors, stats) |
| `hull agent monitor` | Planned | Query deployed app diagnostics |

#### Phase 5: Model Distillation

| Feature | Status | Notes |
|---------|--------|-------|
| MCP trace recording | Planned | Record tool-use traces from frontier models developing Hull apps |
| Hull development benchmark | Planned | Task suite: scaffold API, add auth, debug migration, implement outbox, fix vuln |
| LoRA fine-tuning pipeline | Planned | Fine-tune Qwen/Llama on Hull-specific traces (~5K–10K examples) |
| Evaluation harness | Planned | Benchmark score per model: tool selection accuracy, API recall, pattern adherence |
| Context optimization | Planned | Test minimum viable context per task per model size |

**Agentic Workflow Design:**

```
Bootstrap → Scaffold → Develop (tight loop) → Validate → Deploy → Monitor
    │            │            │                    │          │         │
    ▼            ▼            ▼                    ▼          ▼         ▼
 context()   scaffold()   status()            request()   build()   monitor()
                          errors()            render()    migrate()
                          routes()            test()
                          db_schema()
```

Every step produces machine-readable JSON output. The agent never parses human-formatted text. `hull dev --agent` writes structured sidecar files (`.hull/last_error.json`, `.hull/status.json`) for the develop loop.

**Supported Environments:**

| Environment | Interface | Context Strategy |
|-------------|-----------|------------------|
| Claude Code | CLAUDE.md + CLI + MCP | Full context (~20K tokens) |
| Codex | codex.md + CLI | Full context |
| OpenCode | .opencode.yml + MCP | Compact context + MCP tool schemas |
| Cursor/Windsurf | .cursorrules + MCP | Compact context |
| Ollama (local 70B) | CLI or MCP | Compact context (~4K tokens) |
| Ollama (local 7–14B) | MCP (structured tools) | Minimal context (~1K tokens) |

### Local Agent Runtime - Open-Weight Models Building Hull Apps

Hull should be able to run as the complete local development loop for apps
written by agents: a constrained runtime for the generated app, a constrained
tool surface for the generating agent, and a local/open-weight model backend
that can inspect, edit, test, build, and sign Hull applications without cloud
dependency.

The product shape is not "Hull embeds a chat UI." It is a secure local agent
workbench:

```
open-weight model
  -> Hull Agent Runtime
  -> bounded tools: read/patch/hull agent/test/build/dev
  -> Hull app source
  -> hull build
  -> signed single-file artifact
```

Target model backends:

- `bitnet.c` for zero-dependency CPU-first local inference.
- `llama.cpp` / `llama-server` for GGUF local models.
- Ollama for existing local model installations.
- Generic OpenAI-compatible local HTTP servers.
- Optional remote providers later, only behind explicit manifest/CLI network
  permission.

Example UX:

```bash
hull models add qwen-local --provider llama.cpp --url http://127.0.0.1:8080
hull models add gemma-local --provider ollama --model gemma
hull models add bitnet-qwen --provider bitnet --model ~/.hull/models/qwen.gguf
hull models test bitnet-qwen

hull develop . --model bitnet-qwen --goal "Add login, sessions, and tests"
hull develop . --model qwen-local --review-only
hull develop . --model gemma-local --approve-each-patch
```

#### Provider Layer

Normalize model backends behind one local interface:

| Operation | Purpose |
|-----------|---------|
| `chat` | Agent planning and tool decisions |
| `complete` | Fallback for non-chat models |
| `stream` | TUI progress and long generations |
| `embed` | Optional context retrieval |
| `tokenize` / `count_tokens` | Context budgeting |
| `health` | Backend readiness and model metadata |

Model profile:

```json
{
  "id": "qwen-local",
  "provider": "llama.cpp",
  "endpoint": "http://127.0.0.1:8080",
  "context_window": 32768,
  "supports_tools": true,
  "supports_json_schema": true,
  "supports_streaming": true,
  "supports_embeddings": false
}
```

CLI surface:

| Command | Status | Notes |
|---------|--------|-------|
| `hull models list` | Planned | Configured local and remote model profiles |
| `hull models add` | Planned | Register `bitnet`, `llama.cpp`, `ollama`, or OpenAI-compatible endpoint |
| `hull models test` | Planned | Health check + simple JSON/tool-call probe |
| `hull models inspect --json` | Planned | Context window, capabilities, backend version |
| `hull models serve` | Planned | Launch managed local backend where supported |
| `hull models bench` | Planned | Hull-specific coding/tool-use evaluation suite |

#### Bundled `bitnet.c`

Bundling `bitnet.c` makes sense as an optional build/profile because it closes
the local-first loop: a single Hull tool binary can run the app runtime, the
agent tool runtime, the build pipeline, and CPU-first inference without Ollama,
Python, CUDA, or external servers.

Recommended policy:

- Ship `bitnet.c` as an optional embedded provider in the developer tool
  distribution, not as part of every built Hull app.
- Keep model weights outside normal app bundles by default. Weights are large,
  often separately licensed, and should be user-managed under
  `~/.hull/models/` or an explicitly configured path.
- Allow a special `hull models bundle` / `hull build --include-model` path only
  for deliberate appliances where the size/licensing tradeoff is explicit.
- Compile-time flag: `HL_ENABLE_BITNET=1`.
- Doctor row: `hull doctor` reports `bitnet.c` support, CPU features, and
  model directory.
- Provider behavior must still flow through the same agent permission model as
  every other backend. The model is local, but the agent is still constrained.

This keeps the default Hull app artifact small while allowing a "single binary
developer tool" distribution that genuinely works offline with open weights.

#### Constrained Agent Command

Add a first-class local agent loop:

```bash
hull develop .
hull develop . --goal "Build a local invoicing app"
hull develop . --model bitnet-qwen --budget 30m
hull develop . --dry-run
hull develop . --review-only
```

The loop should be policy-driven rather than prompt-only:

1. Understand the user goal.
2. Inspect the project with `hull agent`.
3. Build a task-specific context pack.
4. Ask the model for the next structured action.
5. Apply patches only through a controlled patch tool.
6. Run `hull agent validate`, `hull agent test`, and relevant requests.
7. Iterate until tests/build pass or the budget is exhausted.
8. Produce a signed session report.

Default tool permissions:

- no arbitrary shell
- no network
- no writes outside the project
- no deletion unless explicitly approved
- edits are patches, not direct blind rewrites
- all tool calls are logged
- command surface is allowlisted

Example policy:

```json
{
  "read": ["app.lua", "migrations/", "templates/", "static/", "tests/"],
  "write": ["app.lua", "migrations/", "templates/", "static/", "tests/"],
  "commands": [
    "hull agent *",
    "hull test",
    "hull build",
    "hull dev"
  ],
  "network": false,
  "max_steps": 80,
  "max_patch_bytes": 200000
}
```

#### Agent Sessions and Auditability

Every `hull develop` run should be replayable:

```
.hull/agent/sessions/<timestamp>/
  goal.json
  model.json
  policy.json
  transcript.jsonl
  tool_calls.jsonl
  patches/
  final_report.md
  build_artifacts.json
```

Commands:

| Command | Status | Notes |
|---------|--------|-------|
| `hull agent sessions .` | Planned | List recorded local agent sessions |
| `hull agent diff SESSION` | Planned | Show patches from a session |
| `hull agent replay SESSION` | Planned | Replay tool calls where deterministic |
| `hull agent report SESSION` | Planned | Human-readable final report |
| `hull verify-agent-session SESSION` | Planned | Check transcript, patch hashes, model profile, and build output |

#### Context Packs

Local models need less raw text and more structure. Extend `hull agent context`
into project-aware context packs:

```bash
hull context build . --task web-app --model bitnet-qwen
hull context inspect . --json
```

Automatic context should include:

- manifest and declared modules
- route map
- DB schema and migrations
- relevant stdlib docs snippets
- failing tests and recent structured errors
- project tree and selected file excerpts
- capability diff
- build/test command recipes

The context packer should know the model's context window and choose
`minimal`, `compact`, or `full` context automatically.

#### Structured Tool Calling

Models should return validated JSON actions:

```json
{
  "action": "apply_patch",
  "path": "app.lua",
  "patch": "...",
  "reason": "Add POST /login and session creation"
}
```

Support degraded mode for weaker local models:

- prompt-enforced JSON
- parser repair
- retry with validation errors
- strict retry limit
- fallback to user question

The agent runtime should never execute natural-language commands directly.

#### Model Benchmarks

Add a Hull-specific local model benchmark:

```bash
hull models bench bitnet-qwen
hull models bench qwen-local --suite hull-apps
hull models rank
```

Evaluate:

- JSON action validity
- patch apply rate
- Lua syntax correctness
- manifest/module correctness
- migration generation
- test-fixing ability
- use of `hull agent` outputs
- context-window behavior

Output:

```json
{
  "model": "bitnet-qwen",
  "tool_json_valid_rate": 0.92,
  "patch_apply_rate": 0.87,
  "hull_test_pass_rate": 0.64,
  "recommended_roles": ["review", "small_edit"]
}
```

#### Phased Plan

| Phase | Feature | Status | Notes |
|-------|---------|--------|-------|
| L1 | Provider abstraction | Planned | `bitnet.c`, `llama.cpp`, Ollama, OpenAI-compatible local HTTP |
| L2 | `hull models` CLI | Planned | add/list/test/inspect/serve |
| L3 | `hull develop` agent loop | Planned | inspect -> patch -> validate -> test -> build |
| L4 | Agent policy file | Planned | read/write/command/network restrictions |
| L5 | Session logging | Planned | replayable transcript, tool calls, patch hashes |
| L6 | Context packs | Planned | project-aware context assembly and token budgeting |
| L7 | Structured action schemas | Planned | JSON action validation + repair/retry |
| L8 | Model benchmark suite | Planned | score local models on Hull development tasks |
| L9 | Managed local backends | Planned | launch/stop/status for supported providers |
| L10 | Multi-role workflows | Planned | planner/coder/reviewer/test-fixer/security-reviewer |

### Mission-Critical and Embedded Agentic Software

Hull's strongest thesis is not simply local app development. It is a secure
runtime and build system for agent-generated local software:

> Hull lets organizations use AI-generated code without giving that code, or
> the agent that wrote it, uncontrolled access to the machine.

For mission-critical and embedded markets, the differentiator is the combined
trust boundary:

- constrained agent runtime
- constrained app runtime
- explicit capabilities
- offline/open-weight model support
- single-file deployment
- reproducible builds
- signed provenance
- structured local observability
- no mandatory cloud control plane
- no package-manager dependency graph at runtime

The missing work is mostly not more web framework APIs. It is policy,
provenance, certification, deterministic offline operation, and embedded
deployment hardening.

#### Policy as Product

Add first-class named policy profiles:

```bash
hull policy check .
hull policy apply embedded-strict
hull policy apply factory-floor-offline
hull policy apply medical-device-local
hull policy apply vehicle-semantic-layer
```

Policies should cover:

- allowed capabilities
- network mode: none, localhost, allowlisted outbound only
- max binary size
- max memory
- dynamic code prohibition
- runtime model-download prohibition
- required tests
- required signatures
- required SBOM
- required reproducibility metadata
- required human approvals for capability changes

Policy check output should be machine-readable and signable:

```json
{
  "policy": "embedded-strict",
  "ok": true,
  "capabilities": {
    "network": false,
    "fs_write": ["data/"],
    "env": []
  },
  "requirements": {
    "tests": "pass",
    "sbom": "present",
    "reproducible_build": "attested"
  }
}
```

#### Signed Agent and Build Provenance

Every agent-generated change should be attributable:

```bash
hull attest build .
hull verify-attestation app.bin
hull verify-agent-session SESSION
```

Attestation should include:

- model/provider/profile used
- model file hash where local
- prompt/goal hash
- context pack hash
- tool-call transcript hash
- patch hashes
- test results
- capability diff
- policy result
- build inputs
- output binary hash

This creates an auditable chain from human goal -> model actions -> source
patches -> tests -> signed binary.

#### Deterministic Offline Development

Support a fully offline agentic loop:

```bash
hull develop . \
  --offline \
  --model bitnet-qwen \
  --policy embedded-strict \
  --approve capability-change
```

Guarantees:

- no network access
- local model only
- local context/docs only
- local tests only
- bounded tool surface
- replayable session
- signed result

This is the operating mode for defense, industrial control, remote
infrastructure, healthcare, and classified/off-grid environments.

#### Local Observability Without Cloud

Mission-critical systems need supportability without SaaS telemetry:

```bash
hull diagnose ./app
hull inspect-run ./app --last 24h
hull export-support-bundle ./app
```

Support bundle contents:

- app manifest and capability policy
- structured logs
- capability audit logs
- health/readiness history
- crash metadata
- version/build attestation
- local metrics
- recent configuration changes

No data should leave the device unless the operator explicitly exports it.

#### Capability Diff and Approval Gates

Permission changes should be visible and reviewable:

```bash
hull capability diff old.bin new.bin
hull develop . --approve capability-change
hull develop . --approve network
hull develop . --approve db-migration
hull develop . --approve deletion
```

Example diff:

```json
{
  "added": {
    "hosts": ["api.vendor.example"],
    "fs_write": ["exports/"]
  },
  "removed": {},
  "risk": "medium"
}
```

Agent workflows must stop at these gates unless approval policy allows them.

#### Reproducible Build Attestation

Extend the existing reproducible-build direction into a product surface:

```bash
hull build --repro .
hull verify-repro app.bin source.tar
hull attest-repro app.bin
```

Attested inputs:

- Hull version
- platform archive hash
- linker/cosmocc hash
- app source hash
- manifest hash
- generated asset hash
- compiler flags
- timestamp policy
- target platform

#### Embedded Deployment Profiles

Add hardened embedded targets:

```bash
hull build --target linux-arm64 --profile embedded
hull build --target riscv64 --profile gateway
hull deploy systemd --profile watchdog
```

Profile features:

- static binary
- readonly-rootfs mode
- explicit data directory
- watchdog integration
- crash restart policy
- health endpoint or CLI health check
- memory ceiling
- no writable paths except declared data
- OTA/update hook integration

#### Vehicle and Autonomous Systems Boundary

Hull is a good fit for the control-adjacent and semantic layers of autonomous
systems, not for hard realtime actuator loops in v1.

Good-fit layers:

- mission planning
- semantic task execution
- route/waypoint policy
- local UI/API on the vehicle or base station
- telemetry collection and compression
- command validation and authorization
- geofence and rules-of-engagement checks
- perception post-processing where bounded latency is acceptable
- local model inference for classification or decision support
- data logging and replay
- configuration management
- update/rollback controller

Poor-fit layers unless Hull grows substantial realtime/certification support:

- motor control
- flight stabilization loops
- brake/steering-by-wire loops
- hard realtime sensor fusion
- safety-critical actuator arbitration
- sub-millisecond deterministic control

Recommended architecture:

```
certified realtime controller / autopilot
  <-> narrow protocol boundary
Hull semantic/control-adjacent app
  <-> policy, mission logic, logging, local inference, operator UI
```

Required future features for serious vehicle/robotics use:

- explicit realtime/non-realtime boundary docs
- hardware/protocol capabilities: MAVLink, CAN/UAVCAN, serial, GPIO where
  supported
- deadline and watchdog APIs
- bounded queue/backpressure primitives
- deterministic state-machine helper
- command authorization gates
- safe-mode/fail-closed behavior
- flight/drive log replay harness
- simulation-in-the-loop test runner
- hardware-in-the-loop test hooks
- certification artifact generation for standards such as DO-178C,
  ISO 26262, IEC 61508, or IEC 62304 where applicable

#### App-Level Local Inference Capability

Keep "agent uses a model to build the app" separate from "the deployed app uses
a model at runtime." Runtime inference should be a separately declared app
capability:

```lua
app.manifest({
  modules = { "hull/llm@1" },
  llm = {
    models = { "classifier-q4" },
    network = false,
    max_tokens = 256,
  },
})
```

This allows embedded apps to run small local classifiers/planners while keeping
model files, token limits, memory use, and network access explicit.

#### Certification Artifacts

Add a certification bundle generator:

```bash
hull certify .
```

Outputs:

- SBOM
- capability manifest
- policy result
- threat model summary
- test report
- build attestation
- agent provenance
- dependency list
- known limitations
- runtime policy
- update/rollback procedure

#### Phased Plan

| Phase | Feature | Status | Notes |
|-------|---------|--------|-------|
| M1 | Policy profiles | Planned | `hull policy check/apply`, named embedded and regulated profiles |
| M2 | Agent/build provenance | Planned | Signed session and build attestations |
| M3 | Offline agent mode | Planned | local model, local context, no network, bounded tools |
| M4 | Capability diff | Planned | binary/source capability delta with risk labels |
| M5 | Human approval gates | Planned | network, deletion, DB migration, capability change |
| M6 | Repro build surface | Planned | `build --repro`, `verify-repro`, signed input metadata |
| M7 | Embedded profiles | Planned | readonly rootfs, watchdog, memory ceilings, target presets |
| M8 | Local observability bundle | Planned | diagnostics and support export without telemetry |
| M9 | Runtime LLM capability | Planned | explicit app-level local inference module and policy |
| M10 | Certification bundle | Planned | SBOM, policy, tests, provenance, update procedure |

### Future. Advanced Features

| Feature | Status | Notes |
|---------|--------|-------|
| WASM compute plugins (WAMR) | **Done** | Sandboxed, gas-metered, no I/O. Sync + async + streaming + persistent instances + shared data segments + SIMD128 + AOT. (Memory64: detection + `memory64_requires_aot` + 8-cell AOT dispatch shipped via the public accessor patch 0005, CI-gated — [#318](https://github.com/artalis-io/hull/issues/318); SPAN_INFO-under-mem64 mapped spans validated, CI-gated — [#334](https://github.com/artalis-io/hull/issues/334); the `hull build` AOT path for a mem64 plugin is not yet supported — [#336](https://github.com/artalis-io/hull/issues/336).) |
| GPU compute shaders (wgpu-native) | **Done** | dispatch + pipeline + persistent buffers + textures + fire-and-forget + async + buffer copy |
| User-defined SQL functions | **Done** | Lua/JS callbacks + WASM-backed UDFs with gas metering |
| Image processing | **Done** | stb_image decode/encode, raw pixel buffers, GPU texture interop |
| WebSocket server + client | **Done** | `app.ws()` + `ws.connect()` + broadcast + per-connection data |
| SSE endpoints | **Done** | `app.sse()` with chunked transfer encoding |
| Background work / timers | **Done** | `app.every()`, `app.daily()`. Async-capable repeating timers |
| Compression (gzip) | **Done** | Keel-integrated response compression via miniz |
| Connection pooling | **Done** | Outbound HTTP reuses TCP+TLS connections (32 pool, 4 per host, 60s idle) |
| ETag support | **Done** | `hull.web.middleware.etag`. Compute + compare + 304 Not Modified |
| PostgreSQL support | Shipped | Pure-C wire client behind the `HlDbBackend` vtable; `postgres://` DSN selects it. Handles-only API (`db.default()` / `db.connect(name)`) |
| Database encryption at rest | Planned | SQLite SEE or custom VFS |
| HTTP/2 full support | [Plan](http2_plan.md) | Currently h2c upgrade only |
| PDF document builder | Planned | Report generation |
| Module/package ecosystem | Planned | `hull add <package>` for sharing middleware and compute plugins |

### WASM / GPU Compute. Remaining Work

(Merged in from the former `roadmap_wasm_compute.md`. The shipped phases (SIMD128, memory limits, GPU/WebGPU, instance pooling, WasmBuffer protocol, persistent instances, shared data segments, GPU textures, streaming I/O, SQLite UDFs) are listed in the "Done" sections above. Memory64's Hull cap-layer dispatch (detection + `memory64_requires_aot` + 8-cell AOT dispatch) shipped via the public accessor patch 0005, CI-gated — see [#318](https://github.com/artalis-io/hull/issues/318); SPAN_INFO metadata under Memory64 (mapped spans on a 64-bit guest) is validated, CI-gated — [#334](https://github.com/artalis-io/hull/issues/334); the `hull build` AOT path for a Memory64 compute plugin is not yet supported — [#336](https://github.com/artalis-io/hull/issues/336).)

| Item | Status | Notes |
|------|--------|-------|
| `hull compute new <name> [--lang c]` scaffolding | ✅ Shipped | Generates `compute/<name>/{<name>.c, hull_compute.h, test_fixtures.json}` with the correct `hull_process` ABI exports. C language only; Rust deferred. |
| `hull compute build [name]` | ✅ Shipped | Compiles `compute/<name>/<name>.c` → `compute/<name>.wasm` via clang. Auto-runs as a step inside `hull build` for stale sources (`--no-build-compute` opts out). |
| `hull compute test <name>` | ✅ Shipped | Runs `test_fixtures.json` against the compiled module via a tempdir `hull test` harness. |
| `hull compute check <name>` | ✅ Shipped | Validates WASM magic + roundtrip-loads the module in WAMR. |
| `hull_compute.h` freestanding ABI header | ✅ Shipped | Embedded in the binary; written to each module dir on `hull compute new`. Includes libc shim (`hull_memcpy`/`memset`/`memcmp`/`strlen`) + 64 KiB bump allocator + UDF wire format constants. |
| `hull agent deploy` compute enumeration | ✅ Shipped | Per-module `{name, wasm_size, has_aot, has_source, source_stale}` plus stale-source advisory in recommendations. |
| Sample compute modules | ✅ Shipped (initial set) | `examples/compute/` includes `vector_ops`, `sort`, `hash`, `json_extract`, `scoring`, `text`, `score`, `echo`. Each has C source + compiled `.wasm` + a working `app.lua` that invokes them. |
| `--lang=rust` scaffolding | Planned | `wasm32-unknown-unknown` target + `Cargo.toml` template + `panic_handler`. Deferred. Manually-authored Rust modules work today (they only need to expose `hull_process` and `hull_version`); only the scaffolding shortcut is C-only. |
| Reproducible AOT cross-builds | Planned | Today `hull build` auto-AOT-compiles for the host arch when `wamrc` is present. Multi-arch AOT (`x86_64` + `aarch64` together) works under cosmocc; CI should produce both for every release. |

Out of scope (declined or downstream):

- **Result caching for `compute.call`**. Declined; app-level concern. Apps that need it can cache on top of `db.query` or in a Lua table.
- **`compute.async.streaming`**. Covered by the existing `compute.stream` API. No separate async-stream variant needed.

### Phase 9. Trusted Rebuild Infrastructure

- [ ] Reproducible build verification service at `api.gethull.dev/ci/v1`
- [ ] Build metadata attestation: `cc_version` + `flags` in `package.sig`
- [ ] Binary hash comparison: rebuild from source, compare against signed hash
- [ ] "Reproducible Build Verified" badge
- [ ] Self-hosted rebuild: run your own service, pin your own platform key

Hull's architecture makes reproducible builds achievable:

1. App developers cannot write C. Only Lua/JS source
2. Platform binary is hash-pinned. `platform.sig` locks exact bytes
3. Trampoline is deterministic. Generated from template + app registry
4. Cosmopolitan produces deterministic output. Static linking, no timestamps
5. Build metadata is signed. `cc_version` + `flags` attested by developer

### Keel HTTP Server

Keel is a separate project ([github.com/artalis-io/keel](https://github.com/artalis-io/keel)) vendored as a git submodule. Its audit history and roadmap live there. Hull's pinned submodule tracks Keel's current state; the previous Hull-side audit findings (kqueue bitmask, WebSocket / HTTP/2 partial writes, TLS key zeroization, `writev_all` EAGAIN spin) are resolved upstream and reflected in the current pin.

## Benchmark Baseline

Measured on GitHub Actions Ubuntu runner (2 threads, 50 connections, 5s duration via `wrk`).

### GET /health (no DB. Pure runtime overhead)

| Runtime | Req/sec | Avg Latency | Max Latency |
|---------|--------:|------------:|------------:|
| Lua 5.4 | 98,531 | 500 us | 1.84 ms |
| QuickJS | 52,263 | 950 us | 1.73 ms |

### GET / (DB write + JSON response)

| Runtime | Req/sec | Avg Latency | Max Latency |
|---------|--------:|------------:|------------:|
| Lua 5.4 | 6,866 | 7.42 ms | 28.02 ms |
| QuickJS | 4,588 | 10.97 ms | 28.36 ms |

### GET /greet/:name (route param extraction)

| Runtime | Req/sec | Avg Latency | Max Latency |
|---------|--------:|------------:|------------:|
| Lua 5.4 | 102,204 | 485 us | 6.98 ms |
| QuickJS | 57,405 | 870 us | 7.71 ms |

Lua is ~1.9x faster than QuickJS. Non-DB routes sustain 50k–100k req/s on a single CI VM core.
