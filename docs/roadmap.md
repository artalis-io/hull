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

#### Phase D1: Version + Release Pipeline

| Feature | Status | Effort | Notes |
|---------|--------|--------|-------|
| `hull version` command | Planned | 1h | VERSION file baked at compile time, printed by `hull version` subcommand |
| Git tag → version string | Planned | 1h | `git describe --tags` at build time → `HL_VERSION` define |
| GitHub Actions release workflow | Planned | 4h | On tag push: `make platform-cosmo` → `make CC=cosmocc EMBED_PLATFORM=cosmo` → sign → upload |
| Release artifacts | Planned | — | `hull` (Cosmo APE), `hull.sha256`, `hull.sig` (Ed25519) |
| Native release artifacts | Planned | 2h | `hull-linux-x86_64`, `hull-darwin-arm64` for users who prefer native binaries |

#### Phase D2: Install Script + First-Run Experience

| Feature | Status | Effort | Notes |
|---------|--------|--------|-------|
| Install script (`curl -fsSL hull.com/install \| sh`) | Planned | 2h | Detect OS/arch, download binary, verify SHA-256, install to PATH |
| `hull init` (in-place project init) | Planned | 2h | Like `git init` — initialize hull in current directory (vs `hull new` which creates a new dir) |
| First-run welcome + doctor | Planned | 2h | `hull doctor` checks environment: compiler available? correct version? platform embedded? |
| Shell completions | Planned | 2h | Bash/Zsh/Fish completions for all subcommands and flags |

#### Phase D3: Zero-Dependency Builds (Bundle tcc)

This is the most impactful step. Currently `hull build` requires a system C compiler. Bundling [tcc](https://bellard.org/tcc/) (~100KB) makes `hull build` truly zero-dependency.

| Feature | Status | Effort | Notes |
|---------|--------|--------|-------|
| Vendor tcc | Planned | 4h | Add tcc to `vendor/tcc/`, compile as part of hull build |
| `hull build` auto-selects compiler | Planned | 2h | Prefer system cc if available, fall back to bundled tcc |
| `hull build --compiler=tcc\|cc\|cosmocc` | Planned | 1h | Explicit compiler selection flag |
| tcc cross-compilation | Planned | 4h | tcc can target x86_64 and aarch64 — verify both work for hull apps |
| `hull toolchain install` | Planned | 2h | Download cosmocc on demand, cache in `~/.hull/toolchain/` |

**Why tcc:** The code `hull build` compiles is trivial — one `app_registry.c` file containing byte arrays + a table, and a small `app_main.c` trampoline. All the real code is pre-compiled in `libhull_platform.a`. tcc compiles this in milliseconds. Optimization doesn't matter because it's just data declarations and one function call.

**Alternative considered:** Pre-compile `app_main.o` for each arch and embed it, so hull only needs to compile `app_registry.c` + link. Still needs a compiler for the registry. tcc is cleaner.

#### Phase D4: Embedded CA Bundle

| Feature | Status | Effort | Notes |
|---------|--------|--------|-------|
| Embed Mozilla CA bundle | Planned | 2h | ~200KB addition to binary, enables HTTPS on systems without a CA store |
| Auto-detect system CA store | Planned | 1h | Prefer system store if available, fall back to embedded |
| `hull build --ca-bundle=PATH` | Planned | 1h | Custom CA bundle for enterprise environments |
| CA bundle update mechanism | Planned | 1h | `hull update-ca` fetches latest Mozilla bundle |

Matters for: Cosmopolitan APE on Windows (no system CA store), minimal Docker containers (`FROM scratch`), air-gapped environments.

#### Phase D5: Self-Update

| Feature | Status | Effort | Notes |
|---------|--------|--------|-------|
| `hull update` | Planned | 4h | Check GitHub releases for newer version, download + verify + replace |
| `hull update --check` | Planned | 1h | Print "new version available" without updating |
| Signature verification on update | Planned | 1h | Verify Ed25519 signature of downloaded binary before replacing |
| Update channel (stable/beta) | Planned | 2h | `hull update --channel=beta` for pre-release testing |

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

### Keel HTTP Server — Audit Backlog

The [Keel C audit](keel_audit.md) identified issues to address upstream:

| Priority | Issue | Impact |
|----------|-------|--------|
| Critical | ~~kqueue READ\|WRITE bitmask (C-1)~~ | **Resolved** in Keel upstream |
| Critical | ~~WebSocket partial writes (C-2)~~ | **Resolved** in Keel upstream |
| High | ~~Protocol upgrade partial writes (H-3, H-4)~~ | **Resolved** in Keel upstream |
| High | ~~Private key material not zeroed (H-2)~~ | **Resolved** in Keel upstream |
| High | ~~writev_all busy-spin on EAGAIN (H-5)~~ | **Resolved** in Keel upstream |
| Medium | Add WebSocket fuzz target | Attack surface coverage gap |

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
