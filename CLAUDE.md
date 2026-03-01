# HULL — Development Guide

## Build

```bash
make                    # build hull binary (epoll on Linux, kqueue on macOS)
make test               # build and run all 79 unit tests
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
make clean              # remove all build artifacts
```

### Dependencies

All vendored — no external dependencies:

| Library | Location | Purpose |
|---------|----------|---------|
| Keel | `vendor/keel/` (git submodule) | HTTP server library |
| Lua 5.4 | `vendor/lua/` | Application scripting |
| QuickJS | `vendor/quickjs/` | ES2023 JavaScript runtime |
| SQLite | `vendor/sqlite/` | Embedded database |
| mbedTLS | `vendor/mbedtls/` | TLS client |
| TweetNaCl | `vendor/tweetnacl/` | Ed25519 + NaCl crypto |
| pledge/unveil | `vendor/pledge/` | Linux kernel sandbox polyfill |
| log.c | `vendor/log.c/` | Logging |
| sh_arena | `vendor/sh_arena/` | Arena allocator |
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
stdlib/                 # Embedded standard library
  lua/hull/             #   Lua modules (json, cookie, session, jwt, csrf, auth, build, verify, etc.)
  js/hull/              #   JS modules (cookie, session, jwt, csrf, auth, verify)
vendor/                 # Vendored libraries (do not modify)
tests/                  # Unit tests (test_*.c) and E2E scripts (e2e_*.sh)
  fixtures/             #   Test fixtures (null_app, etc.)
  hull/                 #   Hull-specific test suites
examples/               # 8 example apps (hello, rest_api, auth, jwt_api, etc.)
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
Hull Core (main.c, manifest.c, sandbox.c, signature.c)
        ↓
Keel HTTP Server (vendor/keel/)  →  Event loop + routing
        ↓
Kernel Sandbox (pledge + unveil)  →  OS enforcement
```

Each layer talks only to the one below it. Application code cannot bypass capabilities.

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

### Request Flow

```
Client → Keel HTTP → Route Match → hl_{lua,js}_dispatch() → Handler → KlResponse
                                           ↓
                                    hl_cap_* API (shared C)
                                           ↓
                                    SQLite / FS / Crypto / HTTP
```

### Command Dispatch

Table-driven dispatcher in `src/hull/commands/dispatch.c`. 10 commands:

```
hull keygen | build | verify | inspect | manifest | test | new | dev | eject | sign-platform
```

Each command is a separate `.c`/`.h` under `src/hull/commands/`. Adding a new command = one line in the table + one source file.

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

Apps declare capabilities via `app.manifest()`. After extraction, `hl_sandbox_apply()` in `sandbox.c`:
1. `unveil(path, "r")` for each `fs.read` path
2. `unveil(path, "rwc")` for each `fs.write` path
3. `unveil(NULL, NULL)` — seal (no more paths)
4. `pledge("stdio inet rpath wpath cpath flock [dns]")` — syscall filter

Violation = SIGKILL on Linux/Cosmo. No-op on macOS (C-level validation only).

### Capability Enforcement Invariants

- **SQL injection impossible:** All DB access uses `sqlite3_bind_*` parameterized binding. SQL is always a literal string.
- **Path traversal blocked:** `hl_cap_fs_validate()` rejects absolute paths, `..` components, symlink escapes via `realpath()` ancestor check. Plus kernel unveil.
- **Host allowlist enforced:** `hl_cap_http_request()` validates target host against manifest's `hosts` array.
- **Env allowlist enforced:** `hl_cap_env_get()` checks against manifest's `env` array (max 32 entries).
- **No shell invocation:** Tool mode uses `hl_tool_spawn()` with compiler allowlist. No `system()`/`popen()`.
- **Key material zeroed:** `hull_secure_zero()` (volatile memset) scrubs crypto material from stack buffers.

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
| `cors` | `hull.cors` | `hull:cors` | CORS headers + preflight handling |
| `ratelimit` | `hull.ratelimit` | `hull:ratelimit` | In-memory rate limiting with configurable windows |
| `csrf` | `hull.csrf` | `hull:csrf` | Stateless CSRF token generation/verification |
| `auth` | `hull.auth` | `hull:auth` | Session-based and JWT-based authentication middleware |
| `session` | `hull.session` | `hull:session` | Server-side sessions backed by SQLite |
| `cookie` | `hull.cookie` | `hull:cookie` | Cookie parse/serialize helpers |
| `jwt` | `hull.jwt` | `hull:jwt` | JWT sign/verify (HMAC-SHA256) |
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

### Recommended Middleware Stack

Order matters — each middleware runs before the next:

```lua
local cors = require("hull.cors")
local ratelimit = require("hull.ratelimit")
local auth = require("hull.auth")
local csrf = require("hull.csrf")
local session = require("hull.session")

session.init()  -- create hull_sessions table

-- 1. Request ID (custom — assign unique ID early)
-- 2. Logging (custom — log method, path, ID)
-- 3. Rate limiting — reject abusive traffic before doing any work
app.use("*", "/api/*", ratelimit.middleware({ limit = 60, window = 60 }))
-- 4. CORS — must run before auth so preflight doesn't require credentials
app.use("*", "/api/*", cors.middleware({ origins = { "https://myapp.com" } }))
-- 5. Authentication — session or JWT
app.use("*", "/api/*", auth.session_middleware({}))
-- 6. CSRF — only for session-based apps (not needed with JWT Bearer)
app.use("POST", "/api/*", csrf.middleware({ secret = "change-me" }))
-- 7. Route handlers
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

## Testing

Tests use Sheredom's utest.h. Each `tests/hull/*/test_*.c` is a standalone executable.

```bash
make test                           # run all 79 unit tests
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

**Total: 79 unit tests** + E2E suites (`e2e_build.sh`, `e2e_examples.sh`, `e2e_http.sh`, `e2e_sandbox.sh`)

### E2E Tests

| Script | What it tests |
|--------|---------------|
| `e2e_build.sh` | Build pipeline: platform build, app compilation, signing, self-build chain |
| `e2e_examples.sh` | All 8 examples in both Lua and JS runtimes |
| `e2e_http.sh` | HTTP routing, middleware, error handling |
| `e2e_sandbox.sh` | Kernel sandbox enforcement (Linux + Cosmo) |

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
4. Only safe libs: base, table, string, math, utf8, coroutine
5. Custom `require()` resolves only from embedded stdlib registry
6. `hull.*` modules registered as globals

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
