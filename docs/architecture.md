# Hull — Architecture

## System Layers

```
┌─────────────────────────────────────────────────────┐
│  Application Code (Lua/JS/WASM)                     │  ← Developer writes this
├─────────────────────────────────────────────────────┤
│  Hull Standard Library (stdlib/)                    │  ← Pre-built, signed
│  hull.json, hull.build, hull.verify, etc.           │
├─────────────────────────────────────────────────────┤
│  Hull Runtimes (Lua 5.4 + QuickJS)                 │  ← Sandboxed interpreters
│  Custom allocators, blocked globals, gas metering   │
├─────────────────────────────────────────────────────┤
│  Hull Capability Layer (src/hull/cap/)              │  ← C enforcement boundary
│  fs, db, crypto, time, env, body, http, tool, test  │
├─────────────────────────────────────────────────────┤
│  Hull Core (src/hull/)                              │  ← Manifest, sandbox, sig
│  manifest.c, sandbox.c, signature.c, main.c         │
├─────────────────────────────────────────────────────┤
│  Keel HTTP Server (vendor/keel/)                    │  ← Event loop + routing
│  epoll/kqueue/poll, router, connection pool          │
├─────────────────────────────────────────────────────┤
│  Kernel Sandbox                                     │  ← OS enforcement
│  pledge + unveil (Linux seccomp/landlock, Cosmo)    │
├─────────────────────────────────────────────────────┤
│  Operating System                                   │
└─────────────────────────────────────────────────────┘
```

Each layer only talks to the one directly below it. Application code cannot bypass the capability layer to reach the kernel or filesystem.

---

## 1. Keel HTTP Server

Keel (`vendor/keel/`) is an independent C11 HTTP server library. Hull uses it as the transport layer.

**What Keel provides:**
- Event loop backends: epoll (Linux), kqueue (macOS), poll (universal POSIX fallback)
- HTTP/1.1 parsing via llhttp (pluggable parser vtable)
- Route matching with `:param` extraction
- Middleware chain (method + pattern filtering)
- Pre-allocated connection pool with state machine + timeout sweep
- Response builder: buffered (writev), sendfile, or streaming chunked
- Body reader vtable: pluggable readers for multipart, buffered, etc.
- TLS transport vtable (bring-your-own backend)

**What Keel does NOT do:**
- No application logic
- No database
- No sandboxing
- No scripting runtime

Hull registers routes and handlers with Keel's router, then runs Keel's event loop.

---

## 2. Hull Core

### Entry Point (`main.c`)

The server startup sequence:

1. Parse CLI flags (`--port`, `--db`, `--verify-sig`, `--runtime`, etc.)
2. Initialize allocator (default stdlib wrapper via `KlAllocator`)
3. Open SQLite database
4. Initialize Keel server (`kl_server_init`)
5. Select runtime (Lua or QuickJS) based on entry point extension
6. Initialize runtime with config (heap limit, stack limit)
7. Load and evaluate the app (`rt->vt->load_app`)
8. Verify app signature if `--verify-sig` provided
9. Wire routes from runtime into Keel router (`rt->vt->wire_routes_server`)
10. Extract manifest from runtime (`rt->vt->extract_manifest`)
11. Apply kernel sandbox based on manifest (`hl_sandbox_apply`)
12. Run Keel event loop

### Manifest Extraction (`manifest.c`)

The manifest declares what the app needs:

```lua
app.manifest({
    fs = { read = {"data/"}, write = {"data/uploads/"} },
    env = {"PORT", "DATABASE_URL"},
    hosts = {"api.stripe.com"}
})
```

Extraction reads the stored manifest table from the runtime:
- **Lua:** `__hull_manifest` in Lua registry → `hl_manifest_extract()`
- **QuickJS:** `globalThis.__hull_manifest` → `hl_manifest_extract_js()`

Result: `HlManifest` struct with up to 32 entries per category (`fs_read`, `fs_write`, `env`, `hosts`).

### Sandbox Application (`sandbox.c`)

After manifest extraction, `hl_sandbox_apply()` locks down the process:

| Step | Action | Effect |
|------|--------|--------|
| 1 | `unveil(path, "r")` for each `fs.read` path | Filesystem read-only |
| 2 | `unveil(path, "rwc")` for each `fs.write` path | Filesystem read-write |
| 3 | `unveil(db_path, "rwc")` for SQLite | Database access |
| 4 | `unveil(NULL, NULL)` | Seal — no more paths can be added |
| 5 | `pledge("stdio inet rpath wpath cpath flock [dns]")` | Syscall filter |

After sealing, any attempt to access undeclared paths triggers SIGKILL (Linux/Cosmo) or returns ENOENT.

### Signature Verification (`signature.c`)

Dual-layer Ed25519 signature system:

**Platform layer (inner):** Signed by gethull.dev. Proves the platform library is authentic.
- Payload: `canonicalStringify(platforms)` (per-arch hashes + canary)
- Key: Hardcoded `HL_PLATFORM_PUBKEY_HEX` (overridable via `--platform-key`)

**App layer (outer):** Signed by the developer. Proves the app hasn't been tampered with.
- Payload: `canonicalStringify({binary_hash, build, files, manifest, platform, trampoline_hash})`
- Key: Developer's `.pub` file

Full startup verification (`hl_verify_startup`):
1. Read developer public key from `.pub` file
2. Read and parse `package.sig`
3. Verify platform signature against pinned key
4. Verify app signature against developer key
5. Verify file hashes against embedded entries or filesystem
6. Return 0 (all valid) or -1 (any failure → refuse to start)

---

## 3. Capability Layer

Every capability is a C function that validates inputs before performing the operation. Application code (Lua/JS) can only access system resources through these functions.

### Filesystem (`cap/fs.c`)

- `hl_cap_fs_validate(path, base_dir)` — Rejects absolute paths, `..` components, symlink escapes via `realpath()` ancestor check
- `hl_cap_fs_read(path, base_dir, ...)` — Read file within base directory
- `hl_cap_fs_write(path, base_dir, ...)` — Write file, auto-creates parent directories
- `hl_cap_fs_exists()` / `hl_cap_fs_delete()` — Existence check and deletion

All paths must be relative and resolve within the declared base directory.

### Database (`cap/db.c`)

- `hl_cap_db_query(cache, sql, params, callback)` — SELECT with parameterized binding
- `hl_cap_db_exec(cache, sql, params)` — INSERT/UPDATE/DELETE with parameterized binding
- `hl_cap_db_begin/commit/rollback(db)` — explicit transaction control
- `db.batch(fn)` — Lua/JS API wrapping fn() in BEGIN IMMEDIATE..COMMIT

SQL is always a literal string from app code. Parameters are bound via SQLite's `sqlite3_bind_*` family. No string concatenation. SQL injection is structurally impossible.

**Prepared statement cache**: A 32-entry LRU cache (`HlStmtCache`) avoids repeated `sqlite3_prepare_v2()` calls for hot queries. Statements are reused via `sqlite3_reset()` + `sqlite3_clear_bindings()`.

**Performance PRAGMAs** (applied once at connection open via `hl_cap_db_init()`):

| PRAGMA | Value | Rationale |
|--------|-------|-----------|
| journal_mode | WAL | Concurrent readers during writes |
| synchronous | NORMAL | Sync on checkpoint only (safe in WAL mode) |
| foreign_keys | ON | Referential integrity |
| busy_timeout | 5000 | Wait 5s on lock contention |
| cache_size | -16384 | 16 MB page cache (vs 2 MB default) |
| temp_store | MEMORY | Temp tables in RAM |
| mmap_size | 268435456 | Memory-map up to 256 MB for reads |
| wal_autocheckpoint | 1000 | Checkpoint every ~4 MB of WAL |

**Shutdown**: `hl_cap_db_shutdown()` runs `PRAGMA optimize` and `wal_checkpoint(TRUNCATE)` for clean state.

### Crypto (`cap/crypto.c`)

All primitives backed by vendored TweetNaCl (770 lines, public domain):

- SHA-256, SHA-512 (hashing)
- HMAC-SHA256 (JWT/CSRF token signing)
- Base64url encode/decode (JWT encoding, no padding)
- PBKDF2 (key derivation)
- Ed25519 sign/verify/keypair (signatures)
- XSalsa20+Poly1305 secretbox (symmetric AEAD)
- Curve25519 box (asymmetric encryption)
- HMAC-SHA512/256 (authentication)
- `/dev/urandom` random bytes

Key material is zeroed from stack buffers after use via `hull_secure_zero()` (volatile memset, not optimizable away).

### Time (`cap/time.c`)

- `hl_cap_time_now()` — Unix timestamp (seconds)
- `hl_cap_time_now_ms()` — Milliseconds
- `hl_cap_time_clock()` — Monotonic clock (benchmarking)
- `hl_cap_time_date()` / `hl_cap_time_datetime()` — Formatted output

### Environment (`cap/env.c`)

- `hl_cap_env_get(name, config)` — Returns env var only if `name` is in the declared allowlist

Allowlist comes from manifest's `env` array (max 32 entries).

### HTTP Client (`cap/http.c`)

- `hl_cap_http_request(method, url, body, config)` — Outbound HTTP with host validation

Only hosts declared in manifest's `hosts` array are allowed.

### Tool (`cap/tool.c`) — Build Mode Only

- `hl_tool_spawn(argv, ...)` — Fork/exec with compiler allowlist (`cc`, `gcc`, `clang`, `cosmocc`, `cosmoar`, `ar`)
- `hl_tool_find_files(dir, pattern)` — Recursive glob (skips dotdirs, vendor, node_modules)
- `hl_tool_copy()` / `hl_tool_mkdir()` / `hl_tool_rmdir()` — Filesystem ops with unveil validation

No shell invocation (`system()`, `popen()`). Only allowlisted executables can be spawned.

### Test (`cap/test.c`)

- In-process test runner — direct router dispatch without TCP
- `test.get("/path")`, `test.post("/path", body)` — simulate HTTP requests
- `test.eq(a, b)`, `test.ok(val)`, `test.err(fn, pattern)` — assertions

---

## 4. Runtimes

Both runtimes implement a polymorphic vtable:

```c
typedef struct {
    int  (*init)(HlRuntime *rt, const void *config);
    int  (*load_app)(HlRuntime *rt, const char *filename);
    int  (*wire_routes_server)(HlRuntime *rt, KlServer *server, void *alloc_fn);
    int  (*extract_manifest)(HlRuntime *rt, HlManifest *out);
    void (*free_manifest_strings)(HlRuntime *rt, HlManifest *m);
    void (*destroy)(HlRuntime *rt);
} HlRuntimeVtable;
```

### Lua 5.4

**Sandboxing:**
- Custom allocator with per-request heap limit (default 64 MB)
- Globals removed: `io`, `os`, `loadfile`, `dofile`, `load`
- Custom `require()` resolves only from embedded stdlib registry
- Exceeding memory limit → NULL allocation → script error (not crash)

**Request dispatch:**
- KlRequest → Lua table (method, path, headers, body, params, ctx)
- Route handler called as Lua function (1-based index)
- Return marshaled via KlResponse builder

**Middleware context (`req.ctx`):**
- Middleware can set `req.ctx.session`, `req.ctx.user`, etc.
- After middleware returns, `req.ctx` is JSON-serialized and stored in `KlRequest.ctx` (void pointer)
- Next middleware or handler deserializes and merges `req.ctx` into a fresh table
- Size capped at 64KB to prevent unbounded allocation

### QuickJS

**Sandboxing:**
- Memory limit via `JS_SetMemoryLimit()` (default 64 MB)
- Stack limit via `JS_SetMaxStackSize()` (default 1 MB)
- Instruction-count interrupt handler for gas metering
- `eval()` disabled, no std/os module loading
- GC threshold (default 256 KB)

**Request dispatch:**
- KlRequest → JS object (method, path, headers, body, params, ctx)
- Route handler called as JS function
- Microtask queue drained after each request (`hl_js_run_jobs`)
- Instruction counter reset before each dispatch

**Middleware context (`req.ctx`):**
- Same serialization model as Lua — JSON round-trip through `KlRequest.ctx`
- Auth middleware attaches `{ sessionId, session }` or `{ token, claims }` to ctx

---

## 5. Standard Library

Embedded Lua/JS modules in `stdlib/`:

| Module | Purpose |
|--------|---------|
| `hull.json` | Canonical JSON encode/decode (sorted keys for deterministic signatures) |
| `hull.cookie` | Cookie parsing (`parse`) and serialization (`serialize`, `clear`) with secure defaults |
| `hull.session` | Server-side sessions backed by SQLite (create, load, update, destroy, cleanup) |
| `hull.jwt` | JWT HS256 sign/verify/decode — constant-time signature comparison, no "none" algorithm |
| `hull.csrf` | Stateless CSRF tokens via HMAC-SHA256 — generate, verify, middleware factory |
| `hull.auth` | Authentication middleware factories — session auth, JWT Bearer auth, login/logout helpers |
| `hull.build` | Full build pipeline: extract platform, collect files, generate trampoline, compile, link, sign |
| `hull.verify` | Dual-layer signature verification (CLI tool) |
| `hull.inspect` | Display capabilities + signature status |
| `hull.manifest` | Extract and print manifest as JSON |
| `hull.sign_platform` | Sign platform libraries with per-arch hashes |

Stdlib modules are compiled into the binary as byte arrays (auto-generated registry). They are resolved by the custom `require()` / module loader.

---

## 6. Build Pipeline

### Compilation

Three supported compiler paths:

| Compiler | Target | Binary Type |
|----------|--------|-------------|
| `gcc` / `clang` | Linux | ELF (platform-specific) |
| `gcc` / `clang` | macOS | Mach-O (platform-specific) |
| `cosmocc` | Any x86_64/aarch64 | APE (Actually Portable Executable) |

### Build Sequence (`hull build`)

1. Extract `libhull_platform.a` from embedded assets
2. Extract `app_main.c` template
3. Collect app source files (Lua/JS/HTML/CSS)
4. Generate `app_registry.c` — xxd byte arrays of all app files
5. Generate `app_main.c` from template + route registry
6. Compile `app_main.c` + `app_registry.c` with selected compiler
7. Link against `libhull_platform.a`
8. Sign with Ed25519: file hashes + binary hash + build metadata → `package.sig`

### Platform Library

`libhull_platform.a` contains everything except `main.c` and build-tool-specific code:
- Keel HTTP server
- Lua 5.4 + QuickJS runtimes
- All capability modules
- SQLite
- TweetNaCl
- Sandbox (pledge/unveil polyfill)

The platform library is signed separately (`platform.sig`) with the gethull.dev key.

---

## 7. Compiler/Platform Security Matrix

| Feature | gcc/clang (Linux) | gcc/clang (macOS) | cosmocc (APE) |
|---------|-------------------|-------------------|---------------|
| Stack protector | `-fstack-protector-strong` | `-fstack-protector-strong` | Built-in |
| Pledge (syscall filter) | jart/pledge (seccomp-bpf) | No-op | Native |
| Unveil (path restriction) | jart/pledge (landlock) | No-op | Native |
| ASLR | OS-provided | OS-provided | Static binary (N/A) |
| W^X enforcement | OS-provided | OS-provided | Cosmo enforces |
| Dynamic linking | Possible | Possible | Static only |
| LD_PRELOAD risk | Yes | Yes | N/A (static) |
| Cross-platform | Linux only | macOS only | Any x86_64/aarch64 |

### Defense Depth by Platform

**Linux (gcc/clang + jart/pledge):**
- Kernel sandbox: seccomp-bpf + Landlock (**strong**)
- C-level validation: always active
- Violation behavior: SIGKILL (unbypassable)

**Cosmopolitan APE (cosmocc):**
- Kernel sandbox: native pledge/unveil on Linux/FreeBSD/OpenBSD/Windows (**strong**)
- Static binary: no dynamic linking, no LD_PRELOAD (**eliminates class of attacks**)
- C-level validation: always active
- Violation behavior: SIGKILL

**macOS (gcc/clang):**
- Kernel sandbox: **not available** (pledge/unveil are no-ops)
- C-level validation: only defense layer
- Violation behavior: function returns error (no kill)
- Known limitation: bugs in C validation have no kernel backup
