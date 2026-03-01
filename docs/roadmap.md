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
- `hull.session` — server-side SQLite-backed sessions with sliding expiry
- `hull.jwt` — JWT HS256 sign/verify/decode (no "none" algorithm, constant-time comparison)
- `hull.csrf` — stateless CSRF tokens via HMAC-SHA256
- `hull.auth` — authentication middleware factories (session auth, JWT Bearer auth)

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
- E2E tests for all 8 examples in both runtimes
- Sandbox violation tests (Linux + Cosmo)
- Benchmarks (Lua vs QuickJS, DB vs non-DB routes)

## Roadmap

### Next — Standard Library Expansion

| Feature | Status | Notes |
|---------|--------|-------|
| Template engine (`{{ }}` HTML templates) | Planned | Server-side rendering |
| Input validation (schema-based) | Planned | Declarative field validation |
| Rate limiting middleware | Planned | Token bucket or sliding window |
| Static file serving (`app.static("/public")`) | Planned | With caching headers |
| CSV encode/decode (RFC 4180) | Planned | Import/export |
| FTS5 search wrapper | Planned | Full-text search stdlib |
| i18n (locale detection + translations) | Planned | Message bundles |
| RBAC (role-based access control) | Planned | Permission middleware |
| Email (SMTP / API) | Planned | Outbound notifications |
| License key system | Planned | Ed25519 offline verification for commercial distribution |

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
