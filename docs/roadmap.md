# Hull — Roadmap & Benchmarks

## What's Built

- Lua 5.4 + QuickJS runtimes with HTTP route dispatch
- SQLite with parameterized queries (injection-proof)
- Request body reading + route parameter extraction
- Crypto: SHA-256, PBKDF2, random bytes, password hash/verify, Ed25519 (sign/verify/keypair)
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

### Build Pipeline & Security — MVP Roadmap

**Phase 1: TweetNaCl + Ed25519** ✓
- [x] Vendor `tweetnacl.c` + `tweetnacl.h` (770 lines, public domain) to `vendor/tweetnacl/`
- [x] Implement `hl_cap_crypto_ed25519_verify()` in `crypto.c` (replace stub)
- [x] Add `hl_cap_crypto_ed25519_sign()` in `crypto.c`
- [x] Add `hl_cap_crypto_ed25519_keypair()` in `crypto.c`
- [x] All TweetNaCl calls wrapped behind `hl_cap_crypto_*` — only `crypto.c` includes `tweetnacl.h`
- [x] Expose to Lua: `crypto.ed25519_sign()`, `crypto.ed25519_verify()`, `crypto.ed25519_keypair()`
- [x] Add tweetnacl.o to Makefile VENDOR_OBJS

**Phase 2: Manifest System** ✓
- [x] Add `app.manifest(table)` in `modules.c` — stores in `__hull_manifest` registry key
- [x] Add `app.get_manifest()` — retrieves stored manifest table
- [x] Create `include/hull/manifest.h` — HlManifest struct
- [x] Create `src/hull/manifest.c` — `hl_manifest_extract()` reads Lua registry → C struct
- [x] Wire manifest extraction into `main.c` (logging manifest contents after app load)

**Phase 3: Subcommand Routing + Tool Mode** ✓
- [x] Extract existing server logic into `hull_serve()`
- [x] Add subcommand dispatch: `build`, `verify`, `inspect`, `manifest`, `keygen`
- [x] Create `include/hull/tool.h` + `src/hull/tool.c` — tool mode Lua VM (unsandboxed)
- [x] Add `int sandbox` flag to `HlLuaConfig` (default: 1)
- [x] Implement `hull_keygen()` — pure C Ed25519 keypair generation
- [x] Pass CLI args as Lua `arg` global table

**Phase 4: Platform Library Embedding** ✓
- [x] New Makefile target: `libhull_platform.a` (3.2MB, all .o except main/tool/build_assets)
- [x] `make EMBED_PLATFORM=1` — xxd .a + templates into build_assets.c
- [x] Create `src/hull/build_assets.c` + `include/hull/build_assets.h` — extraction API
- [x] Create `templates/app_main.c` — main() → hull_main() trampoline
- [x] Create `templates/entry.h` — HlStdlibEntry type definition
- [x] Replace `#ifdef HL_APP_EMBEDDED` with default/override pattern for `hl_app_lua_entries`
- [x] Create `src/hull/app_entries_default.c` — empty sentinel array

**Phase 5: Lua Build/Verify/Inspect/Manifest Scripts** ✓
- [x] Create `stdlib/lua/hull/manifest.lua` — extract + print manifest as JSON
- [x] Create `stdlib/lua/hull/build.lua` — full build + sign:
  - Extract platform .a, collect app files, generate app_registry.c (xxd in Lua)
  - Generate app_main.c from template, invoke CC, link, sign
- [x] Create `stdlib/lua/hull/verify.lua` — app + platform signature verification
- [x] Create `stdlib/lua/hull/inspect.lua` — display capabilities + signature status
- [x] Define `hull.sig` JSON format (version, files, manifest, signature, public_key)
- [x] Rename `main()` → `hull_main()` (exported from platform .a); thin entry.c for hull binary
- [x] Canonical JSON encoding (sorted keys) for deterministic signatures

**Phase 6: pledge/unveil Sandbox**
- [ ] Vendor cosmopolitan libc pledge/unveil polyfill for Linux (seccomp-bpf + landlock)
- [ ] Create `include/hull/sandbox.h` + `src/hull/sandbox.c` — `hl_pledge()`, `hl_unveil()` wrappers
- [ ] Derive pledge promises from manifest (fs → rpath/wpath, hosts → inet/dns)
- [ ] Call unveil/pledge in `main.c` after manifest extraction, before event loop
- [ ] No-op on macOS (C-level validation handles it); native on Cosmopolitan builds

**Phase 7: Server-side Signature Verification**
- [ ] Add `--verify-sig pubkey.pub` CLI flag to server mode
- [ ] On startup: read `hull.sig`, verify Ed25519 against provided pubkey
- [ ] Refuse to start if signature is invalid

### Build Tool — Future Enhancements

- [ ] `hull new` — project scaffolding
- [ ] `hull dev` — hot-reload development server
- [ ] `hull test` — run app tests
- [ ] `hull build --self` — self-hosting (rebuild hull from embedded source)
- [ ] License key system (Ed25519 offline verification)
- [ ] Database backup/restore
- [ ] `hull eject` — export to standalone Makefile project

### Advanced

- [ ] WASM compute plugins (WAMR)
- [ ] Database encryption at rest
- [ ] Background work / coroutines

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
