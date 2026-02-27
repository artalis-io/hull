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
- Tool hardening: shell-free `tool.spawn` with compiler allowlist, `tool.find_files`, `tool.copy`, `tool.rmdir` with unveil path validation
- Command module architecture: table-driven dispatcher (`src/hull/commands/`)
- `hull test` subcommand: in-process test runner for Lua and JS apps (no TCP, direct router dispatch)
- JS manifest parity: `app.manifest()` + sandbox enforcement for QuickJS apps
- Cosmo sandbox test coverage: `sandbox_violation.c` compiles/runs under cosmocc
- Self-build chain: `make self-build` proves hull→hull2→hull3 reproducibility

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

**Phase 6: pledge/unveil Sandbox** ✓
- [x] Vendor jart/pledge polyfill for Linux (seccomp-bpf + landlock)
- [x] Create `include/hull/sandbox.h` + `src/hull/sandbox.c` — platform-dispatching sandbox
- [x] Derive pledge promises from manifest (fs → rpath/wpath, hosts → inet/dns)
- [x] Call unveil/pledge in `main.c` after manifest extraction, before event loop
- [x] No-op on macOS (C-level validation handles it); native on Cosmopolitan builds
- [x] E2E sandbox tests + standalone violation test for CI

**Phase 7: Post-MVP Hardening** ✓

Phase 7a: Cosmo sandbox test coverage ✓
- [x] Add `__COSMOPOLITAN__` code path to `tests/sandbox_violation.c` (extern decls for pledge/unveil)
- [x] Refactor platform detection: `SANDBOX_SUPPORTED` + `HAS_PLEDGE_MODE` macros
- [x] Update `tests/e2e_sandbox.sh` to compile violation test with cosmocc (no polyfill objects needed)

Phase 7b: QuickJS manifest parity ✓
- [x] Add `app.manifest(obj)` + `app.getManifest()` to JS `hull:app` module (`src/hull/runtime/js/modules.c`)
- [x] Add `hl_manifest_extract_js()` to `src/hull/manifest.c` — read `globalThis.__hull_manifest` into HlManifest
- [x] Wire manifest extraction + sandbox into `main.c` QuickJS path (after `wire_js_routes`, before event loop)
- [x] Unit tests in `tests/hull/runtime/js/test_js.c`, extend `e2e_sandbox.sh` for JS manifest app

Phase 7c: Tool mode hardening ✓
- [x] Shell-free `tool.spawn(argv)` with compiler allowlist (cc, gcc, clang, cosmocc, cosmoar, ar) — no `system()`/`popen()`
- [x] `tool.find_files(dir, pattern)` — pure C recursive `opendir`/`fnmatch`, skips dotdirs/vendor/node_modules
- [x] `tool.copy(src, dst)`, `tool.rmdir(path)` — pure C, no shell
- [x] Unveil path validation on all filesystem tool functions (mandatory, not optional)
- [x] Configurable compiler via `tool.cc` (default: `cosmocc`, overridable with `--cc`)
- [x] Removed `tool.exec()` and `tool.read()` entirely
- [x] Migrated `build.lua` to shell-free tool API
- [x] Unit tests: `tests/hull/cap/test_tool.c` (allowlist, find_files, copy, rmdir, unveil enforcement)

Phase 7d: Self-build test ✓
- [x] Create `tests/fixtures/null_app/app.lua` (minimal fixture)
- [x] Add `make self-build` target: hull→hull2→hull3, verify keygen at each step
- [x] Add Step 13 to `tests/e2e_build.sh`: self-build chain test

Phase 7e: Command module architecture ✓
- [x] Table-driven dispatcher in `src/hull/commands/dispatch.c` — one line per command
- [x] Each command in its own `.c`/`.h` under `src/hull/commands/` (keygen, build, verify, inspect, manifest, test)
- [x] `hull_main()` reduced to `hl_command_dispatch()` + fallback to `hull_serve()`
- [x] Unit tests: `tests/hull/commands/test_dispatch.c`

Phase 7f: `hull test` subcommand ✓
- [x] In-process test runner — no TCP, no pipes, direct `kl_router_match` + handler dispatch
- [x] Supports both Lua (`test_*.lua`) and JS (`test_*.js`) test files
- [x] Recursive test discovery via `hl_tool_find_files()`
- [x] `test("desc", fn)` registration + `test.get/post/put/delete/patch` HTTP dispatch
- [x] `test.eq(a, b)`, `test.ok(val)`, `test.err(fn, pattern)` assertions
- [x] `:memory:` SQLite for test isolation
- [x] Refactored route wiring out of `main.c` into `runtime/lua.h` and `runtime/js.h`
- [x] Unit tests: `tests/hull/cap/test_test.c` (planned)

**Phase 8: Server-side Signature Verification**
- [ ] Add `--verify-sig pubkey.pub` CLI flag to server mode
- [ ] On startup: read `hull.sig`, verify Ed25519 against provided pubkey
- [ ] Refuse to start if signature is invalid

### Build Tool — Future Enhancements

- [ ] `hull new` — project scaffolding
- [ ] `hull dev` — hot-reload development server
- [x] `hull test` — in-process test runner (Lua + JS)
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
