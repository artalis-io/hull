# Hull — Roadmap

## What's Built

### Core Platform
- Dual-runtime support: Lua 5.4 + QuickJS (ES2023), one active per app
- Keel HTTP server (epoll/kqueue/io_uring/poll) with route params and middleware
- SQLite with WAL mode, parameterized queries, prepared statement cache, performance PRAGMAs
- Request body reading, multipart/form-data, chunked transfer-encoding
- WebSocket support (text, binary, ping/pong, close)
- HTTP/2 support (h2c upgrade)

### Capabilities (C enforcement layer)
- **Crypto:** SHA-256, SHA-512, HMAC-SHA256, HMAC-SHA512/256, PBKDF2, base64url, random bytes, password hash/verify, Ed25519 (sign/verify/keypair), XSalsa20+Poly1305 secretbox, Curve25519 box
- **Filesystem:** Sandboxed read/write/exists/delete/mmap with path traversal rejection, symlink escape prevention via realpath
- **Database:** Query/exec with parameterized binding, batch transactions, statement cache, user-defined functions (Lua/JS/WASM)
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
- `hull.json` — canonical JSON encode/decode (sorted keys for deterministic signatures)
- `hull.cookie` — cookie parsing and serialization with secure defaults
- `hull.middleware.session` — server-side SQLite-backed sessions with sliding expiry
- `hull.jwt` — JWT HS256 sign/verify/decode (no "none" algorithm, constant-time comparison)
- `hull.middleware.csrf` — stateless CSRF tokens via HMAC-SHA256
- `hull.middleware.auth` — authentication middleware factories (session auth, JWT Bearer auth)
- `hull.middleware.logger` — request logging with logfmt output and auto-assigned request IDs
- `hull.middleware.transaction` — wraps handlers in SQLite BEGIN IMMEDIATE..COMMIT
- `hull.middleware.idempotency` — Idempotency-Key middleware with response caching and fingerprinting
- `hull.middleware.outbox` — transactional outbox for reliable webhook/HTTP delivery with exponential backoff
- `hull.middleware.inbox` — inbox deduplication for incoming events/webhooks
- `hull.validate` — declarative input validation with schema rules
- `hull.form` — URL-encoded form body parsing
- `hull.i18n` — internationalization with locale detection, message bundles, formatting helpers
- `hull.template` — compile-once render-many HTML template engine with inheritance, includes, filters, auto-escaping
- `hull.csv` — CSV parse/encode (RFC 4180)
- `hull.search` — full-text search (SQLite FTS5)
- `hull.middleware.rbac` — role-based access control
- `hull.middleware.cors` — CORS headers + preflight handling
- `hull.middleware.ratelimit` — in-memory rate limiting with configurable windows
- Static file serving — convention-based (`static/` → `/static/*`), MIME detection, ETag/304, embedded in builds, zero-copy sendfile in dev

### Background Work
- `app.every(ms, fn)` — repeating interval timers with full async support (sleep, HTTP fetch, DB)
- `app.daily("HH:MM", fn, opts)` — daily wall-clock timers (UTC or local)
- Self-cancellation via `return false`, error-resilient (logs + reschedules), one-invocation-at-a-time guard
- Detached async mode — timer callbacks use `kl_timer_add` instead of `kl_async_suspend` for connectionless operation

### Build & Deployment
- `hull build` — compile Lua/JS apps into standalone binaries (auto-AOT for WASM, embeds all assets)
- `hull new` — project scaffolding with example routes and tests
- `hull dev` — development server with hot reload
- `hull test` — in-process test runner (no TCP, memory SQLite, both runtimes)
- `hull deploy` — deployment config generator (Dockerfile, systemd, fly.toml) — manifest-aware
- `hull eject` — export to standalone Makefile project
- `hull inspect` — display capabilities and signature status
- `hull verify` — dual-layer Ed25519 signature verification
- `hull keygen` — Ed25519 keypair generation
- `hull sign-platform` — sign platform libraries with per-arch hashes
- `hull manifest` — extract and print manifest as JSON
- `hull migrate` — SQL migration runner (auto-run on startup, embedded in builds)
- `hull migrate new` / `hull migrate status` — migration scaffolding and status
- `hull agent` — 10 machine-readable subcommands (routes, db schema/query, request, status, errors, test, context, migrate, deploy)
- `hull mcp` — stdio MCP server wrapping agent core
- `hull compute` — WASM module management
- `hull check` — full validation (clean + ASan + test + e2e)
- Multi-arch Cosmopolitan APE builds (`make platform-cosmo`)
- Self-build reproducibility chain (hull → hull2 → hull3)

### Security
- Kernel sandbox: pledge/unveil on Linux (seccomp-bpf + landlock) and Cosmopolitan
- Manifest-driven capability declaration and enforcement
- Dual-layer Ed25519 signatures (platform + app)
- Platform canary with integrity hash
- Browser verifier (offline, zero-dependency HTML tool)
- Runtime startup verification (`--verify-sig`)
- Shell-free tool mode with compiler allowlist
- Lua sandbox (removed io/os/load, memory limit, custom allocator)
- QuickJS sandbox (removed eval/std/os, memory limit, instruction-count gas metering)

### CI/CD
- Linux, macOS, Cosmopolitan APE builds
- ASan + UBSan, MSan + UBSan sanitizer runs
- Static analysis (scan-build + cppcheck)
- Code coverage
- E2E tests for all examples in both runtimes + template engine tests + stdlib middleware tests + deploy config tests
- Sandbox violation tests (Linux + Cosmo)
- Benchmarks (Lua vs QuickJS, DB vs non-DB routes, WASM interpreter vs AOT, GPU vs CPU)

## Roadmap

### Next — Distribution: hull.com Downloadable Tool

The goal: a single downloadable Cosmopolitan APE binary that users install and immediately use to create, build, test, run, and deploy Hull applications — zero dependencies.

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

The missing piece: `hull build` shells out to `cc` to compile generated C code. Users need gcc/clang/cosmocc installed. Everything else below addresses that gap and the surrounding distribution story.

#### Phase D1: Version + Release Pipeline — **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| `hull version` command | **Done** | Pure C; `--json` for machine-readable output (version/runtime/platform/build) |
| Git tag → version string | **Done** | `git describe --tags` at build time → `HL_VERSION` define; `VERSION` file override |
| GitHub Actions release workflow | **Done** | `.github/workflows/release.yml`, triggered on `v*` tag push |
| Release artifacts | **Done** | `hull-cosmo` (APE), `hull-linux-x86_64`, `hull-darwin-arm64`, `hull.sha256` |
| Native release artifacts | **Done** | Linux x86_64 + macOS arm64 native; cosmo APE covers Linux arm64, macOS x86_64, BSDs, Windows |

#### Phase D2: Install Script + First-Run Experience — **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| Install script (`curl -fsSL .../install.sh \| sh`) | **Done** | POSIX, ~250 LOC: detect OS/arch, fetch latest release, SHA-256 verify, install to `~/.local/bin/hull` |
| `hull init` (in-place project init) | **Done** | Like `git init`; idempotent; auto-detects existing runtime; Lua tool module |
| First-run welcome + doctor | **Done** | `hull doctor` reports platform embed, compiler availability, TCC + CA bundle status; `--json` mode |
| Shell completions | **Done** | bash, zsh, fish; covers every subcommand and flag in `completions/` |

#### Phase D3: Zero-Dependency Builds (Embedded TCC) — **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| Vendor tcc | **Done** | `vendor/tcc` submodule (mob branch), built by Makefile, embedded via xxd into `libhull_platform.a` |
| `hull build` auto-selects compiler | **Done** | `HlCompilerVtable`: TCC backend (compile) + system cc/gcc/clang (link); auto-fallback on macOS where TCC produces ELF |
| `hull build --compiler=tcc\|system\|<path>` | **Done** | Explicit backend selection; both `--compiler tcc` and `--compiler=tcc` accepted |
| Linux native + cosmo coverage | **Done** | TCC compile + system link verified end-to-end on Linux CI in `e2e_tcc.sh` |
| `hull toolchain install` | Skipped | Embedded TCC removes the original need; cosmocc users can `make fetch-cosmocc` |

#### Phase D4: Embedded CA Bundle — **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| Embed Mozilla CA bundle | **Done** | `vendor/cacert/cacert.pem` (curl.se, SHA-256 verified); xxd'd into `libhull_platform.a` so built apps inherit it |
| Auto-detect system CA store | **Done** | Resolution order: `--skip-ca-bundle` → `--ca-bundle PATH` → system paths → embedded → fail |
| `hull <app> --ca-bundle=PATH` | **Done** | Custom override via runtime flag |
| CA bundle update mechanism | **Done** | `make fetch-ca-bundle` pulls + verifies; `hull doctor` reports the embedded bundle's update date |
| New Keel API: `kl_tls_mbedtls_client_ctx_create_from_buf` | **Done** | Released in Keel v1.1.0 — TLS client from in-memory PEM/DER bundle |
| Pledge + unveil for outbound HTTPS | **Done** | Linux: `dns netlink unix` promises + unveiled `/etc/resolv.conf` (and family); polyfill's `kPledgeDns` extended with `sendmmsg`/`recvmmsg`/`sendmsg`/`recvmsg` for glibc 2.32+ |

#### Phase D5: Self-Update — **Done** (signature verification deferred)

| Feature | Status | Notes |
|---------|--------|-------|
| `hull update` | **Done** | Pure C in `src/hull/commands/update.c` (~330 LOC) — keel client + embedded CA bundle + mbedTLS SHA-256 + atomic `rename(2)` |
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
                                             ├── tcc (bundled compiler, embedded)
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
| CORS middleware | **Done** | `hull.middleware.cors` — configurable origins, preflight handling |
| Template engine (`{{ }}` HTML templates) | **Done** | `hull.template` — inheritance, includes, filters, compiled & cached |
| Input validation (schema-based) | **Done** | `hull.validate` — declarative field validation |
| Rate limiting middleware | **Done** | `hull.middleware.ratelimit` — sliding window, per-key |
| Static file serving (`/static/*` convention) | **Done** | MIME detection, ETag/304, embedded in builds, zero-copy sendfile in dev |
| i18n (locale detection + translations) | **Done** | `hull.i18n` — locale detection, message bundles, format helpers |
| Request logging middleware | **Done** | `hull.middleware.logger` — logfmt output, request IDs |
| Transaction middleware | **Done** | `hull.middleware.transaction` — BEGIN IMMEDIATE..COMMIT wrappers |
| Idempotency-Key middleware | **Done** | `hull.middleware.idempotency` — response caching, fingerprinting, 409 on mismatch |
| Transactional outbox | **Done** | `hull.middleware.outbox` — reliable delivery with exponential backoff |
| Inbox deduplication | **Done** | `hull.middleware.inbox` — incoming event dedup with TTL |
| CSV encode/decode (RFC 4180) | **Done** | `hull.csv` — RFC 4180 parse/encode |
| FTS5 search wrapper | **Done** | `hull.search` — full-text search backed by SQLite FTS5 |
| RBAC (role-based access control) | **Done** | `hull.middleware.rbac` — role/permission middleware |
| Email (SMTP / API) | **Done** | Outbound SMTP via C capability + stdlib |
| Health + readiness endpoints | **Done** | `hull.middleware.health` — liveness + readiness with custom checks |
| ETag response helpers | **Done** | `hull.middleware.etag` — compute + compare + 304 |
| Deployment config generator | **Done** | `hull deploy` — Dockerfile, systemd, fly.toml from manifest |
| License key system | Planned | Ed25519 offline verification for commercial distribution |

### Package & Module System

First-party stdlib modules are declared by the app and gated by the runtime.
"Nothing exists unless declared" — language runtimes provide pure computation;
access to side-effect modules (crypto, db, http, fs, …) is opt-in via the
manifest's `modules = {...}` array of canonical specs
(`"vendor/name@version"`, e.g. `"hull/crypto@1"`). Imports use the standard
`require()` / `import` forms with any local binding name. The foundation
(phases 1–2c plus header-dep tracking) is shipped; what follows is the rest
of the package-system roadmap in implementation order.

This is *not* npm/pip/cargo. It is capability-aware dependency declaration
for sealed Hull apps — no remote registry, no runtime install, no dynamic
discovery, no transitive third-party deps. Runtime code cannot install,
fetch, or load packages; the resolved module set is fixed at build/startup.

#### Foundation — **Done** (phases 1, 2a, 2b, 2c)

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
| Header-dep tracking in Makefile | **Done** | `-MMD -MP` + `-include build/**/*.d` — touching a header invalidates only the right `.o` files. (Was previously the source of mid-phase SIGSEGVs.) |

#### Visibility & UX — **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| `hull modules list [app_dir]` | **Done** | Declared modules from the app's manifest as JSON. |
| `hull modules available [--json]` | **Done** | Full registry dump (40 entries) with deps, required caps, intrinsic flag. |
| `hull modules explain <name>` | **Done** | Pretty-print one spec. |
| `hull modules analyze [app_dir]` | **Done** | Static `require`/`import` scanner — flags undeclared imports and unused declarations. Soft-fails on app-load errors so app-level filesystem fallbacks (e.g. `require("./locales/en.json")`) don't break the scan. |
| `hull check` integration | **Done** | Runs `modules list` + `modules analyze` before tests + verify. Manifest errors surface at check-time rather than first server startup. |
| `hull doctor` module subsystems section | **Done** | Doctor reports which `HL_ENABLE_*` subsystems are linked in. |
| `hull agent modules [app_dir]` | **Done** | Emits `{declared, intrinsic, build_caps, registry_count}` JSON. `hull agent manifest` also includes a `modules` block. |
| Record resolved set in `package.sig` | **Done** | `modules_resolved` field included in the canonical reconstruction in `verify.lua` — covered by the existing Ed25519 app-layer signature. "This binary was built with these modules" is now a signed, tamper-evident claim. |
| `hull init` / `hull new` generate `modules = {...}` | **Done** | Templates produce array-form module declarations matching what they actually use. |
| Documentation (`docs/security.md` §5b + `CLAUDE.md` + `README.md` + `AGENTS.md`) | **Done** | Module Declaration sections added to all four; failure-mode table, registry overview, CLI/agent pointers. |

#### Correctness coverage — **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| Static import/require analyzer | **Done** | `hull modules analyze` — Lua + JS state-machine scanners that skip comments and string literals; auto-seeds intrinsic core; integrated into `hull check`. |
| Gate non-server entry points | **Done** | Every consumer that runs user app code now opts in via `HlAppContextOpts.gate_modules = 1`: `hull test` (`test.c`), `hull agent` warm context (`agent.c`), all 7 `hull mcp` context openings (`mcp.c`). `hull dev`/serve is gated through `main.c::hl_serve_wire_caps`. Verified by `make e2e-examples` (280/280 passing under the gated test runner). |

#### Error-message UX — **Done**

| Feature | Status | Notes |
|---------|--------|-------|
| "Did you mean …" suggestions on gate errors | **Done** | `hl_module_registry_suggest()` (length-scaled Levenshtein, threshold 1/2/3 for ≤4/5–8/≥9-char inputs) wired into three error sites: resolver `unknown module 'X' in app.manifest.modules`, Lua `module not found: hull.X`, JS `unknown hull module: hull:X`. Each emits the suggested canonical name in the syntax the caller used. |

At this point the Package & Module System is feature-complete for v1; everything below is explicitly deferred.

#### Future — out of v1 (explicitly deferred)

| Feature | Status | Notes |
|---------|--------|-------|
| Third-party packages (`acme/widgets@1`) | Deferred | Vendor-prefix design + fetching + verification + trust model. The registry already supports the `vendor/name` form, so adding a second namespace is a small step; the broader infrastructure is a separate project. |
| Bundle stripping for `hull build` | Deferred | Physically remove undeclared stdlib bytes. Smaller binaries + reduced ROP surface, but adds a per-build compile step and complicates the platform-embed path. Runtime gating is the boundary in v1. |
| Per-opcode WASM `host_call` gating | Deferred | Manifest design supports it (`required_caps` bitmask has room); current gate admits all opcodes together when `hull/compute@1` is declared. |
| CBOR / additional pure codecs in intrinsic core | Deferred | Pure-stdlib expansion; orthogonal to the module system. |
| **Demote `hull/log` and `hull/json` to declared modules** | **Done** | The intrinsic core is now `hull/app` alone. Apps that call `log.X` or `json.X` directly must declare `"hull/log@1"` and/or `"hull/json@1"` in `manifest.modules`. The `res:json(...)` response helper and the runtime's internal JSON marshalling continue to work without any declaration — they bypass user-visible imports at the C layer via a registry-stashed decoder. |
| Lockfile for resolved-set pinning | Deferred | The "resolved set in `manifest.bin`" item above covers tamper detection. A separate lockfile is overkill for v1's no-third-party model. |
| `hull add <package>` install command | Deferred | The existing "Module/package ecosystem" row in *Future — Advanced Features* covers this. Out of scope until third-party packages exist. |

### CLI / Compute-Only Mode

`HL_ENABLE_HTTP=0` builds Hull without Keel, routing, or middleware —
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
| 1   | `app.main(fn)` registration + lifecycle | **Done** | — | Bindings, mode detection, conflict error, return-value → exit code coercion. Works on HTTP=1 builds. |
| 1.5 | Event-loop integration for async-in-main | **Done** | — | Lua coroutine wrap + cli_main_co detection in async resume; JS Promise + `.then` callback; both call `kl_server_stop` on terminal state. |
| 2   | `hull run` command + scaffolding | **Done** | — | `run` token stripped in `hull_main` and falls through to serve. `--` separator for app args. `hull new --cli` + `hull init` mode detection (auto-detected from `app.main(` presence). |
| 3a  | HTTP foundation (no behavior change) | **Done** | — | `HL_MOD_CAP_HTTP` bit, registry markings on http/ws/server/smtp/middleware/*, resolver + cap_label updates, `hull doctor` row + `--json` subsystems object, Makefile flag, `hull_serve` extracted from `main.c` into `src/hull/serve.c`. |
| 3b  | `HL_ENABLE_HTTP=0` build flag | **Done** | — | `make HL_ENABLE_HTTP=0` produces a working CLI-only binary. Drops HTTP cap files (http/http_async/ws/body/smtp/static), route/middleware/ws/sse/timers + runtime support files (routes/dispatch/sse/ws/timers/bindings), the agent_api in-process server, stdlib middleware embedding, `hull dev`. Replaces `serve.c` with `serve_cli.c` (load → resolve modules → sandbox → migrate → app.main → exit). Keel remains linked because its async primitives back `hull.sleep` / compute.async / etc; full Keel unlink → Phase 3d. Binary on arm64 Darwin shrinks ~145KB; ~1MB+ awaits Phase 3d. |
| 3c  | Sandbox narrowing | **Done** | — | `HlSandboxPolicy.network_inbound`: defaults to 1; serve.c sets to 0 when `rt->vt->has_main(rt)` is true. Pledge's `inet` promise is dropped entirely on CLI apps with no outbound either; macOS Seatbelt's `network-inbound`/`network-bind` rules become conditional on the same. |
| 3d  | Async + Net backend vtables | Planned | ~14d | Reframed from "replace Keel primitives" to **define `HlAsyncBackend` + `HlNetBackend` vtables** (mirror of `HlDbBackend`). Keel becomes the default impl; a poll-based async backend ships for `HL_ENABLE_HTTP=0` builds, fully unlinking Keel and enabling async-in-main on CLI builds. Future backends (libuv, io_uring, sandboxed mini-server) slot into the same interface. Full design: [docs/backend_vtables.md](backend_vtables.md). |
| 4   | Polish & documentation | **Mostly done** | — | `hull agent manifest` mode field, `examples/hello_cli/`, `tests/e2e_cli.sh`, AGENTS/CLAUDE doc markings for HTTP-build modules. Remaining: `hull build` mode validation (needs Phase 3b), more sample CLI apps. |

### Agent Platform — AI-Native Development Tooling

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

Common agent core with dual interface: CLI JSON mode for frontier models, MCP server for mid-range local models. Zero logic duplication — both call the same C functions.

#### Phase 1: Foundation (agent core + CLI) — Done

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

#### Phase 2: Context + Render — Done

Dynamic context system — `hull agent context` assembles task-relevant documentation on demand, sized for the model's context window.

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

#### Phase 3: MCP Server + Agent Configs — Partial

| Feature | Status | Notes |
|---------|--------|-------|
| `hull mcp` | **Done** | stdio MCP server wrapping agent core, warm context (shared `HlAppContext`) |
| `hull mcp serve --sse` | Planned | SSE transport for network-accessible agents |
| `.cursorrules` | Planned | Cursor/Windsurf agent rules |
| `codex.md` | Planned | Codex-specific instructions |
| `.opencode.yml` | Planned | OpenCode config with MCP server reference |
| Updated `CLAUDE.md` | **Done** | Full API reference, agent commands, conventions |
| Updated `AGENTS.md` | **Done** | Agent development guide with hull agent commands, patterns, stdlib |

#### Phase 4: Lifecycle + Monitoring — Partial

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
- TCC/cosmocc hash
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

### Future — Advanced Features

| Feature | Status | Notes |
|---------|--------|-------|
| WASM compute plugins (WAMR) | **Done** | Sandboxed, gas-metered, no I/O — sync + async + streaming + persistent instances + shared data segments + SIMD128 + Memory64 + AOT |
| GPU compute shaders (wgpu-native) | **Done** | dispatch + pipeline + persistent buffers + textures + fire-and-forget + async + buffer copy |
| User-defined SQL functions | **Done** | Lua/JS callbacks + WASM-backed UDFs with gas metering |
| Image processing | **Done** | stb_image decode/encode, raw pixel buffers, GPU texture interop |
| WebSocket server + client | **Done** | `app.ws()` + `ws.connect()` + broadcast + per-connection data |
| SSE endpoints | **Done** | `app.sse()` with chunked transfer encoding |
| Background work / timers | **Done** | `app.every()`, `app.daily()` — async-capable repeating timers |
| Compression (gzip) | **Done** | Keel-integrated response compression via miniz |
| Connection pooling | **Done** | Outbound HTTP reuses TCP+TLS connections (32 pool, 4 per host, 60s idle) |
| ETag support | **Done** | `hull.middleware.etag` — compute + compare + 304 Not Modified |
| PostgreSQL support | Planned | Behind same `db.query()`/`db.exec()` capability interface |
| Database encryption at rest | Planned | SQLite SEE or custom VFS |
| HTTP/2 full support | [Plan](http2_plan.md) | Currently h2c upgrade only |
| PDF document builder | Planned | Report generation |
| Module/package ecosystem | Planned | `hull add <package>` for sharing middleware and compute plugins |

### WASM / GPU Compute — Remaining Work

(Merged in from the former `roadmap_wasm_compute.md`. The shipped phases — SIMD128, memory limits, Memory64, GPU/WebGPU, instance pooling, WasmBuffer protocol, persistent instances, shared data segments, GPU textures, streaming I/O, SQLite UDFs — are listed in the "Done" sections above.)

| Item | Status | Notes |
|------|--------|-------|
| `hull compute new <name> [--lang c]` scaffolding | ✅ Shipped | Generates `compute/<name>/{<name>.c, hull_compute.h, test_fixtures.json}` with the correct `hull_process` ABI exports. C language only; Rust deferred. |
| `hull compute build [name]` | ✅ Shipped | Compiles `compute/<name>/<name>.c` → `compute/<name>.wasm` via clang. Auto-runs as a step inside `hull build` for stale sources (`--no-build-compute` opts out). |
| `hull compute test <name>` | ✅ Shipped | Runs `test_fixtures.json` against the compiled module via a tempdir `hull test` harness. |
| `hull compute check <name>` | ✅ Shipped | Validates WASM magic + roundtrip-loads the module in WAMR. |
| `hull_compute.h` freestanding ABI header | ✅ Shipped | Embedded in the binary; written to each module dir on `hull compute new`. Includes libc shim (`hull_memcpy`/`memset`/`memcmp`/`strlen`) + 64 KiB bump allocator + UDF wire format constants. |
| `hull agent deploy` compute enumeration | ✅ Shipped | Per-module `{name, wasm_size, has_aot, has_source, source_stale}` plus stale-source advisory in recommendations. |
| Sample compute modules | ✅ Shipped (initial set) | `examples/compute/` includes `vector_ops`, `sort`, `hash`, `json_extract`, `scoring`, `text`, `score`, `echo`. Each has C source + compiled `.wasm` + a working `app.lua` that invokes them. |
| `--lang=rust` scaffolding | Planned | `wasm32-unknown-unknown` target + `Cargo.toml` template + `panic_handler`. Deferred — manually-authored Rust modules work today (they only need to expose `hull_process` and `hull_version`); only the scaffolding shortcut is C-only. |
| Reproducible AOT cross-builds | Planned | Today `hull build` auto-AOT-compiles for the host arch when `wamrc` is present. Multi-arch AOT (`x86_64` + `aarch64` together) works under cosmocc; CI should produce both for every release. |

Out of scope (declined or downstream):

- **Result caching for `compute.call`** — declined; app-level concern. Apps that need it can cache on top of `db.query` or in a Lua table.
- **`compute.async.streaming`** — covered by the existing `compute.stream` API. No separate async-stream variant needed.

### Phase 9 — Trusted Rebuild Infrastructure

- [ ] Reproducible build verification service at `api.gethull.dev/ci/v1`
- [ ] Build metadata attestation: `cc_version` + `flags` in `package.sig`
- [ ] Binary hash comparison: rebuild from source, compare against signed hash
- [ ] "Reproducible Build Verified" badge
- [ ] Self-hosted rebuild: run your own service, pin your own platform key

Hull's architecture makes reproducible builds achievable:

1. App developers cannot write C — only Lua/JS source
2. Platform binary is hash-pinned — `platform.sig` locks exact bytes
3. Trampoline is deterministic — generated from template + app registry
4. Cosmopolitan produces deterministic output — static linking, no timestamps
5. Build metadata is signed — `cc_version` + `flags` attested by developer

### Keel HTTP Server

Keel is a separate project ([github.com/artalis-io/keel](https://github.com/artalis-io/keel)) vendored as a git submodule. Its audit history and roadmap live there. Hull's pinned submodule tracks Keel's current state; the previous Hull-side audit findings (kqueue bitmask, WebSocket / HTTP/2 partial writes, TLS key zeroization, `writev_all` EAGAIN spin) are resolved upstream and reflected in the current pin.

## Benchmark Baseline

Measured on GitHub Actions Ubuntu runner (2 threads, 50 connections, 5s duration via `wrk`).

### GET /health (no DB — pure runtime overhead)

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
