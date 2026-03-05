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
- **Filesystem:** Sandboxed read/write/exists/delete with path traversal rejection, symlink escape prevention
- **Database:** Query/exec with parameterized binding, batch transactions, statement cache
- **HTTP client:** Outbound HTTP/HTTPS with host allowlist enforcement (mbedTLS)
- **Environment:** Allowlist-enforced env var access
- **Time:** now, now_ms, clock, date, datetime

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
- Static file serving — convention-based (`static/` → `/static/*`), MIME detection, ETag/304, embedded in builds, zero-copy sendfile in dev

### Build & Deployment
- `hull build` — compile Lua/JS apps into standalone binaries
- `hull new` — project scaffolding with example routes and tests
- `hull dev` — development server with hot reload
- `hull test` — in-process test runner (no TCP, memory SQLite, both runtimes)
- `hull eject` — export to standalone Makefile project
- `hull inspect` — display capabilities and signature status
- `hull verify` — dual-layer Ed25519 signature verification
- `hull keygen` — Ed25519 keypair generation
- `hull sign-platform` — sign platform libraries with per-arch hashes
- `hull manifest` — extract and print manifest as JSON
- `hull migrate` — SQL migration runner (auto-run on startup, embedded in builds)
- `hull migrate new` — migration scaffolding
- `hull migrate status` — migration status display
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
- E2E tests for all 9 examples in both runtimes + 40 template engine tests + stdlib middleware tests
- Sandbox violation tests (Linux + Cosmo)
- Benchmarks (Lua vs QuickJS, DB vs non-DB routes)

## Roadmap

### Next — Standard Library Expansion

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
| CSV encode/decode (RFC 4180) | Planned | Import/export |
| FTS5 search wrapper | Planned | Full-text search stdlib |
| RBAC (role-based access control) | Planned | Permission middleware |
| Email (SMTP / API) | In progress | Outbound notifications (C cap + stdlib) |
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

#### Phase 1: Foundation (agent core + CLI) — In Progress

| Feature | Status | Notes |
|---------|--------|-------|
| `hull agent status` | Planned | Dev server state, PID, port, last reload result, uptime |
| `hull agent errors` | Planned | Structured errors from `.hull/last_error.json` sidecar |
| `hull agent routes` | Planned | List registered routes as JSON (method, pattern, middleware) |
| `hull agent request` | Planned | HTTP request to dev server with JSON response |
| `hull agent db schema` | Planned | Introspect current DB tables, columns, types, PKs |
| `hull agent db query` | Planned | Read-only query on dev DB with JSON output |
| `hull agent test` | Planned | Structured test results (passed, failed, failure details) |
| `hull dev --agent` | Planned | Write structured errors/status to `.hull/` sidecar files |
| `AGENTS.md` | Planned | Comprehensive agent development guide |

#### Phase 2: Context + Render

Dynamic context system — `hull agent context` assembles task-relevant documentation on demand, sized for the model's context window.

| Feature | Status | Notes |
|---------|--------|-------|
| `hull agent context --task=T --level=L` | Planned | Dynamic docs: domains (auth, db, middleware, etc.) × levels (minimal/compact/full) |
| `hull render` | Planned | Offline template rendering without running server |
| `agents/context/*.md` | Planned | Per-domain knowledge files consumed by context system |
| `--model-size` auto-selection | Planned | Auto-select context level based on model size (7B→minimal, 70B→compact, frontier→full) |

Context levels:

| Level | Size | Target Models |
|-------|------|---------------|
| `minimal` | ~1K tokens | Small local (7–14B): API signatures, one-liner patterns |
| `compact` | ~4K tokens | Mid-range local (30–70B): signatures + patterns + gotchas + one example |
| `full` | ~12K tokens | Frontier (Claude, GPT-4): comprehensive with multiple examples, edge cases |

#### Phase 3: MCP Server + Agent Configs

| Feature | Status | Notes |
|---------|--------|-------|
| `hull mcp serve` | Planned | stdio MCP server wrapping agent core (Claude Code, Cursor) |
| `hull mcp serve --sse` | Planned | SSE transport for network-accessible agents |
| `.cursorrules` | Planned | Cursor/Windsurf agent rules |
| `codex.md` | Planned | Codex-specific instructions |
| `.opencode.yml` | Planned | OpenCode config with MCP server reference |
| Updated `CLAUDE.md` | Planned | MCP server setup, hull agent commands |

#### Phase 4: Lifecycle + Monitoring

| Feature | Status | Notes |
|---------|--------|-------|
| `hull agent scaffold` | Planned | Project scaffolding from templates with structured output |
| `hull agent build` | Planned | Structured build output (binary path, size, platform) |
| `hull agent migrate` | Planned | Structured migration status/apply |
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
| WASM compute plugins (WAMR) | Architecture designed | Sandboxed, gas-metered, no I/O — pure computation |
| Database encryption at rest | Planned | SQLite SEE or custom VFS |
| Background work / coroutines | Planned | `app.every()`, `app.daily()` |
| Compression (gzip/zstd) | [Plan](compression_plan.md) | Response compression middleware |
| ETag support | [Plan](etag_plan.md) | Conditional request handling |
| HTTP/2 full support | [Plan](http2_plan.md) | Currently h2c upgrade only |
| PDF document builder | Planned | Report generation |

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
| Critical | kqueue READ\|WRITE bitmask (C-1) | HTTP/2 broken on macOS |
| Critical | WebSocket partial writes (C-2) | Frame corruption on non-blocking sockets |
| High | Protocol upgrade partial writes (H-3, H-4) | 101 response corruption |
| High | Private key material not zeroed (H-2) | Key residue in heap |
| High | writev_all busy-spin on EAGAIN (H-5) | Event loop starvation |
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
