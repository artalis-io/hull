# Hull — Roadmap & Benchmarks

## What's Built

- Lua 5.4 + QuickJS runtimes with HTTP route dispatch
- SQLite with parameterized queries (injection-proof)
- Request body reading + route parameter extraction
- Crypto: SHA-256, PBKDF2, random bytes, password hash/verify
- Filesystem: sandboxed read/write/exists/delete
- Time, env, logging modules
- Keel HTTP server (epoll/kqueue/poll)
- Cosmopolitan APE cross-platform builds
- CI pipeline (Linux, macOS, ASan, MSan, static analysis, Cosmo) + benchmarks

## Roadmap

### High Impact — Unlocks Real Apps

1. **JSON module** — `json.encode()` / `json.decode()`
2. **HTTP client** — `http.get()` / `http.post()` (cap declared, not wired)
3. **Session + CSRF** — cookie sessions, CSRF tokens
4. **Template engine** — `{{ }}` HTML templates
5. **Validation** — input schema validation
6. **Rate limiting** — middleware
7. **Static file serving** — `app.static("/public")`

### Medium Impact — Production Features

8. **Email** — SMTP / API providers (cap declared, not wired)
9. **Search** — FTS5 wrapper
10. **CSV** — RFC 4180 encode/decode
11. **RBAC** — role-based access control
12. **i18n** — locale detection + translations
13. **PDF** — document builder
14. **Multipart / file uploads**
15. **WebSockets**
16. **Scheduled tasks** — `app.every()`, `app.daily()`

### Build Tool — The `hull` CLI

17. `hull new`, `hull build`, `hull dev` (hot-reload), `hull test`
18. App signing + verification (`hull build --sign`, `hull inspect`)
19. License key system
20. Database backup/restore
21. `hull eject`

### Advanced

22. WASM compute plugins (WAMR)
23. Database encryption at rest
24. Background work / coroutines

## Benchmark Baseline

Measured on GitHub Actions Ubuntu runner (2 threads, 50 connections, 5s duration via `wrk`).
Commit: `f1e3996` (2026-02-26).

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

### Summary

- Lua is ~1.9x faster than QuickJS across all benchmarks.
- Non-DB routes sustain 50k-100k req/s on a single CI VM core with sub-millisecond average latency.
- DB-bound routes (SQLite write per request) sustain 5k-7k req/s.
