# HULL — Development Guide

## Build

```bash
make                    # build hull binary (epoll on Linux, kqueue on macOS)
make test               # build and run all unit tests
make e2e                # end-to-end tests (all examples, both runtimes)
make debug              # debug build with ASan + UBSan (recompiles from clean)
make msan               # MSan + UBSan (Linux clang only)
make check              # full validation: clean + ASan + test + e2e
make analyze            # Clang static analyzer (scan-build)
make cppcheck           # cppcheck static analysis
make platform           # build libhull_platform.a (everything except main/build-tool code)
make platform-cosmo     # build multi-arch cosmo platform archives (x86_64 + aarch64)
make self-build         # reproducible build chain: hull → hull2 → hull3
make CC=cosmocc         # build with Cosmopolitan (APE binary)
make EMBED_PLATFORM=1   # embed platform library in hull binary (distribution mode)
make EMBED_PLATFORM=cosmo  # embed multi-arch cosmo platform (distribution mode)
make wamrc              # build WAMR AOT compiler (requires cmake + LLVM)
make bench-wasm         # WASM compute benchmark (native vs interpreter vs AOT)
make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu  # build with GPU compute (wgpu-native)
make bench-gpu HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu  # GPU vs WASM vs native benchmark
make clean              # remove all build artifacts
```

### Dependencies

All vendored — no external dependencies:

| Library | Location | Purpose |
|---------|----------|---------|
| Keel | `vendor/keel/` (git submodule) | HTTP server library (async primitives, thread pool) |
| Lua 5.4 | `vendor/lua/` | Application scripting |
| QuickJS | `vendor/quickjs/` | ES2023 JavaScript runtime |
| SQLite | `vendor/sqlite/` | Embedded database |
| mbedTLS | `vendor/mbedtls/` | TLS client |
| TweetNaCl | `vendor/tweetnacl/` | Ed25519 + NaCl crypto |
| pledge/unveil | `vendor/pledge/` | Linux kernel sandbox polyfill |
| log.c | `vendor/log.c/` | Logging |
| sh_arena | `vendor/sh_arena/` | Arena allocator |
| sh_json | `vendor/sh_json/` | Streaming JSON writer + arena-based parser |
| WAMR | `vendor/wamr/` (git submodule) | WebAssembly Micro Runtime (compute plugins) |
| wgpu-native | `vendor/wgpu/` | GPU compute backend (optional, `HL_ENABLE_GPU=1`) |
| utest.h | `vendor/utest.h` | Unit test framework |

## Project Structure

```
include/hull/           # Public headers
  cap/                  #   Capability module headers (db.h, fs.h, crypto.h, etc.)
  commands/             #   Command headers (build.h, test.h, verify.h, etc.)
  runtime/              #   Runtime headers (lua.h, js.h)
src/hull/               # Core source
  cap/                  #   Capability implementations (db.c, fs.c, crypto.c, http.c, tool.c, etc.)
  commands/             #   Subcommand implementations (build.c, test.c, verify.c, etc.)
  runtime/lua/          #   Lua 5.4 runtime (bindings.c, modules.c, runtime.c)
  runtime/js/           #   QuickJS runtime (bindings.c, modules.c, runtime.c)
  vfs.c                 #   Unified Virtual Filesystem (O(log n) binary search over HlEntry arrays)
  static.c              #   Static file serving middleware (/static/* convention)
stdlib/                 # Embedded standard library
  lua/hull/             #   Lua modules (json, cookie, session, jwt, csrf, auth, build, verify, etc.)
  js/hull/              #   JS modules (cookie, session, jwt, csrf, auth, verify)
vendor/                 # Vendored libraries (do not modify)
tests/                  # Unit tests (test_*.c) and E2E scripts (e2e_*.sh)
  fixtures/             #   Test fixtures (null_app, etc.)
  hull/                 #   Hull-specific test suites
examples/               # 11 example apps (hello, rest_api, auth, jwt_api, todo, compute, etc.)
docs/                   # Architecture, security, roadmap, audit documentation
templates/              # Build templates (app_main.c, entry.h)
```

## Architecture

### System Layers

```
Application Code (Lua/JS)  →  Standard Library (stdlib/)
        ↓
Runtimes (Lua 5.4 / QuickJS)  →  Sandboxed interpreters
        ↓
Capability Layer (src/hull/cap/)  →  C enforcement boundary
        ↓
Hull Core (main.c, manifest.c, sandbox.c, signature.c, static.c, vfs.c)
        ↓
Keel HTTP Server (vendor/keel/)  →  Event loop + routing + async + thread pool
        ↓
Kernel Sandbox (pledge/unveil/seatbelt)  →  OS enforcement
```

Each layer talks only to the one below it. Application code cannot bypass capabilities.

### Orchestration vs Compute

Hull separates control-plane orchestration from data-plane computation:

- **Orchestration (Lua/JS):** Request handling, routing, middleware, database queries, template rendering. Runs in sandboxed Lua 5.4 or QuickJS interpreters with capability-mediated system access.
- **Compute (WASM, planned):** CPU-intensive data processing (scoring, transformation, deduplication). Runs in WAMR's isolated linear memory with no I/O imports, gas-metered execution, and configurable memory caps. See `docs/wamr_architecture.md`.

This separation means orchestration code has full capability access (mediated by the C layer), while compute plugins are pure functions with no side effects.

### Dual-Runtime Design

Hull supports Lua 5.4 and QuickJS (ES2023). Only one is active per application — selected by entry point extension (`.lua` or `.js`). Both runtimes implement the same polymorphic vtable (`HlRuntimeVtable`) and call the same C capability functions.

### Capability Layer (`hl_cap_*`)

All system access is mediated by C capability functions. Neither runtime touches SQLite, filesystem, or network directly.

| Module | File | Key Functions |
|--------|------|---------------|
| Database | `cap/db.c` | `hl_cap_db_query()`, `hl_cap_db_exec()`, `hl_cap_db_begin/commit/rollback()` |
| Filesystem | `cap/fs.c` | `hl_cap_fs_read()`, `hl_cap_fs_write()`, `hl_cap_fs_exists()`, `hl_cap_fs_delete()` |
| Crypto | `cap/crypto.c` | SHA-256/512, HMAC, PBKDF2, Ed25519, secretbox, box, random |
| HTTP client | `cap/http.c` | `hl_cap_http_request()` with host allowlist |
| Environment | `cap/env.c` | `hl_cap_env_get()` with manifest allowlist |
| Time | `cap/time.c` | `hl_cap_time_now()`, `_now_ms()`, `_clock()`, `_date()`, `_datetime()` |
| Tool (build mode) | `cap/tool.c` | `hl_tool_spawn()`, `hl_tool_find_files()`, `hl_tool_copy()`, `hl_tool_mkdir()` |
| Test | `cap/test.c` | In-process HTTP dispatch, assertions |
| Body | `cap/body.c` | Request body handling |
| WASM compute | `cap/wasm.c` | `hl_cap_wasm_init()`, `_load()`, `_call()` — WAMR compute plugins |
| GPU compute | `cap/gpu.c`, `cap/gpu_wgpu.c` | `hl_cap_gpu_init()`, `_compile()`, `_dispatch()` — wgpu-native compute shaders |
| Audit | `cap/audit.c` | Structured capability audit logging (JSON to stderr) |

### Request Flow

```
Client → Keel HTTP → Route Match → hl_{lua,js}_dispatch() → Handler → KlResponse
                                           ↓
                                    hl_cap_* API (shared C)
                                           ↓
                                    SQLite / FS / Crypto / HTTP
```

### Command Dispatch

Table-driven dispatcher in `src/hull/commands/dispatch.c`. 19 commands:

```
hull keygen | build | verify | inspect | manifest | test | new | init | dev | eject | sign-platform | migrate | agent | mcp | check | compute | deploy | version | doctor
Runtime flags: --audit (capability audit logging), --agent (sidecar files), --no-migrate, --no-sandbox, --skip-ca-bundle
Global flags: --version / -v (equivalent to hull version)
```

Each command is a separate `.c`/`.h` under `src/hull/commands/`. Adding a new command = one line in the table + one source file.

**`hull init [dir] [--runtime lua|js]`** — Initialize a hull project in-place. Like `git init`: creates missing files (`app.lua`, `tests/`, `migrations/`, `.gitignore`) without touching existing ones. Detects existing runtime from `app.lua`/`app.js` presence. Implemented as a Lua tool module (`stdlib/lua/hull/init.lua`).

**`hull doctor [--json]`** — Environment check for distribution readiness. Reports hull version/runtime/platform, whether the platform library is embedded (none / single-arch / multi-arch), whether TinyCC is embedded, and which system C compilers (`cc`, `gcc`, `clang`, `cosmocc`) are found in PATH. Exits 0 only when `hull build` is fully ready (platform embedded AND at least one compiler available). Pure C implementation (`src/hull/commands/doctor.c`). `--json` for machine-readable output.

**`hull build --compiler=<backend>`** — Select the C compiler backend for `hull build`. Options: `tcc` (embedded TinyCC, compile-only), `system` (system cc/gcc/clang, no tcc fallback), or an explicit compiler path. Default: embedded TinyCC if available, otherwise system cc. The compiler abstraction uses `HlCompilerVtable` (`include/hull/compiler.h`); backends live in `src/hull/compiler.c` and `src/hull/compiler_tcc.c`.

### Agent Tooling (`hull agent`)

Machine-readable introspection for AI coding agents. All output is JSON to stdout.

```bash
hull agent routes [app_dir]              # list routes + middleware
hull agent db schema [app_dir] [-d path] # introspect DB schema
hull agent db query "SQL" [app_dir]      # run read-only SQL query
hull agent request METHOD PATH [opts]    # HTTP request to dev server
hull agent status [app_dir] [-p port]    # check dev server status
hull agent errors [app_dir]              # structured errors from last reload
hull agent test [app_dir]                # run tests with JSON output
hull agent context --task=T --level=L    # task-relevant documentation
hull agent migrate [app_dir] [-d path]   # migration status
hull agent deploy [app_dir]              # deployment readiness analysis
```

`hull dev --agent` enables sidecar files: `.hull/dev.json` (port, PID) on start, `.hull/last_error.json` on load failure. See [AGENTS.md](AGENTS.md) for the full agent development guide.

### Migration System

SQL migrations provide versioned schema management for SQLite databases.

| Component | File | Purpose |
|-----------|------|---------|
| Migration runner | `src/hull/migrate.c`, `include/hull/migrate.h` | Core migration execution engine (uses VFS prefix query) |
| CLI command | `src/hull/commands/migrate.c` | `hull migrate` subcommand |
| Scaffolding | `stdlib/lua/hull/migrate.lua` | `hull migrate new` template generation |
| Auto-run (dev) | `main.c` | Runs pending migrations on startup |
| Auto-run (test) | `test.c` | Runs migrations against `:memory:` database |
| Embedding | `build.lua` | Embeds `migrations/*.sql` in built binaries |

**Convention:** `migrations/*.sql` files numbered `001_`, `002_`, etc. Each runs in `BEGIN IMMEDIATE` / `COMMIT`. The `_hull_migrations` table tracks applied migrations (name + checksum + timestamp). Opt out with `--no-migrate`.

**Commands:**
- `hull migrate [app_dir]` — run pending migrations
- `hull migrate status` — show applied/pending
- `hull migrate new <name>` — create numbered migration file

### Virtual Filesystem (VFS)

All embedded file lookups go through a unified VFS module (`src/hull/vfs.c`, `include/hull/vfs.h`). Two VFS instances are created at startup:

| Instance | Entries | root_dir | Used by |
|----------|---------|----------|---------|
| `app_vfs` | `hl_app_entries[]` | `app_dir` | templates, static, migrations, app modules, signature |
| `platform_vfs` | `hl_stdlib_entries[]` | NULL | Lua/JS stdlib module loading |

Both are stored in `HlRuntime` and accessible to all consumers.

**API:**
- `hl_vfs_find(vfs, name)` — O(log n) exact lookup (binary search)
- `hl_vfs_prefix(vfs, prefix, &first)` — O(log n) prefix query (returns count + pointer to first match)
- `hl_vfs_has_prefix(vfs, prefix)` — O(log n) prefix existence check
- `hl_vfs_path(vfs, name, buf, size)` — filesystem path construction (`root_dir/name`)

**Build-time requirement:** Entry arrays must be sorted by name in C `strcmp` order (the Makefile uses `LC_ALL=C sort`). `hl_vfs_init()` debug-asserts sorted order.

**Consumers:**
- Static serving: `hl_vfs_find(vfs, "static/style.css")`
- Migrations: `hl_vfs_prefix(vfs, "migrations/", &first)`
- Templates: `hl_vfs_find(vfs, "templates/base.html")`
- Module loading: `hl_vfs_find(vfs, "hull:cookie")` (JS), `hl_vfs_find(vfs, "hull.json")` (Lua)
- Signature: `hl_vfs_find(vfs, "./app.lua")` for hash verification

## Platform Builds

### Standard Build (Linux/macOS)

```bash
make                    # builds build/hull
make platform           # builds build/libhull_platform.a
make EMBED_PLATFORM=1   # embeds platform in hull for distribution
```

### Cosmopolitan APE Build

Cosmopolitan produces fat APE binaries that run on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from a single file.

**How cosmocc works:**
- `cosmocc` runs two separate link passes (x86_64 + aarch64), then combines with `apelink`
- Uses `.aarch64/` directory convention: for every `foo.o`, a `.aarch64/foo.o` exists
- Arch-specific tools: `x86_64-unknown-cosmo-cc`, `aarch64-unknown-cosmo-cc`

**Multi-arch platform build:**

```bash
# Build both x86_64 and aarch64 platform archives
make platform-cosmo

# This creates:
#   build/libhull_platform.x86_64-cosmo.a
#   build/libhull_platform.aarch64-cosmo.a
#   build/platform_cc  (contains "cosmocc")

# Then build hull with cosmocc
make CC=cosmocc
```

`platform-cosmo` internally:
1. `make clean && make platform CC=x86_64-unknown-cosmo-cc` → copies to staging
2. `make clean && make platform CC=aarch64-unknown-cosmo-cc` → copies to staging
3. Cleans build artifacts, copies both archives to `build/`

**Keel Cosmo detection:**
- Keel's Makefile detects the cosmo toolchain via `ifneq ($(findstring cosmo,$(CC)),)`
- Sets `COSMO=1`: forces poll backend, omits `-fstack-protector-strong`
- Sets `COSMO_FAT=1` only when `CC=cosmocc`: creates `.aarch64/libkeel.a` counterpart
- Uses plain `ar` (not `cosmoar` — cosmoar fails with recursive `.aarch64/` lookups)

**hull build with cosmo:**
- `build.lua` detects `is_cosmo = cc:find("cosmocc")`
- Searches for both arch-specific archives in `build/` or hull binary directory
- Copies `x86_64-cosmo.a` → `tmpdir/libhull_platform.a`
- Copies `aarch64-cosmo.a` → `tmpdir/.aarch64/libhull_platform.a`
- `cosmocc` automatically finds the `.aarch64/` counterpart during linking

**Embedding for distribution:**
```bash
make platform-cosmo
make CC=cosmocc EMBED_PLATFORM=cosmo  # embeds both arch archives
```

### CI Configuration

The Cosmo CI job in `.github/workflows/ci.yml`:
1. Installs cosmocc from `cosmo.zip/pub/cosmocc/cosmocc.zip`
2. `make platform-cosmo` — builds both arch platform archives
3. `make CC=cosmocc` — builds hull as APE binary
4. `make test CC=cosmocc` — runs unit tests
5. E2E smoke test + sandbox tests

## Security

### Manifest & Sandbox

Two-phase sandbox in `sandbox.c`:

**Phase 1** — `hl_sandbox_apply_pledge()`: Called before `load_app()`. On Linux/Cosmo, pledges `stdio inet rpath wpath cpath flock dns unveil` — blocks `exec`, `proc`, `fork` during module loading. On macOS, phase 1 is a no-op (Seatbelt's `sandbox_init` is irreversible, so the full profile is applied in phase 2).

**Phase 2** — `hl_sandbox_apply()`: Called after manifest extraction. Platform-specific enforcement:
- **Linux/Cosmo:** Unveils specific paths, seals filesystem, applies pledge syscall filter
- **macOS:** Builds dynamic SBPL profile from manifest, applies via `sandbox_init_with_parameters()`. Deny-default with selective allows for app_dir, db files, manifest paths, network.

Violation = SIGABRT on OpenBSD, SIGKILL on Linux/Cosmo, EPERM on macOS. `--no-sandbox` flag disables kernel enforcement for debugging.

### Capability Enforcement Invariants

- **SQL injection impossible:** All DB access uses `sqlite3_bind_*` parameterized binding. SQL is always a literal string.
- **Internal tables protected:** `hl_cap_db_check_namespace()` blocks user code from accessing `_hull_*` tables. Enforcement uses call-stack inspection — Lua checks `ar.source` for `hull.` prefix, JS checks module name for `hull:` prefix — so stdlib modules transparently bypass the check via normal `db.exec`/`db.query`. No internal API is exposed. Tables: `_hull_outbox`, `_hull_inbox_processed`, `_hull_idempotency_keys`, `_hull_sessions`.
- **Path traversal blocked:** `hl_cap_fs_validate()` rejects absolute paths, `..` components, symlink escapes via `realpath()` ancestor check. Plus kernel unveil.
- **Host allowlist enforced:** `hl_cap_http_request()` validates target host against manifest's `hosts` array.
- **Env allowlist enforced:** `hl_cap_env_get()` checks against manifest's `env` array (max 32 entries).
- **No shell invocation:** Tool mode uses `hl_tool_spawn()` with compiler allowlist. No `system()`/`popen()`.
- **Key material zeroed:** `hull_secure_zero()` (volatile memset) scrubs crypto material from stack buffers.
- **Instruction limits:** Both Lua and JS runtimes enforce per-request instruction limits (default 100M). Lua uses `lua_sethook(LUA_MASKCOUNT)`, JS uses `JS_SetInterruptHandler`. Override with `--max-instructions N` or `HULL_MAX_INSTRUCTIONS` env var.
- **Audit logging:** `--audit` flag or `HULL_AUDIT=1` env var enables structured JSON logging of all capability calls to stderr. Off by default (zero overhead — single branch on `hl_audit_enabled` global). Uses `ShJsonWriter` for streaming output with proper escaping. No heap allocation.

### Signature System

Dual-layer Ed25519:
- **Platform layer (inner):** Signed by gethull.dev key. Proves platform library is authentic.
- **App layer (outer):** Signed by developer key. Proves app hasn't been tampered with.

See [docs/security.md](docs/security.md) for the full attack model.

### Keel Audit

Run `/c-audit` to perform a comprehensive C code audit on the Keel HTTP server library. The audit checks for memory safety, input validation, resource management, integer overflow, network security, dead code, and build hardening. Results are in [docs/keel_audit.md](docs/keel_audit.md).

Key findings to be aware of:
- WebSocket and HTTP/2 upgrade code has partial-write issues (C-2, H-3, H-4)
- kqueue event_mod doesn't support READ|WRITE bitmask (C-1) — affects HTTP/2 on macOS
- Private key material should be zeroed before free in tls_mbedtls.c (H-2)

## Key Types

| Type | Header | Purpose |
|------|--------|---------|
| `HlValue` | `cap/types.h` | Runtime-agnostic value (nil, int, double, text, blob, bool) |
| `HlColumn` | `cap/types.h` | Named column + value (query results) |
| `HlRowCallback` | `cap/types.h` | Per-row callback for db_query() |
| `HlManifest` | `manifest.h` | Declared capabilities (fs paths, env vars, hosts) |
| `HlRuntime` | `runtime.h` | Polymorphic runtime context |
| `HlRuntimeVtable` | `runtime.h` | Runtime interface (init, load, wire_routes, extract_manifest, destroy) |
| `HlLua` | `runtime/lua.h` | Lua 5.4 context (VM, config, capabilities) |
| `HlJS` | `runtime/js.h` | QuickJS context (VM, config, capabilities) |
| `HlVfs` | `vfs.h` | Unified VFS: sorted HlEntry array with O(log n) find, prefix query, path construction |
| `HlGpuCtx` | `cap/gpu.h` | GPU compute context: backend vtable, device array, pipeline/buffer caches |
| `HlEmbeddedPlatform` | `build_assets.h` | Multi-arch embedded platform entry (arch, data, len) |

## Git

- When committing, do NOT add any Co-Authored-By trailers.
- Do NOT add "Generated with Claude Code" or similar attribution to PRs.

## Conventions

- C11, compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror`
- `-fstack-protector-strong` for buffer overflow detection (not Cosmo)
- Vendor code compiled with `-w` (relaxed warnings, no `-Werror`)
- Integer overflow guards: check against `SIZE_MAX/2` before arithmetic
- Error handling: return `-1` on failure, `0` on success (or positive value)
- Resource cleanup: every `_init` has a corresponding `_free`
- All SQLite access through `hl_cap_db_*` — never call sqlite3 directly from bindings
- All filesystem access through `hl_cap_fs_*` — never call open/read/write directly from runtimes
- Public Hull functions prefixed with `hl_` (capabilities: `hl_cap_*`, tools: `hl_tool_*`, commands: `hl_cmd_*`)
- Keel functions prefixed with `kl_` (see vendor/keel/CLAUDE.md)

## Stdlib Middleware

### Middleware Factory Pattern

All middleware modules follow the same contract:

```lua
local mod = require("hull.<module>")
local mw = mod.middleware(opts)   -- factory returns a middleware function
-- mw signature: function(req, res) -> 0 | 1
--   0 = continue to next middleware / handler
--   1 = short-circuit (response already sent)
```

Register with `app.use(method, pattern, mw)`:
- `"*"` method = match any method
- `"/*"` pattern = prefix match all paths
- `"/api/*"` = prefix match under `/api/`

### Module Reference

| Module | Lua | JS | Purpose |
|--------|-----|-----|---------|
| `cors` | `hull.middleware.cors` | `hull:middleware:cors` | CORS headers + preflight handling |
| `ratelimit` | `hull.middleware.ratelimit` | `hull:middleware:ratelimit` | In-memory rate limiting with configurable windows |
| `csrf` | `hull.middleware.csrf` | `hull:middleware:csrf` | Stateless CSRF token generation/verification |
| `auth` | `hull.middleware.auth` | `hull:middleware:auth` | Session-based and JWT-based authentication middleware |
| `session` | `hull.middleware.session` | `hull:middleware:session` | Server-side sessions backed by SQLite |
| `logger` | `hull.middleware.logger` | `hull:middleware:logger` | Request logging with logfmt output and request IDs |
| `transaction` | `hull.middleware.transaction` | `hull:middleware:transaction` | Wraps handlers in `db.batch()` (BEGIN IMMEDIATE..COMMIT) |
| `idempotency` | `hull.middleware.idempotency` | `hull:middleware:idempotency` | Idempotency-Key middleware with response caching |
| `outbox` | `hull.middleware.outbox` | `hull:middleware:outbox` | Transactional outbox for reliable side-effect delivery |
| `inbox` | `hull.middleware.inbox` | `hull:middleware:inbox` | Inbox deduplication for incoming events/webhooks |
| `cookie` | `hull.cookie` | `hull:cookie` | Cookie parse/serialize helpers |
| `jwt` | `hull.jwt` | `hull:jwt` | JWT sign/verify (HMAC-SHA256) |
| `template` | `hull.template` | `hull:template` | HTML template engine with inheritance, includes, filters |
| `validate` | `hull.validate` | `hull:validate` | Declarative input validation with schema rules |
| `form` | `hull.form` | `hull:form` | URL-encoded form body parsing |
| `i18n` | `hull.i18n` | `hull:i18n` | Internationalization: locale detection, translations, formatting |
| `csv` | `hull.csv` | `hull:csv` | CSV parse/encode (RFC 4180) |
| `search` | `hull.search` | `hull:search` | Full-text search (SQLite FTS5) |
| `rbac` | `hull.middleware.rbac` | `hull:middleware:rbac` | Role-based access control |
| `health` | `hull.middleware.health` | `hull:middleware:health` | Health check + readiness endpoints |
| `etag` | `hull.middleware.etag` | `hull:middleware:etag` | ETag response helpers with 304 Not Modified |
| `db.udf` | `db.udf.register/unregister` | `db.udf.register/unregister` | User-defined SQL functions (Lua/JS callbacks or WASM) |
| `image` | `hull.image` | `hull:image` | Image decode/encode (stb_image), raw pixel buffers |
| `ws` | `hull.ws` (global) | `hull:ws` | WebSocket server + client (broadcast, connect) |
| `json` | `hull.json` | (built-in) | JSON encode/decode |

### Module APIs

**cors.middleware(opts)** — CORS headers + OPTIONS preflight.
- `opts.origins` — list of allowed origins (default: `{"*"}`)
- `opts.methods` — allowed methods string (default: `"GET, POST, PUT, DELETE, OPTIONS"`)
- `opts.headers` — allowed headers string (default: `"Content-Type, Authorization"`)
- `opts.credentials` — boolean, send `Access-Control-Allow-Credentials` (default: `false`)
- `opts.max_age` — preflight cache seconds (default: `86400`)
- Returns `1` on OPTIONS preflight (sends 204), `0` otherwise.

**ratelimit.middleware(opts)** — Per-key request rate limiting (in-memory, resets on restart).
- `opts.limit` — max requests per window (default: `60`)
- `opts.window` — window in seconds (default: `60`)
- `opts.key` — string or `function(req) -> string` (default: `"global"`)
- Sets `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset` headers.
- Returns `1` on limit exceeded (sends 429 + JSON), `0` otherwise.

**csrf.middleware(opts)** — Stateless CSRF protection using HMAC tokens.
- `opts.secret` — HMAC secret (required)
- `opts.session_key` — key in `req.ctx` for session ID (default: `"session_id"`) [Lua]
- `opts.max_age` — max token age in seconds (default: `3600`)
- `opts.header_name` — header to read token from (default: `"x-csrf-token"`)
- `opts.field_name` — form field name (default: `"_csrf"`)
- Safe methods (GET/HEAD/OPTIONS): generates token → `req.ctx.csrf_token`.
- Unsafe methods: verifies token from header or form field.
- Returns `1` on verification failure (sends 403 + JSON), `0` otherwise.
- Helpers: `csrf.generate(session_id, secret)`, `csrf.verify(token, session_id, secret, max_age)`.

**auth.session_middleware(opts)** — Session cookie authentication.
- `opts.cookie_name` — session cookie name (default: `"hull_session"`)
- `opts.optional` — continue without session (default: `false`)
- `opts.login_path` — redirect here on failure instead of sending 401
- Sets `req.ctx.session` and `req.ctx.session_id`.
- Returns `1` on auth failure (sends 401 or redirect), `0` on success.

**auth.jwt_middleware(opts)** — JWT Bearer token authentication.
- `opts.secret` — HMAC-SHA256 secret (required)
- `opts.optional` — continue without token (default: `false`)
- Reads `Authorization: Bearer <token>` header.
- Sets `req.ctx.user` (decoded payload).
- Returns `1` on auth failure (sends 401 + JSON), `0` on success.

**auth.login(req, res, user_data, opts)** — Creates session, sets cookie. Returns `session_id`.

**auth.logout(req, res, opts)** — Destroys session, clears cookie.

**session** — Server-side sessions backed by SQLite. Requires `session.init()` at startup.
- `session.init(opts)` — creates `hull_sessions` table. `opts.ttl` = lifetime in seconds (default: `86400`).
- `session.create(data)` → 64-char hex session ID.
- `session.load(session_id)` → data table or nil. Auto-extends expiry.
- `session.update(session_id, data)` — updates session data.
- `session.destroy(session_id)` — deletes session.
- `session.cleanup()` → count of deleted expired sessions.

**cookie** — Cookie helpers (not middleware).
- `cookie.parse(header)` → table `{ name = value, ... }`.
- `cookie.serialize(name, value, opts)` → `Set-Cookie` header string.
  - `opts.path` (default: `"/"`), `opts.httponly` (default: `true`), `opts.secure`, `opts.samesite` (default: `"Lax"`), `opts.max_age`, `opts.domain`.
- `cookie.clear(name, opts)` → `Set-Cookie` header with `Max-Age=0`.

**jwt** — JWT sign/verify (HS256 only, not middleware).
- `jwt.sign(payload, secret)` → token string. Auto-sets `iat`.
- `jwt.verify(token, secret)` → payload table, or `nil, "error reason"`.
- `jwt.decode(token)` → payload table or nil (no signature check).

**logger.middleware(opts)** — Request logging with logfmt output and auto-assigned request IDs.
- `opts.skip` — list of paths to skip (exact match, e.g. `{"/health"}`)
- `opts.include_headers` — list of header names to include in log line
- Sets `X-Request-ID` response header and `req.ctx.request_id`.
- Helpers: `logger.generate_id()`, `logger.format_line(entries)`, `logger.should_skip(path, skip_list)`.
- Returns `0` (always continues).

**validate.check(data, schema)** — Declarative input validation.
- `schema` maps field names to rule tables.
- Rules: `required`, `trim`, `type` (`"string"`, `"number"`, `"integer"`, `"boolean"`), `min`, `max`, `pattern`, `oneof`, `email`, `fn` (custom validator), `message` (custom error).
- `min`/`max` apply to string length or numeric value depending on field type.
- Returns `(ok, errors)` where `errors` maps field names to error strings.

**form.parse(body)** — URL-encoded form body parsing.
- Decodes `application/x-www-form-urlencoded` format.
- Handles `+` → space and `%XX` percent-encoding. Last value wins for duplicates.
- Returns table `{ field_name = value, ... }` (empty table for nil/empty input).

**i18n** — Internationalization: locale detection, message bundles, formatting.
- `i18n.load(name, tbl)` — register a locale with translations and format rules.
- `i18n.locale(name?)` — get or set the active locale.
- `i18n.t(key, params?)` → translated string. Supports `${variable}` interpolation and dot-path keys.
- `i18n.number(n)` → formatted number (locale-specific decimal/thousands separators).
- `i18n.date(timestamp)` → formatted date string.
- `i18n.currency(amount, code)` → formatted currency string (symbol + locale rules).
- `i18n.detect(accept_language_header)` → best matching locale name or nil.

**transaction** — Wraps handlers in SQLite transactions.
- `transaction.middleware()` — post-body middleware that sets `req.ctx._txn = true` for downstream use.
- `transaction.run(fn)` — wraps `fn` in `db.batch()` (BEGIN IMMEDIATE → fn() → COMMIT, ROLLBACK on error).
- `transaction.try(fn)` → `(ok, err)` — like `run` but returns error instead of throwing.

**idempotency** — Idempotency-Key middleware with response caching.
- `idempotency.init(opts)` — creates `_hull_idempotency_keys` table. `opts.ttl` = key lifetime in seconds (default: `86400`).
- `idempotency.middleware(opts)` — post-body middleware intercepting POST (configurable via `opts.methods`).
  - `opts.header_name` — header to read key from (default: `"idempotency-key"`).
  - `opts.get_principal` — `function(req) -> string` for per-user scoping (default: `"__anon"`).
  - Cache hit + same fingerprint → returns cached response (handler skipped).
  - Cache hit + different fingerprint → returns 409 Conflict.
  - Fingerprint: `SHA-256(method + path + body)`.
- `idempotency.respond(req, res, status, data)` — sends response and caches it for replay.
- `idempotency.complete(req)` — marks key as processed without caching response body.
- `idempotency.cleanup()` → count of deleted expired keys.

**outbox** — Transactional outbox for reliable side-effect delivery.
- `outbox.init(opts)` — creates `_hull_outbox` table. `opts.max_attempts` (default: `5`).
- `outbox.enqueue(opts)` — enqueue a delivery (call inside a transaction).
  - `opts.kind` — delivery type (e.g. `"webhook"`, `"email"`).
  - `opts.destination` — target URL or address.
  - `opts.payload` — payload string.
  - `opts.headers` — JSON-encoded headers (optional).
  - `opts.idempotency_key` — dedup key for delivery (optional).
- `outbox.flush(opts)` — deliver pending items. Exponential backoff (`2^attempt * 10s`, capped at 1hr).
- `outbox.middleware()` — sets `req.ctx._outbox_flush = true` for auto-flush.
- `outbox.stats()` → `{ pending, delivered, failed }` counts.
- `outbox.cleanup(max_age)` — delete old delivered items.

**inbox** — Inbox deduplication for incoming events/webhooks.
- `inbox.init(opts)` — creates `_hull_inbox_processed` table. `opts.ttl` = record lifetime (default: `86400`).
- `inbox.is_duplicate(message_id, source?)` → boolean. Default source: `"default"`.
- `inbox.mark_processed(message_id, source?, opts?)` — record as processed.
- `inbox.check_and_mark(message_id, source?, opts?)` → boolean (true = duplicate, false = new + marked).
- `inbox.cleanup()` → count of deleted expired records.

**template** — HTML template engine with compile-once, render-many caching.

```lua
local template = require("hull.template")
template.render("pages/home.html", data)       -- load + compile + render (cached)
template.render_string(source, data)            -- compile from string + render
template.compile("pages/home.html")             -- returns compiled function
template.clear_cache()                          -- clear compiled function cache
```

Template syntax:
- `{{ var }}` — HTML-escaped output
- `{{ var.path }}` — dot path lookup (nil-safe)
- `{{ var | filter }}` — pipe filter (`upper`, `lower`, `trim`, `length`, `default: value`, `json`, `raw`)
- `{{{ var }}}` — raw (unescaped) output
- `{% if var %}` / `{% elif var %}` / `{% else %}` / `{% end %}` — conditionals
- `{% for item in list %}` / `{% for key, val in obj %}` — iteration
- `{% block name %}` / `{% extends "parent.html" %}` — template inheritance
- `{% include "partial.html" %}` — include partials
- `{# comment #}` — stripped from output

Templates are loaded from `app_dir/templates/` in dev mode. In built binaries, templates are embedded as byte arrays via `hull build` or `make APP_DIR=...`.

**JS API** (camelCase):
```javascript
import { template } from "hull:template";
template.render("pages/home.html", data);       // load + compile + render (cached)
template.renderString(source, data);             // compile from string + render
template.compile("pages/home.html");             // returns compiled function
template.clearCache();                           // clear compiled function cache
```

**Template engine details:**
- **Compilation:** Templates are parsed (lexer → recursive-descent parser → AST), then code-generated to native Lua/JS source and compiled via `luaL_loadbuffer` (Lua) or `JS_Eval` (JS). Compiled functions are cached — compile once, render many.
- **XSS safety:** All `{{ }}` output is HTML-escaped by default (`& < > " '` → entities). Only `{{{ }}}` and `| raw` bypass escaping.
- **Dot paths are nil-safe:** `{{ user.address.city }}` returns empty string if any intermediate is nil/undefined — no errors.
- **For-loop variables are scoped:** Inside `{% for item in items %}`, `item` refers to the loop variable, not `data.item`.
- **Lua truthiness caveat:** In Lua, empty tables `{}` and `0` are truthy. Use a boolean flag like `has_items = #items > 0` when checking emptiness in `{% if %}`.
- **Filters:** `upper`, `lower`, `trim`, `length`, `default: "value"`, `json`, `raw`. Filters chain: `{{ name | trim | upper }}`.
- **Inheritance:** `{% extends "base.html" %}` loads parent, child overrides `{% block name %}` content. Multi-level inheritance supported. Circular extends detected.
- **Includes:** `{% include "partials/nav.html" %}` inlines the partial's AST. Included templates share the same data context.
- **Template directory:** Place templates in `app_dir/templates/`. Names are relative paths (e.g. `"pages/home.html"`, `"partials/nav.html"`, `"base.html"`).
- **CSP nonce:** No engine magic needed. Pass nonce as data: `template.render("page.html", { csp_nonce = nonce })`, use `<script nonce="{{ csp_nonce }}">` in template.

**csv.parse(text, opts?)** — Parse CSV text (RFC 4180).
- `opts.headers` — first row is header; returns objects (default: `false`)
- `opts.separator` — field delimiter (default: `","`)
- Returns array of row arrays, or row objects if `headers = true`.

**csv.encode(rows, opts?)** — Encode rows as CSV text.
- `opts.headers` — rows are objects; emit header row (default: `false`)
- `opts.separator` — field delimiter (default: `","`)
- Returns CSV string.

**search** — Full-text search backed by SQLite FTS5.
- `search.create_index(name, columns, opts?)` — Create FTS5 virtual table.
- `search.index(name, id, fields)` — Insert/replace document.
- `search.remove(name, id)` — Delete document.
- `search.query(name, query, opts?)` — Full-text search. Returns `{id, rank}` array.
  - `opts.limit` (default: 20), `opts.offset` (default: 0)
- `search.reindex(name, source_table, opts?)` — Bulk re-index from table.
- `search.drop_index(name)` — Drop FTS5 table.

**rbac** — Role-based access control backed by SQLite.
- `rbac.init()` — creates `_hull_roles`, `_hull_permissions`, `_hull_role_permissions`, `_hull_user_roles` tables.
- `rbac.define_role(name, permissions?)` — create role with optional permissions.
- `rbac.assign(user_id, role)` / `rbac.revoke(user_id, role)` — manage user roles.
- `rbac.roles(user_id)` → array of role names.
- `rbac.has_role(user_id, role)` → boolean.
- `rbac.has_permission(user_id, permission)` → boolean.
- `rbac.require_role(role)` → middleware function (403 on denial).
- `rbac.require_permission(perm)` → middleware function (403 on denial).

**health** — Liveness (`/health`) and readiness (`/ready`) endpoints with DB ping, custom checks, and server stats.
- `health.register(name, fn)` — register a custom health check. `fn()` returns `true` or `false`.
- `health.unregister(name)` — remove a registered check.
- `health.run_checks(opts)` → `{ checks, all_ok }`. `opts.db_check` (default: `true`).
- `health.middleware(opts)` — returns middleware that intercepts `/health` and `/ready`.
  - `opts.path_health` — liveness path (default: `"/health"`). Returns `{ status: "ok", uptime }`.
  - `opts.path_ready` — readiness path (default: `"/ready"`). Returns status, checks, uptime, server stats.
  - `opts.db_check` — include DB ping (default: `true`).
  - Returns `1` on health/ready paths, `0` otherwise (passes through to next handler).
  - Readiness returns 503 if any check fails.
- **JS only:** `health.setDb(dbModule)` — pass the db module explicitly (ES modules can't conditionally import). Also accepts `opts.db` in middleware options.

**etag** — ETag response helpers. Not a middleware — provides wrapper functions for route handlers.
- `etag.json(req, res, data, status?)` — send JSON response with ETag. Sends 304 if `If-None-Match` matches.
- `etag.text(req, res, text, status?)` — same for text responses.
- `etag.html(req, res, html, status?)` — same for HTML responses.
- `etag.compute(body)` → `W/"<first 16 hex chars of SHA-256>"` or `nil`.
- `etag.matches(req, tag)` → boolean. Checks `If-None-Match` header (comma-separated, `*` wildcard).
- Only computes ETags for GET/HEAD requests. Skips bodies > 1 MB.

**db.udf** — User-defined SQL functions backed by Lua/JS callbacks or WASM modules.
- `db.udf.register(name, fn, opts?)` — Register scalar UDF (Lua/JS function).
- `db.udf.register(name, {step, finalize}, opts?)` — Register aggregate UDF.
- `db.udf.register(name, "module_name", opts?)` — Register WASM-backed UDF.
- `db.udf.unregister(name)` — Remove a registered UDF.
- `opts.deterministic` — boolean, enables SQLite optimizer (default: false)
- `opts.aggregate` — boolean, WASM aggregate mode (default: false)
- `opts.args` — number of arguments (-1 = variadic, default: 1)
- `opts.gas` — per-row gas limit for WASM UDFs (default: 100K)
- Names must start with `hull_` to prevent shadowing SQLite built-ins.
- Lua/JS UDFs work with `db.query()` only (sync). WASM UDFs work with both `db.query()` and `db.async.query()`.

**image** — Image creation, encoding, and decoding via pluggable codec vtable (stb_image default).
- `image.new(w, h, format, pixels)` → HlImage. Formats: `"rgba8"`, `"r8"`, `"rgba16float"`, `"r32float"`.
- `image.from_buffer(buf, w, h, format)` → HlImage (zero-copy borrow from WasmBuffer/MappedBuffer).
- `image.decode(data, format?)` → HlImage. Auto-detects PNG/JPEG/BMP from magic bytes.
- `image.encode(img, format, opts?)` → bytes. `opts.quality` for JPEG (default 90).
- `img:width()`, `img:height()`, `img:format()`, `img:size()` — properties.
- `img:pixels()` — raw pixel bytes.
- `img:close()` — explicit free (GC handles it otherwise).

### WebSocket Endpoints

Register WebSocket endpoints with `app.ws(path, callbacks)`. Server-side connections are managed via the `HlWsRegistry`.

**Lua:**
```lua
app.ws("/ws/chat", {
    on_open = function(conn)
        log.info("connected: " .. conn:id())
    end,
    on_message = function(conn, msg, is_binary)
        ws.broadcast("/ws/chat", msg)
    end,
    on_close = function(conn, code, reason)
        log.info("disconnected: " .. conn:id())
    end,
})
```

**JavaScript:**
```javascript
import { ws } from "hull:ws";
app.ws("/ws/chat", {
    onOpen(conn) { log.info("connected: " + conn.id); },
    onMessage(conn, msg, isBinary) { ws.broadcast("/ws/chat", msg); },
    onClose(conn, code, reason) { log.info("disconnected: " + conn.id); },
});
```

**Connection object:**
- `conn:id()` / `conn.id` — monotonic connection ID (getter)
- `conn:path()` / `conn.path` — endpoint path (getter)
- `conn:send(text)` / `conn.send(text)` — send text frame
- `conn:send_binary(data)` / `conn.sendBinary(data)` — send binary frame
- `conn:close(code?, reason?)` / `conn.close(code?, reason?)` — initiate close
- `conn:ping(data?)` / `conn.ping(data?)` — send ping
- `conn.data` — per-connection storage (table/object, lazy-created)

**Module functions:**
- `ws.broadcast(path, data [, binary])` — broadcast to all connections on path, returns count sent
- `ws.connections(path)` — count active connections on path
- `ws.connect(url, handlers [, opts])` — connect to remote WebSocket server (see below)

**Client WebSocket (`ws.connect`):**

```lua
local client = ws.connect("ws://other:8080/feed", {
    on_open = function(conn) conn:send("hello") end,
    on_message = function(conn, msg) log.info(msg) end,
    on_close = function(conn, code, reason) end,
    on_error = function(conn, err) log.error(err) end,
})
```

```javascript
const client = ws.connect("ws://other:8080/feed", {
    onOpen(conn) { conn.send("hello"); },
    onMessage(conn, msg) { log.info(msg); },
    onClose(conn, code, reason) {},
    onError(conn, err) { log.error(err); },
});
```

- Client conn has same `send`/`sendBinary`/`close`/`ping` methods as server conn
- Host allowlist enforced (same as `http.fetch` — must be in manifest `hosts`)
- Requires running server (`hull` with `-p` port)
- Callbacks fire on the event loop thread (same as server WS callbacks)

### SSE Endpoints

Register Server-Sent Events endpoints with `app.sse(path, handler)`. The handler receives a request object and a stream object.

**Lua:**
```lua
app.sse("/sse/events", function(req, stream)
    stream:event("welcome", json.encode({ time = time.datetime() }))
    for i = 1, 5 do
        hull.sleep(1000)
        stream:event("tick", tostring(i), tostring(i))
    end
    stream:close()
end)
```

**JavaScript:**
```javascript
app.sse("/sse/events", async (req, stream) => {
    stream.event("welcome", JSON.stringify({ time: time.datetime() }));
    for (let i = 1; i <= 5; i++) {
        await hull.sleep(1000);
        stream.event("tick", String(i), String(i));
    }
    stream.close();
});
```

**Stream object:**
- `stream:event(name, data [, id])` / `stream.event(name, data, id?)` — send SSE event. `name` = event type (null/nil to omit), `data` = event data (multiline auto-split), `id` = event ID (optional)
- `stream:comment(text)` / `stream.comment(text)` — send SSE comment (keep-alive)
- `stream:close()` / `stream.close()` — end the stream

**Implementation:** Uses Keel's `kl_sse_begin` / `kl_sse_event` / `kl_sse_end` over chunked transfer encoding. The handler runs as a coroutine (Lua) or async function (JS) that can yield with `hull.sleep()` between events.

### Static File Serving

Convention-based: place files in `app_dir/static/`, they're served at `/static/*`.

- **Dev mode:** Reads from disk via `kl_response_file()` (zero-copy sendfile). `Cache-Control: no-cache`.
- **Build mode:** Files are embedded in the binary via `hl_app_entries[]` (with `static/` prefix) and looked up via VFS (`hl_vfs_find`). `Cache-Control: public, max-age=86400`.
- **ETag/304:** `W/"<size_hex>"` for embedded, `W/"<mtime_hex>-<size_hex>"` for filesystem. Returns 304 on `If-None-Match`.
- **MIME types:** Extension-based lookup (21 types: html, css, js, json, png, jpg, svg, woff2, etc.). Default: `application/octet-stream`.
- **Security:** Rejects `..` path traversal, null bytes, leading `/` in relative paths.
- **Auto-detection:** Middleware is registered only when `hl_vfs_has_prefix(app_vfs, "static/")` returns true or `static/` directory exists on disk. User routes take priority (registered first).

Implementation: `src/hull/static.c` + `include/hull/static.h`. Uses `HlVfs` for O(log n) embedded lookup. Registered as a Keel pre-body middleware via `kl_server_use()`.

Embedding paths:
- `make APP_DIR=myapp` — Makefile discovers all app files, generates sorted `app_registry.c` with single `hl_app_entries[]`
- `hull build myapp` — `build.lua` discovers all files, generates sorted `app_registry.c`
- All file types share one `HlEntry` array, sorted by name (`LC_ALL=C sort`), disambiguated by naming convention: `./` (modules), `templates/`, `static/`, `migrations/`
- At runtime, consumers use `HlVfs` for O(log n) lookups instead of O(n) linear scans

### Recommended Middleware Stack

Order matters — each middleware runs before the next:

```lua
local cors        = require("hull.middleware.cors")
local ratelimit   = require("hull.middleware.ratelimit")
local auth        = require("hull.middleware.auth")
local csrf        = require("hull.middleware.csrf")
local session     = require("hull.middleware.session")
local logger      = require("hull.middleware.logger")
local health      = require("hull.middleware.health")
local etag        = require("hull.middleware.etag")
local transaction = require("hull.middleware.transaction")
local idempotency = require("hull.middleware.idempotency")

session.init()       -- create hull_sessions table
idempotency.init()   -- create _hull_idempotency_keys table

-- Pre-body middleware (runs before body is read)
-- 0. Health checks — /health (liveness) and /ready (readiness)
app.use("GET", "/*", health.middleware())
-- 1. Logging — assign request ID, log method + path
app.use("*", "/*", logger.middleware({ skip = {"/health"} }))
-- 2. Rate limiting — reject abusive traffic before doing any work
app.use("*", "/api/*", ratelimit.middleware({ limit = 60, window = 60 }))
-- 3. CORS — must run before auth so preflight doesn't require credentials
app.use("*", "/api/*", cors.middleware({ origins = { "https://myapp.com" } }))
-- 4. Authentication — session or JWT
app.use("*", "/api/*", auth.session_middleware({}))

-- Post-body middleware (runs after body is read)
-- 5. CSRF — needs body for form token (session-based apps only, not JWT)
app.use_post("*", "/*", csrf.middleware({ secret = "change-me" }))
-- 6. Transaction — wrap mutations in BEGIN IMMEDIATE..COMMIT
app.use_post("POST", "/api/*", transaction.middleware())
-- 7. Idempotency — cache POST responses by Idempotency-Key header
app.use_post("POST", "/api/*", idempotency.middleware())
-- 8. Route handlers — use etag.json() instead of res:json() for ETag support
app.get("/api/items", function(req, res)
    local items = db.query("SELECT * FROM items")
    etag.json(req, res, { items = items })
end)
```

### Best Practices

- **Middleware order matters:** Rate limit before auth (reject early, save work). CORS before auth (preflight must not require credentials).
- **Scope middleware to paths:** Use `"/api/*"` not `"/*"` for CORS and rate limiting. Public routes (health checks, static assets) shouldn't be rate limited or require auth.
- **Use `req.ctx` for data passing:** Middleware stores data in `req.ctx` (e.g. `session_id`, `user`, `csrf_token`) for downstream handlers.
- **CORS origins:** Always list explicit origins in production. Never use `"*"` with `credentials = true`.
- **Rate limiting keys:** Default `"global"` key rate-limits all clients together. Use a key function for per-user limits: `key = function(req) return req.ctx.user_id or req.headers["x-forwarded-for"] or "anon" end`.
- **CSRF is for cookies only:** Session/cookie auth needs CSRF protection. JWT Bearer auth does not (tokens aren't sent automatically by browsers).
- **Session init at startup:** Call `session.init()` before registering routes — it creates the SQLite table.
- **Lua vs JS differences:** The Lua and JS APIs are functionally equivalent but differ in naming conventions (snake_case vs camelCase) and some defaults. See the JS stdlib source for JS-specific option names.

### Background Timers

`app.every()` and `app.daily()` register repeating timer callbacks that run on the event loop thread. Timer callbacks support the full async runtime (`hull.sleep()`, `http.fetch()`, `db.*`).

**Lua:**
```lua
-- Repeating interval (milliseconds, minimum 100ms)
app.every(5000, function()
    session.cleanup()
end)

-- Async operations work inside timers
app.every(30000, function()
    outbox.flush()  -- makes HTTP requests
end)

-- Return false to self-cancel
app.every(1000, function()
    local pending = outbox.flush()
    if pending == 0 then return false end
end)

-- Daily at wall-clock time (UTC by default)
app.daily("02:00", function()
    outbox.cleanup(86400 * 30)
end)

-- Daily at local time
app.daily("02:00", function()
    inbox.cleanup()
end, { localtime = true })
```

**JavaScript:**
```javascript
app.every(5000, () => { session.cleanup(); });

app.every(30000, async () => { await outbox.flush(); });

app.every(1000, () => {
    const pending = outbox.flush();
    if (pending === 0) return false;  // self-cancel
});

app.daily("02:00", () => { outbox.cleanup(86400 * 30); });
app.daily("02:00", () => { inbox.cleanup(); }, { localtime: true });
```

**Constraints:**
- Minimum interval: 100ms (enforced, prevents tight loops)
- No `req`/`res` — these are background tasks, not request handlers
- Errors are logged but don't stop the timer (re-schedules regardless)
- One invocation at a time — if a callback is still running (async yield), the next tick is deferred
- Return `false` to stop the repeating timer
- `app.daily("HH:MM")` defaults to UTC. Pass `{ localtime = true }` for local time

**Implementation:** Timer callbacks fire via Keel's `kl_timer_add` min-heap. Self-re-adding callbacks give repeating behavior. Async operations use "detached" mode — `HlAsyncCtx` with `detached=1` resumes via `hl_async_ctx_resume_detached()` instead of `kl_async_complete()`.

### WASM Compute Plugins

Hull supports compute-only WASM plugins for CPU-intensive pure functions. Plugins have no I/O — they transform input bytes to output bytes inside isolated WASM linear memory with gas-metered execution.

**Directory convention:** Place `.wasm` files in `app_dir/compute/`. Module name = filename without extension (e.g. `compute/score.wasm` → `"score"`).

**Lua API:**
```lua
compute.available()                     -- boolean (WASM runtime initialized?)

-- Synchronous call (gas-limited, blocking)
local output, err = compute.call("score", input_bytes, {
    max_input  = 64 * 1024,    -- 64 KB (optional, has defaults)
    max_output = 64 * 1024,    -- 64 KB
    gas        = 10000000,     -- 10M instructions
    heap       = 256 * 1024,   -- 256 KB WASM heap
})
if err then
    -- err: "not_found", "gas_exhausted", "output_too_small",
    --       "input_too_large", "call_failed", "internal_error"
end

-- Async call (yields to event loop, request handler only)
-- Dispatches WASM execution to thread pool. Other requests served while running.
local r = compute.async.call("score", input_bytes, opts)
-- r.result = output string on success, r.error = error string on failure

-- Preload module into cache
compute.load("score")

-- Create a WasmBuffer from a string (for zero-copy chaining)
local buf = compute.buffer("input data")
```

**JavaScript API:**
```javascript
import { compute } from "hull:compute";

compute.available()                    // boolean

// Sync: Input: string or ArrayBuffer. Output: ArrayBuffer.
const output = compute.call("score", inputBytes, {
    maxInput: 64 * 1024,
    maxOutput: 64 * 1024,
    gas: 10000000,
});

// Async: returns Promise. Dispatches to thread pool.
const buf = await compute.async.call("score", inputBytes, opts);

compute.load("score");

// Create a WasmBuffer from a string (for zero-copy chaining)
const buf = compute.buffer("input data");
```

**Sync vs Async:** Use `compute.call()` for fast/small computations (sub-ms) and in tests/timers. Use `compute.async.call()` in request handlers for expensive computations — it yields to the event loop so other requests are served concurrently. The async variant follows the same pattern as `db.async.query()`.

**Shared data segments** — `compute.segment()` loads named read-only data segments that all instances of a module can read at native speed via WAMR shared heaps:

```lua
-- Lua: load named segments for a module
compute.segment("routing", "graph", graph_bytes)       -- segment 0
compute.segment("routing", "landmarks", fs.mmap("landmarks.bin"))  -- zero-copy
compute.segment("routing", "grid", nil)                -- remove segment
compute.segment("routing", nil)                        -- remove all segments
-- Use normally — segments auto-attached to every instance
local out = compute.call("routing", query)
```

```javascript
// JS
compute.segment("routing", "graph", graphBytes);
compute.segment("routing", null);                      // remove all
```

WASM plugins query segments via `host_call(0x02, segment_id, sub)`:
- `host_call(0x02, seg_id, 0)` → WASM address of segment (0 if not loaded)
- `host_call(0x02, seg_id, 1)` → size of segment
- `host_call(0x02, -1, 0)` → total segment count

Segments are page-aligned mmap regions in the high end of WASM32 address space. Up to 16 segments per module, 3 GB total. Adding/removing segments drains the instance pool.

**Plugin ABI:** Plugins must export `hull_process(in_ptr, in_len, out_ptr, out_max) -> bytes_written` and optionally `hull_version() -> int`. Single import: `env.host_call(opcode, ptr, len) -> int` (LOG=0x01, DATA_INFO=0x02, CALLBACK=0x10).

**Build & AOT:** `hull build` embeds `compute/*.wasm` files and auto-compiles them to AOT if `wamrc` is available. AOT modules are embedded alongside `.wasm` files; at runtime, AOT is preferred over interpreter.

```bash
make wamrc                         # build AOT compiler (one-time, requires cmake + LLVM)
hull build myapp                   # auto-AOT compiles compute/*.wasm during build
hull build myapp --no-aot          # skip AOT compilation
hull build myapp --target=x86_64   # cross-compile AOT for different arch
```

For cosmocc builds, both x86_64 and aarch64 AOT files are generated automatically.

**Dev mode AOT:** In `hull dev`, place pre-compiled `.aot.<arch>` files next to `.wasm` files in `compute/` and they'll be loaded automatically (four-tier lookup: VFS AOT → VFS WASM → filesystem AOT → filesystem WASM).

**wamrc build:** `make wamrc` builds the WAMR AOT compiler from `vendor/wamr/wamr-compiler`. Requires cmake and LLVM (`brew install llvm` on macOS, `apt install llvm` on Linux). Override LLVM path: `make wamrc WAMRC_CMAKE_FLAGS="-DLLVM_DIR=/path/to/llvm/cmake"`. Output: `build/wamrc`.

**Configuration:** Controlled by `HL_ENABLE_WASM` (default: 1). Disable with `make HL_ENABLE_WASM=0`. WAMR adds ~256 KB to the binary.

**SIMD128:** Enabled (`-DWASM_ENABLE_SIMD=1`). Compile plugins with `-msimd128` (C) or `#[target_feature(enable = "simd128")]` (Rust). AOT maps to native SSE4.1/NEON. Interpreter cannot load v128 modules (graceful error).

**Instance pooling:** Reuses WASM instances across `compute.call()` invocations (pool max 8 per module, heap ≤ 4 MB). Reduces per-call overhead from ~2.5ms to near-zero.

**Persistent instances:** `compute.instance(name, opts?)` creates a long-lived WASM instance that retains linear memory across calls. Not pooled — exclusively owned until `close()` or GC. Supports sync (`inst:call`/`inst.call`), async (`inst.async:call`/`inst.async.call`), and buffer mode. Gas resets per call; heap/stack are immutable. Use for stateful workloads (ML weights, pre-built indexes) where per-call instantiation cost is too high.

**Memory limits:** Configurable at three tiers — per-call opts, CLI flags (`--wasm-heap 512M`), and compile-time (`make HL_WASM_MAX_HEAP_MB=512`). Default: 2 MB heap, 1 MB I/O. Max: ~4 GB heap, 256 MB I/O (WASM32) / 16 GB I/O (Memory64).

**Memory64:** Modules compiled with 64-bit memory (`(memory i64 N)`) are detected automatically. Memory64 modules **require AOT compilation** — the fast interpreter does not support Memory64. `hull build` passes `--enable-memory64` to wamrc when it detects a Memory64 module. The `hull_process` ABI changes to `(i64, i64, i64, i64) -> i32` for Memory64 modules; the runtime dispatches the correct calling convention based on the module's `is_memory64` flag.

**Streaming I/O:**
```lua
-- Buffer → buffer
local result = compute.stream("module", input_data, nil, { chunk_size = 65536 })

-- File → file (never fully in memory)
compute.stream("module", { file = "input.csv" }, { file = "output.json" }, { chunk_size = 65536 })

-- Buffer → callback
compute.stream("module", data, function(chunk, index, is_last) end, { chunk_size = 65536 })
```

- Input: string, WasmBuffer, MappedBuffer, or `{ file = "path" }`
- Output: nil (return buffer), `{ file = "path" }`, or callback function
- Uses persistent instance internally — state preserved between chunks
- Modules can query chunk metadata via `host_call(0x03)`: `hull_stream_is_first()`, `hull_stream_is_last()`, `hull_stream_chunk_index()`

**Architecture:** See `docs/wamr_architecture.md` for the full design document.

### GPU Compute (wgpu-native)

Hull supports GPU compute shaders via wgpu-native v27 (Vulkan/Metal/DX12). Disabled by default. Enable with `make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu`.

**Manifest declaration:** Apps must declare `gpu: true` in their manifest to access the `gpu` global. Apps without a manifest get GPU access by default (backward compat).

```lua
app.manifest({ gpu = true })
```

**Lua API:**
```lua
gpu.available()                        -- boolean
gpu.devices()                          -- { {id=0, name="Apple M1"}, ... }
gpu.compile(name, wgsl)                -- compile WGSL shader (cached, idempotent)
gpu.load(name)                         -- load + compile shaders/<name>.wgsl from disk/VFS
gpu.dispatch(name, opts)               -- run shader, return output (string)
gpu.dispatch(name, { output = false }) -- fire-and-forget (no readback, returns true)
gpu.pipeline(stages, opts)             -- multi-stage dispatch, single submission
gpu.pipeline(stages, { output = false }) -- fire-and-forget pipeline
gpu.async.dispatch(name, opts)         -- async dispatch (yields to event loop)
gpu.async.pipeline(stages, opts)       -- async pipeline
gpu.buffer(name, data)                 -- create/write persistent buffer (string or MappedBuffer)
gpu.buffer(name, nil)                  -- destroy buffer
gpu.buffer_read(name)                  -- read buffer back to host
gpu.buffer_copy(src, dst, opts?)       -- GPU-side buffer copy (no CPU roundtrip)
```

**JavaScript API:**
```javascript
import { gpu } from "hull:gpu";
gpu.available()                        // boolean
gpu.devices()                          // [{id, name}, ...]
gpu.compile(name, wgsl)                // compile WGSL shader
gpu.load(name)                         // load + compile shaders/<name>.wgsl
gpu.dispatch(name, opts)               // ArrayBuffer output
gpu.dispatch(name, { output: false })  // fire-and-forget (returns true)
gpu.pipeline(stages, opts)             // multi-stage, ArrayBuffer or Array<ArrayBuffer>
gpu.pipeline(stages, { output: false })// fire-and-forget pipeline
gpu.async.dispatch(name, opts)         // Promise<ArrayBuffer>
gpu.async.pipeline(stages, opts)       // Promise
gpu.buffer(name, data)                 // create/write (ArrayBuffer, MappedBuffer, or string)
gpu.buffer(name, null)                 // destroy
gpu.bufferRead(name)                   // ArrayBuffer
gpu.bufferCopy(src, dst, opts?)        // GPU-side buffer copy
```

**Dispatch options:**
```lua
gpu.dispatch("shader_name", {
    uniforms = packed_binary,          -- binding 0 (16-byte aligned)
    buffers = {
        { data = bytes, usage = "read" },       -- binding 1
        { name = "persistent", usage = "read" }, -- binding 2 (named buffer)
        { size = N, usage = "readwrite" },       -- binding 3 (output)
    },
    textures = {                               -- optional texture bindings
        { name = "input" },                    -- sampled (paired: texture + sampler)
        { name = "output", storage = true },   -- storage texture (single binding)
    },
    workgroups = { x = 64, y = 1, z = 1 },
    output = 3,                        -- 1-indexed buffer to read back (Lua)
    output_texture = 2,                -- 1-indexed texture to read back as HlImage (Lua)
    device = -1,                       -- -1 = default device
})
```

**Pipeline (multi-stage dispatch):**
```lua
-- Single command buffer submission, single poll, single readback.
-- Named buffers are shared across stages (max declared size allocated).
local out = gpu.pipeline({
    { shader = "normalize", buffers = {{ name = "data", data = input }}, workgroups = {x=64} },
    { shader = "score",     buffers = {{ name = "data" }, { name = "results", size = N*4 }},
                            uniforms = params, workgroups = {x=64} },
    { shader = "top_k",     buffers = {{ name = "results" }}, workgroups = {x=1} },
}, {
    outputs = { { stage = 3, buffer = 1 } },  -- 1-indexed (Lua)
    device = -1,
})
-- Single output: returns string. Multiple outputs: returns table of strings.
```
```javascript
// JS: 0-indexed outputs
const out = gpu.pipeline([
    { shader: "normalize", buffers: [{ name: "data", data: buf }], workgroups: {x:64} },
    { shader: "score",     buffers: [{ name: "data" }, { name: "results", size: N*4 }],
                           uniforms: paramsBuf, workgroups: {x:64} },
    { shader: "top_k",     buffers: [{ name: "results" }], workgroups: {x:1} },
], { outputs: [{ stage: 2, buffer: 0 }] });
// Single output: ArrayBuffer. Multiple outputs: Array<ArrayBuffer>.
```

**Fire-and-forget dispatch:** Set `output = false` to skip readback. The shader executes and persistent buffers are updated in-place, but no data is returned to the host. Returns `true` on success. Works with both `gpu.dispatch()` and `gpu.pipeline()`.

```lua
-- Update embeddings in-place on GPU (no readback)
gpu.dispatch("normalize", {
    buffers = {{ name = "embeddings" }},
    workgroups = { x = 1024 },
    output = false,
})
-- Pipeline fire-and-forget: double → triple in-place
gpu.pipeline({
    { shader = "double", buffers = {{ name = "data" }}, workgroups = {x=64} },
    { shader = "triple", buffers = {{ name = "data" }}, workgroups = {x=64} },
}, { output = false })
```

**GPU-side buffer copy:** Copy between persistent GPU buffers without CPU roundtrip.

```lua
gpu.buffer_copy("source", "dest")                           -- full copy
gpu.buffer_copy("source", "dest", { size = 1024 })          -- partial
gpu.buffer_copy("source", "dest", {
    src_offset = 0, dst_offset = 512, size = 256,           -- with offsets
})
```

**GPU Textures:**
- `gpu.texture(name, img)` — create persistent texture from HlImage.
- `gpu.texture(name, data, opts)` — create from raw bytes with `opts.width`, `opts.height`, `opts.format`, `opts.storage`.
- `gpu.texture(name, nil)` — destroy persistent texture.
- `gpu.texture_read(name)` → HlImage — read back texture pixels.
- Dispatch with textures:
  ```lua
  gpu.dispatch("shader", {
      textures = {
          { name = "input" },                        -- sampled (binding N, N+1)
          { name = "output", storage = true },       -- storage (binding M)
      },
      output_texture = 2,                            -- readback as HlImage
  })
  ```
- Sampled textures get paired bindings (texture view + sampler). Storage textures get single binding.
- Binding convention: uniforms → buffers → sampled textures → storage textures (all `@group(0)`).

**Shader loading from files:** `gpu.load(name)` reads `shaders/<name>.wgsl` from disk (dev mode) or VFS (built binaries) and compiles it. Enables shader iteration without modifying app code.

```lua
-- shaders/score.wgsl on disk (dev mode) or embedded (built binary)
gpu.load("score")                     -- reads + compiles shaders/score.wgsl
-- equivalent to: gpu.compile("score", <file contents>)
```

**Shader embedding in builds:** `hull build` and `make APP_DIR=` automatically discover and embed `shaders/*.wgsl` files into the binary via the VFS, just like `templates/`, `static/`, `compute/`, and `migrations/`. `gpu.load()` checks VFS first, then falls back to disk — so shaders work identically in dev mode and built binaries.

**Directory convention:**
```
myapp/
  app.lua
  shaders/             ← WGSL compute shaders (gpu.load)
    normalize.wgsl
    score.wgsl
  compute/             ← WASM plugins (compute.call)
    echo.wasm
  templates/           ← HTML templates
  static/              ← Static assets
  migrations/          ← SQL migrations
```

**Buffer sharing in pipelines:** Named buffers are created once and reused across stages. When multiple stages reference the same buffer name with different sizes, the maximum declared size is allocated. First stage with `data` uploads initial content; subsequent stages reuse the existing buffer. Persistent buffers (created via `gpu.buffer()`) participate by name.

**Memory-mapped file input (fs.mmap → GPU):**
```lua
-- Zero-copy: disk → mmap → GPU buffer (no Lua string intermediary)
app.manifest({ gpu = true, fs = { read = {"embeddings.bin"} } })

local mapped = fs.mmap("embeddings.bin")  -- mmap'd pointer
gpu.buffer("vectors", mapped)              -- pointer → wgpuQueueWriteBuffer directly
mapped:close()

-- Also works inline in dispatch/pipeline buffers:
local out = gpu.dispatch("search", {
    buffers = {{ data = mapped, size = mapped:len() }},
    workgroups = { x = 1024 },
    output = 1,
})
```

`gpu.buffer()`, `gpu.dispatch()`, and `gpu.pipeline()` all accept `MappedBuffer` (from `fs.mmap()`) as buffer data in both Lua and JS. This avoids copying large datasets through the scripting runtime.

**Binding layout:** Uniforms at binding 0 (if present), storage buffers at binding 1..N. WGSL shader `@binding()` annotations must match this auto-layout.

**Async dispatch:** `gpu.async.dispatch()` and `gpu.async.pipeline()` submit GPU work to the thread pool and yield to the event loop (Lua coroutine / JS Promise). Other requests are served while the GPU is working. Deep-copies all buffer data for thread safety.

**Sandbox:** When `manifest.gpu` is set:
- macOS: allows `iokit-open` and `com.apple.MTLCompilerService` mach-lookup
- Linux: unveils `/dev/dri` (rw) and `/proc/self` (r)

**Performance (Apple M1 Max, cosine similarity on 128-dim vectors):**

| Vectors | Native C | WASM AOT | GPU | GPU vs AOT |
|---------|----------|----------|-----|------------|
| 64 | 7 µs | 7 µs | 2,630 µs | 0.0x |
| 1K | 118 µs | 108 µs | 2,630 µs | 0.0x |
| 16K | 1,830 µs | 2,534 µs | 2,629 µs | 1.0x |
| 64K | 7,270 µs | 10,969 µs | 2,653 µs | **4.1x** |

GPU latency is constant ~2.6ms (dominated by submit+poll overhead). Crossover vs AOT at ~16K vectors. Use GPU for large parallel workloads; use WASM AOT for small sequential ones.

**GPU timeout:** Dispatches time out after 5 seconds (configurable via `HL_GPU_TIMEOUT_MS` at compile time). Returns `HL_GPU_ERR_TIMEOUT`. Prevents infinite hangs from shader bugs. Applies to `dispatch`, `pipeline`, and `buffer_copy`.

**Build:** `make fetch-wgpu` downloads and SHA-256 verifies wgpu-native. Then `make HL_ENABLE_GPU=1` auto-detects `vendor/wgpu/`. macOS links Metal + QuartzCore + CoreGraphics + Foundation; Linux links `-lvulkan`. Not compatible with Cosmopolitan builds.

### Unified Buffer Protocol

All compute and GPU functions accept any buffer type as input via the unified buffer protocol (`HlBufferView` in `include/hull/buffer.h`):

| Type | Source | Lua | JS |
|------|--------|-----|-----|
| String | Literals, `string.pack` | Default | N/A |
| ArrayBuffer | JS typed arrays | N/A | Default |
| MappedBuffer | `fs.mmap(path)` | Userdata | Object |
| WasmBuffer | `compute.call(name, input, { buffer = true })` | Userdata | Object |

All four types are accepted by `compute.call()`, `compute.segment()`, `gpu.buffer()`, `gpu.dispatch()` buffer data, and `gpu.pipeline()` buffer data. This enables zero-copy data flow:

```lua
-- Disk → GPU (zero-copy via mmap)
local mapped = fs.mmap("embeddings.bin")
gpu.buffer("vectors", mapped)
mapped:close()

-- WASM → GPU (zero-copy via WasmBuffer)
local processed = compute.call("preprocess", raw_data, { buffer = true })
gpu.buffer("features", processed)  -- WasmBuffer accepted directly
```

C helper functions:
- **Lua:** `lua_get_buffer(L, idx, &view)` — extracts `HlBufferView` from any buffer type at stack index
- **JS:** `js_get_buffer(ctx, val, &view, &str, &needs_free)` — same for JS values

### Compute API Harmonization

WASM and GPU compute share symmetric naming where the concepts align:

| Concept | WASM (`compute.*`) | GPU (`gpu.*`) |
|---------|-------------------|---------------|
| Availability | `compute.available()` | `gpu.available()` |
| Load from file | `compute.load(name)` | `gpu.load(name)` |
| Execute | `compute.call(name, input)` | `gpu.dispatch(name, opts)` |
| Async execute | `compute.async.call(...)` | `gpu.async.dispatch(...)` |
| Persistent state | `compute.instance(name)` | `gpu.buffer(name, data)` |
| Shared data | `compute.segment(mod, seg, data)` | `gpu.buffer(name, data)` |
| Input types | string, WasmBuffer, MappedBuffer | string, WasmBuffer, MappedBuffer |
| Execution limits | Gas metering (per-instruction) | Timeout (5s wall clock) |

Intentionally different: `call` vs `dispatch` (function call vs hardware dispatch), `instance` vs `buffer` (retained linear memory vs GPU storage), `segment` vs `buffer` (read-only shared heap vs read/write GPU buffer).

## Testing

Tests use Sheredom's utest.h. Each `tests/hull/*/test_*.c` is a standalone executable.

```bash
make test                           # run all unit tests
make debug && make test             # run under ASan + UBSan
make e2e                            # run all E2E tests (examples + build + sandbox)
./build/test_hull_cap_db            # run a single test suite
```

### Test Suites

| Suite | Tests | What it covers |
|-------|------:|----------------|
| `test_hull_cap_db` | 10 | SQLite query, exec, params, null, error handling |
| `test_hull_cap_time` | 8 | Timestamps, date formatting, buffer bounds |
| `test_hull_cap_env` | 7 | Allowlist enforcement, null safety |
| `test_hull_cap_crypto` | 11 | SHA-256, random, PBKDF2, Ed25519, null safety |
| `test_hull_cap_fs` | 14 | Path validation, read/write, traversal rejection |
| `test_js_runtime` | 13 | QuickJS init, eval, sandbox, modules, GC, limits |
| `test_lua_runtime` | 16 | Lua init, eval, sandbox, modules, GC, double-free |
| `test_static` | 18 | MIME detection, path traversal, embedded VFS lookup |
| `test_vfs` | 19 | Binary search find, prefix queries, path construction, empty VFS |
| `test_wasm` | 47 | WAMR init/destroy, module load, echo call, gas exhaustion, limits, pools, persistent instances, shared data segments |
| `test_gpu` | 13 | GPU init/destroy, device enumeration, shader compile (valid + invalid WGSL), dispatch with data doubling, persistent buffer roundtrip (real GPU tests skip if no adapter) |

\+ E2E suites (`e2e_build.sh`, `e2e_examples.sh`, `e2e_http.sh`, `e2e_sandbox.sh`)

### E2E Tests

| Script | What it tests |
|--------|---------------|
| `e2e_build.sh` | Build pipeline: platform build, app compilation, signing, self-build chain |
| `e2e_examples.sh` | All 9 examples in both Lua and JS runtimes |
| `e2e_http.sh` | HTTP routing, middleware, error handling |
| `e2e_sandbox.sh` | Kernel sandbox enforcement (OpenBSD + Linux + macOS + Cosmo) |
| `e2e_templates.sh` | Template engine: 20 tests per runtime (text, vars, escaping, conditionals, loops, filters, inheritance, includes, XSS) |
| `e2e_migrate.sh` | Migration system: apply, status, idempotency, embedding |
| `e2e_compute.sh` | WASM compute: compute.call() from Lua + JS, preload, error handling |
| `e2e_deploy.sh` | Deploy config generator: Dockerfile, systemd, fly.toml, agent deploy |

## Runtime Sandboxes

### QuickJS Sandbox
1. `eval()` removed (C-level `JS_Eval` still works for host code)
2. `std`/`os` modules NOT loaded
3. Memory limit via `JS_SetMemoryLimit()` (64 MB default)
4. Stack limit via `JS_SetMaxStackSize()` (1 MB default)
5. Instruction-count interrupt handler for gas metering
6. Only `hull:*` modules available

### Lua Sandbox
1. `io`/`os` libraries NOT loaded
2. `loadfile`, `dofile`, `load` globals removed
3. Memory limit via custom allocator with tracking (64 MB default)
4. Instruction-count hook for gas metering (`lua_sethook(LUA_MASKCOUNT)`, 100M default)
5. Only safe libs: base, table, string, math, utf8, coroutine
6. Custom `require()` resolves only from embedded stdlib registry
7. `hull.*` modules registered as globals

## Adding a New Capability Module

### 1. C Capability Layer
- Create `src/hull/cap/<name>.c` and `include/hull/cap/<name>.h`
- Implement `hl_cap_<name>_*()` functions with input validation
- Add to Makefile `HULL_CAP_SRC` and `HULL_CAP_OBJ`

### 2. Lua Bindings
- Add bindings in `src/hull/runtime/lua/modules.c`
- `luaL_Reg` array + `luaopen_hull_<name>()` opener
- Register in `hl_lua_register_modules()`

### 3. JavaScript Bindings
- Add bindings in `src/hull/runtime/js/modules.c`
- Init function + register in `hl_js_register_modules()`

### 4. Tests
- Unit tests in `tests/hull/cap/test_<name>.c`
- Add to Makefile test discovery

## Adding a New Subcommand

1. Create `src/hull/commands/<name>.c` and `include/hull/commands/<name>.h`
2. Implement `int hl_cmd_<name>(int argc, char **argv, const char *hull_path)`
3. Add one line to the command table in `src/hull/commands/dispatch.c`
4. Add Lua implementation in `stdlib/lua/hull/<name>.lua` if tool-mode command

## Debugging

```bash
make debug              # clean + rebuild with -fsanitize=address,undefined -g -O0
make msan               # clean + rebuild with -fsanitize=memory,undefined (Linux clang)
make test               # run tests under whichever sanitizer was built
```

ASan catches: heap/stack buffer overflow, use-after-free, double-free, memory leaks.
UBSan catches: signed overflow, null dereference, misaligned access, shift overflow.
MSan catches: use of uninitialized memory.
