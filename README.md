<p align="center">
  <img src="hull_logo.svg" alt="Hull" width="480">
</p>

# Hull

A capability-secure runtime for local-first, agent-native software. Runs Lua/JS and WASM under declared permissions, SQLite storage, and single-binary deployment.

Hull is a sandboxed runtime where application code executes within declared capability boundaries. The app specifies what it can access — files, environment variables, outbound hosts, and database resources — and the runtime enforces those permissions.

Write backend logic in Lua or JavaScript, frontend in HTML5, and store data in SQLite. `hull build` produces a single portable executable — under 10 MB — that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, and NetBSD.

Everything is packaged together: code, schema, static assets, tests, and signatures. No external services, no hidden dependencies. The resulting binary is the product.

AI coding agents get structured JSON access to routes, schema, tests, and HTTP responses through the built-in `hull agent` command — no plugins, no MCP servers, no configuration.

## Why

AI coding agents solved code generation. They created two new problems: deployment and trust.

Hull addresses both. Applications are distributed as a single file, with no infrastructure required. Each app declares its capabilities in a manifest enforced by the runtime, so users can verify exactly what it can access.

In a world where AI writes code, the runtime becomes the trust boundary. Hull is that boundary.

## Name

Hull — Hardened Userspace Lockdown Layer.

- **Hardened** — capability-secure, sandboxed execution  
- **Userspace** — single binary, no kernel modules, no cloud  
- **Lockdown** — the app declares what it can access, the runtime enforces it  
- **Layer** — sits on top of Keel (Kernel Event Engine — Lightweight), mirroring the nautical metaphor

## Quick Start

### Install

```bash
# Download the latest signed release binary:
curl -fsSL https://raw.githubusercontent.com/artalis-io/hull/main/install.sh | sh
```

The installer detects your OS/arch, downloads a native binary (or the universal Cosmopolitan APE fallback), verifies the SHA-256 checksum against the release manifest, and installs to `~/.local/bin/hull` (no sudo). Override with `HULL_PREFIX=/usr/local/bin sh install.sh`.

Environment knobs: `HULL_VERSION=v0.1.0` (specific tag), `HULL_FLAVOR=cosmo` (force universal APE), `HULL_FORCE=1` (overwrite existing), `HULL_DRY_RUN=1` (preview only).

Or build from source:

```bash
git clone --recursive https://github.com/artalis-io/hull
cd hull
make
make test
```

### Develop

```bash
# Create a new project
hull new myapp
cd myapp

# Run in development mode (hot reload)
hull dev app.lua

# Build a standalone binary
hull build -o myapp .

# Run it
./myapp -p 8080 -d app.db
```

### Shell completions

Tab-completion for bash, zsh, and fish is shipped in [`completions/`](completions/) — see [completions/README.md](completions/README.md) for install instructions.

## Hull Tools

Hull ships 20 subcommands for the full development lifecycle:

| Command | Purpose |
|---------|---------|
| `hull new &lt;name&gt;` | Scaffold a new project in a new directory |
| `hull init [dir]` | Initialize a hull project in-place (idempotent, like `git init`) |
| `hull dev &lt;app&gt;` | Development server with hot reload |
| `hull build -o &lt;out&gt; &lt;dir&gt;` | Compile app into a standalone binary |
| `hull build --compiler=tcc\|system\|&lt;path&gt;` | Select compiler backend (default: embedded tcc if available, else system cc) |
| `hull test &lt;dir&gt;` | In-process test runner (no TCP, memory SQLite) |
| `hull deploy &lt;target&gt; [app_dir]` | [Generate deployment configs](#deployment) — Dockerfile, systemd, fly.toml |
| `hull agent &lt;subcommand&gt;` | [AI agent interface](#using-hull-with-ai-agents) — routes, schema, tests, requests as JSON |
| `hull inspect &lt;dir&gt;` | Display declared capabilities and signature status |
| `hull verify [--developer-key &lt;key&gt;]` | Verify Ed25519 signatures and file integrity |
| `hull eject &lt;dir&gt;` | Export to a standalone Makefile project |
| `hull keygen &lt;name&gt;` | Generate Ed25519 signing keypair |
| `hull sign-platform &lt;key&gt;` | Sign platform library with per-arch hashes |
| `hull manifest &lt;app&gt;` | Extract and print manifest as JSON |
| `hull version [--json]` | Print version string (`--json` for machine-readable output) |
| `hull doctor [--json]` | Check environment: compiler available, platform embedded, hull build readiness |
| `hull update [--check] [--force] [--channel=beta]` | Self-update from GitHub releases (verifies SHA-256, atomic replace) |
| `hull &lt;app&gt; --max-instructions N` | Set per-request instruction limit (default: 100M) |
| `hull &lt;app&gt; --audit` | Enable capability audit logging (JSON to stderr) |
| `hull &lt;app&gt; --max-connections N` | Max concurrent connections (default: 256) |
| `hull &lt;app&gt; --body-max-size SIZE` | Max request body size (default: 1m) |
| `hull &lt;app&gt; --read-timeout MS` | Read timeout in milliseconds (default: 30000) |
| `hull &lt;app&gt; --workers N` | Thread pool worker count (default: 4) |
| `hull &lt;app&gt; --queue-capacity N` | Thread pool queue capacity (default: 64) |
| `hull &lt;app&gt; --no-compress` | Disable gzip response compression |
| `hull &lt;app&gt; --ca-bundle PATH` | Custom CA bundle (overrides system + embedded) |
| `hull &lt;app&gt; --no-ca-bundle` | Skip TLS certificate verification (dev only) |
| `hull migrate [app_dir]` | Run pending SQL migrations |
| `hull migrate status` | Show migration status (applied/pending) |
| `hull migrate new &lt;name&gt;` | Create a new numbered migration file |
| `hull compute new &lt;name&gt; [--lang c]` | [Scaffold a new WASM compute module](#authoring-compute-modules) under `compute/&lt;name&gt;/` |
| `hull compute build [name]` | Compile `compute/&lt;name&gt;/&lt;name&gt;.c` → `compute/&lt;name&gt;.wasm` (all modules if no name) |
| `hull compute test &lt;name&gt;` | Run JSON fixtures against a compiled module |
| `hull compute check &lt;name&gt;` | Validate that a `.wasm` module loads correctly in WAMR |
| `hull compute refresh-header [name]` | Overwrite per-module `hull_compute.h` from the canonical embedded version (all modules if no name) |

### Build Pipeline

```
Source files (Lua/JS/HTML/CSS/static assets)
        ↓
hull build: collect → generate sorted registry (hl_app_entries[]) → compile → link → sign
        ↓
Single binary + package.sig (Ed25519 signed)
```

The build links against `libhull_platform.a` — a static archive containing Keel HTTP server, Lua 5.4, QuickJS, SQLite, mbedTLS, TweetNaCl, and the kernel sandbox. The platform library is signed separately with the gethull.dev key.

### Cross-Platform Builds

Hull supports four compiler targets:

| Compiler | Target | Binary Type | Notes |
|----------|--------|-------------|-------|
| Embedded TinyCC | Linux / macOS | ELF / Mach-O | Default; zero-dependency; compile-only (links via system ld) |
| `gcc` / `clang` | Linux / macOS | ELF / Mach-O | `--compiler=system` or explicit path |
| `cosmocc` | Any x86_64/aarch64 | APE (Actually Portable Executable) | Multi-arch fat binary |

**Zero-dependency builds:** Distribution builds of Hull embed a copy of [TinyCC](https://github.com/TinyCC/tinycc) (mob branch, ~400 KB). When no system compiler is installed, `hull build` uses the embedded TinyCC for the compile step and falls back to the system linker (`ld`/`cc`) for linking. `hull doctor` reports whether TinyCC is embedded and whether a system compiler is available.

```bash
hull build -o myapp .                  # auto-select: embedded tcc → system cc → gcc/clang
hull build -o myapp . --compiler=tcc   # force embedded tcc (compile) + system linker
hull build -o myapp . --compiler=system  # force system cc (no tcc fallback)
hull build -o myapp . --compiler=/path/to/cc  # explicit compiler path
```

Cosmopolitan APE binaries run on Linux, macOS, Windows, FreeBSD, OpenBSD, and NetBSD from a single file. Hull builds multi-architecture platform archives (`make platform-cosmo`) so the resulting APE binary is a true fat binary for both x86_64 and aarch64.

## Architecture

```
┌─────────────────────────────────────────────┐
│  Application Code (Lua / JS)                │  ← Developer writes this
├─────────────────────────────────────────────┤
│  WASM Compute Plugins (WAMR)                │  ← Sandboxed data-plane computation
├─────────────────────────────────────────────┤
│  GPU Compute Shaders (wgpu-native)          │  ← Parallel data processing (optional)
├─────────────────────────────────────────────┤
│  Standard Library (stdlib/)                 │  ← cors, ratelimit, csrf, auth, jwt, session
├─────────────────────────────────────────────┤
│  Runtimes (Lua 5.4 + QuickJS)              │  ← Sandboxed interpreters
├─────────────────────────────────────────────┤
│  Capability Layer (src/hull/cap/)           │  ← C enforcement boundary
│  fs, db, crypto, time, env, http, gpu, tool │  ← audit logging (--audit)
├─────────────────────────────────────────────┤
│  Hull Core                                  │  ← Manifest, sandbox, signatures, VFS
├─────────────────────────────────────────────┤
│  Keel HTTP Server (vendor/keel/)            │  ← Event loop + routing + async + thread pool
│                                             │  ← gzip compression + client connection pool
├─────────────────────────────────────────────┤
│  Kernel Sandbox (pledge + unveil)           │  ← OS enforcement
└─────────────────────────────────────────────┘
```

Each layer only talks to the one directly below it. Application code cannot bypass the capability layer.

### Standard Library

Hull ships a full set of middleware and utility modules for building secure backends:

| Module | Lua | JS | Purpose |
|--------|-----|-----|---------|
| `cors` | `hull.middleware.cors` | `hull:middleware:cors` | CORS headers + preflight handling |
| `ratelimit` | `hull.middleware.ratelimit` | `hull:middleware:ratelimit` | In-memory rate limiting with configurable windows |
| `csrf` | `hull.middleware.csrf` | `hull:middleware:csrf` | Stateless CSRF token generation/verification |
| `auth` | `hull.middleware.auth` | `hull:middleware:auth` | Session-based and JWT-based authentication middleware |
| `session` | `hull.middleware.session` | `hull:middleware:session` | Server-side sessions backed by SQLite |
| `cookie` | `hull.cookie` | `hull:cookie` | Cookie parse/serialize helpers |
| `jwt` | `hull.jwt` | `hull:jwt` | JWT sign/verify (HMAC-SHA256) |
| `template` | `hull.template` | `hull:template` | HTML template engine with inheritance, includes, filters |
| `csv` | `hull.csv` | `hull:csv` | CSV parse/encode (RFC 4180) |
| `search` | `hull.search` | `hull:search` | Full-text search (SQLite FTS5) |
| `rbac` | `hull.middleware.rbac` | `hull:middleware:rbac` | Role-based access control |
| `logger` | `hull.middleware.logger` | `hull:middleware:logger` | Request logging with logfmt output and request IDs |
| `transaction` | `hull.middleware.transaction` | `hull:middleware:transaction` | Wraps handlers in SQLite BEGIN IMMEDIATE..COMMIT |
| `idempotency` | `hull.middleware.idempotency` | `hull:middleware:idempotency` | Idempotency-Key middleware with response caching |
| `outbox` | `hull.middleware.outbox` | `hull:middleware:outbox` | Transactional outbox for reliable webhook delivery |
| `inbox` | `hull.middleware.inbox` | `hull:middleware:inbox` | Inbox deduplication for incoming events/webhooks |
| `validate` | `hull.validate` | `hull:validate` | Declarative input validation with schema rules |
| `form` | `hull.form` | `hull:form` | URL-encoded form body parsing |
| `i18n` | `hull.i18n` | `hull:i18n` | Internationalization: locale detection, translations, formatting |
| `health` | `hull.middleware.health` | `hull:middleware:health` | Health check + readiness endpoints |
| `etag` | `hull.middleware.etag` | `hull:middleware:etag` | ETag response helpers with 304 Not Modified |
| `json` | `hull.json` | (built-in) | JSON encode/decode |

All middleware modules follow the same factory pattern: `module.middleware(opts)` returns a function `(req, res) -> 0|1` where `0` = continue, `1` = short-circuit.

#### Background Timers

`app.every(ms, fn)` and `app.daily("HH:MM", fn)` register repeating background callbacks. Timer callbacks run on the event loop thread with full async support — `hull.sleep()`, `http.fetch()`, and `db.*` all work inside timers.

```lua
-- Flush outbox every 30 seconds
app.every(30000, function()
    outbox.flush()
end)

-- Clean up expired sessions daily at 2am UTC
app.daily("02:00", function()
    session.cleanup()
end)

-- Return false to self-cancel
app.every(1000, function()
    local pending = outbox.flush()
    if pending == 0 then return false end
end)
```

Minimum interval: 100ms. Errors are logged but don't stop the timer. One invocation at a time — if a callback is still running (async yield), the next tick is deferred.

#### Database & Migrations

Hull apps use SQLite with WAL mode, parameterized queries, and a prepared statement cache. Schema changes are managed through numbered SQL migration scripts.

**Convention:** Place migration files in `migrations/` in your app directory, numbered sequentially:

```
myapp/
  app.lua
  migrations/
    001_init.sql        ← creates initial tables
    002_add_index.sql   ← adds an index
    003_new_feature.sql ← adds a new table
```

Migrations run automatically on startup (opt out with `--no-migrate`). Each migration runs in a transaction, and the `_hull_migrations` table tracks which migrations have been applied.

```bash
hull migrate new add_tags        # creates migrations/002_add_tags.sql
hull migrate                     # run pending migrations
hull migrate status              # show applied/pending migrations
```

In built binaries (`hull build`), migration files are embedded alongside Lua/template/static assets. `hull test` runs migrations against an in-memory database.

#### Static File Serving

Place files in `static/` in your app directory — they're served at `/static/*` automatically.

```
myapp/
  app.lua
  static/
    style.css       → GET /static/style.css
    js/app.js       → GET /static/js/app.js
    images/logo.png → GET /static/images/logo.png
```

In dev mode, files are read from disk with zero-copy sendfile and `Cache-Control: no-cache`. In built binaries (`hull build`), static files are embedded in the unified `hl_app_entries[]` array and looked up via the VFS module (O(log n) binary search). `Cache-Control: public, max-age=86400`. ETag and 304 Not Modified are supported in both modes.

#### Backend Best Practices

Recommended middleware stack for a typical API backend:

```lua
local cors = require("hull.middleware.cors")
local ratelimit = require("hull.middleware.ratelimit")
local auth = require("hull.middleware.auth")
local session = require("hull.middleware.session")

session.init()

-- Order matters: rate limit → CORS → auth → routes
app.use("*", "/api/*", ratelimit.middleware({ limit = 100, window = 60 }))
app.use("*", "/api/*", cors.middleware({ origins = {"https://myapp.com"} }))
app.use("*", "/api/*", auth.session_middleware({}))

app.get("/api/me", function(req, res)
    res:json({ user = req.ctx.session })
end)
```

Key principles: rate limit before auth (reject early), CORS before auth (preflight must not require credentials), scope middleware to paths (`"/api/*"` not `"/*"`). For simple CORS needs, use `cors` in `app.manifest()` — it registers Keel-level CORS middleware automatically. Use the stdlib `hull.middleware.cors` for per-route or conditional CORS logic. See [examples/middleware/](examples/middleware/) and [CLAUDE.md](CLAUDE.md) for full API reference.

### Vendored Libraries

| Component | Purpose |
|-----------|---------|
| [Keel](https://github.com/artalis-io/keel) | HTTP server (epoll/kqueue/io_uring/poll), routing, middleware, TLS vtable, async primitives, thread pool |
| [Lua 5.4](https://www.lua.org/) | Application scripting (1.9x faster than QuickJS) |
| [QuickJS](https://bellard.org/quickjs/) | ES2023 JavaScript runtime with instruction-count gas metering |
| [SQLite](https://sqlite.org/) | Embedded database (WAL mode, parameterized queries) |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | TLS client for outbound HTTPS |
| [TweetNaCl](https://tweetnacl.cr.yp.to/) | Ed25519 signatures, XSalsa20+Poly1305, Curve25519 |
| [miniz](https://github.com/richgel999/miniz) | gzip compression (response compression, client decompression) |
| [pledge/unveil](https://github.com/jart/pledge) | Kernel sandbox (Linux seccomp/landlock) |

## Security Model

Hull apps declare a manifest of exactly what they can access — files, hosts, environment variables. The kernel enforces it.

```lua
app.manifest({
    fs = { read = {"data/"}, write = {"data/uploads/"} },
    env = {"PORT", "DATABASE_URL"},
    hosts = {"api.stripe.com"},
    cors = {
        origins = {"https://myapp.com"},
        methods = "GET, POST, PUT, DELETE",
        credentials = true,
    },
})
```

**Three verification points:**

| When | Tool | Checks |
|------|------|--------|
| Before download | [verify.gethull.dev](https://verify.gethull.dev) (offline browser tool) | Platform sig, app sig, canary, manifest |
| Before install | `hull verify --developer-key dev.pub` | Both signatures + file hashes |
| At startup | `./myapp --verify-sig dev.pub` | Signatures verified before accepting connections |

**Defense depth by platform:**

| Platform | Kernel Sandbox | Violation | Static Binary |
|----------|---------------|-----------|---------------|
| Linux (gcc/clang) | seccomp-bpf + Landlock | SIGKILL | No |
| Cosmopolitan APE | Native pledge/unveil | SIGKILL | Yes (no LD_PRELOAD) |
| macOS | Seatbelt (sandbox_init, deny-default SBPL) | EPERM | No |

See [docs/security.md](docs/security.md) for the full attack model and [docs/architecture.md](docs/architecture.md) for implementation details.

### Audit Logging

Hull can log every capability call (database queries, file I/O, HTTP requests, env access) as structured JSON to stderr. Off by default for zero overhead.

```bash
# Enable via CLI flag
hull app.lua --audit

# Or via environment variable
HULL_AUDIT=1 hull app.lua
```

Each line is a self-contained JSON object with a UTC timestamp and the capability name:

```
{"ts":"2026-03-06T14:23:01Z","cap":"db.query","sql":"SELECT * FROM tasks WHERE id = ?","nparams":1,"result":0}
{"ts":"2026-03-06T14:23:01Z","cap":"fs.read","path":"uploads/file.txt","bytes":4096}
{"ts":"2026-03-06T14:23:02Z","cap":"http.request","method":"POST","url":"https://api.example.com","status":200,"result":0}
{"ts":"2026-03-06T14:23:02Z","cap":"env.get","name":"DATABASE_URL","result":"ok"}
```

When disabled (default), the audit check is a single branch on a global flag — zero escaping, formatting, or I/O.

## Performance

77,000–86,000 requests/sec on a single core. ~15% overhead vs raw C (Keel baseline: 101,000 req/s). SQLite write-heavy routes sustain 19,000 req/s.

| Route | Lua 5.4 | QuickJS |
|-------|--------:|--------:|
| GET /health (no DB) | 98,531 req/s | 52,263 req/s |
| GET / (DB write + JSON) | 6,866 req/s | 4,588 req/s |
| GET /greet/:name (params) | 102,204 req/s | 57,405 req/s |

See [docs/benchmark.md](docs/benchmark.md) for methodology.

## Authoring Compute Modules

WASM compute modules let you drop pure-function workloads (scoring,
transforms, parsing, vector math, image kernels) into a Hull app at
near-native speed without leaving the capability-secure sandbox.
Modules are written in C (Rust planned), compiled to WebAssembly,
and either embedded in the final binary or loaded from disk in dev.

> See [docs/wamr_architecture.md](docs/wamr_architecture.md) for the
> full WAMR design (ABI, gas metering, pooling, segments, streaming,
> AOT, Memory64); this section is the developer guide.

### Directory convention

A compute module is a pair of paths inside your app:

```
myapp/
  app.lua
  compute/
    score.wasm           ← compiled artifact (what gets embedded)
    score/               ← source directory (one per module)
      score.c            ← required: the actual source
      hull_compute.h     ← required: the freestanding ABI header
      test_fixtures.json ← optional: input/output fixtures for `hull compute test`
```

The runtime only ever loads `compute/<name>.wasm` (and the AOT siblings
`compute/<name>.aot.<arch>`). The source directory `compute/<name>/`
is purely a build input — it's not embedded in the final binary and
nothing in production looks at it.

### Lifecycle

```
                                              hull build (auto)
                                                  ▼
hull compute new score          ┌───────────────────┐
        │                       │ compute/score.wasm│ ──── embedded into
        ▼                       └───────────────────┘      the app binary
compute/score/score.c (edit)            ▲
        │                               │
        ├──► hull compute build score ──┘   (manual rebuild)
        │
        ├──► hull compute test  score        (run JSON fixtures)
        │
        └──► hull compute check score        (load + smoke-test in WAMR)
```

### Step 1 — Scaffold

```bash
hull compute new score
```

Creates:

- `compute/score/score.c` — a 30-line skeleton implementing
  `hull_process(in, in_len, out, out_max)`. The default scaffold sums
  input bytes into a 0-100 score; replace it with your actual logic.
- `compute/score/hull_compute.h` — the freestanding ABI header
  (described below). Don't edit it; it's owned by Hull.
- `compute/score/test_fixtures.json` — a few example fixtures.

`hull compute new` is **idempotent-safe**: re-running on an existing
module errors instead of clobbering. Names must match
`[A-Za-z0-9_-]+`.

### Step 2 — Write your module

Implement `hull_process` in `compute/<name>/<name>.c`. The ABI is the
function signature you export, nothing more:

```c
#include "hull_compute.h"

HULL_VERSION_EXPORT

HULL_EXPORT
int32_t hull_process(const void *in_ptr, int32_t in_len,
                     void *out_ptr, int32_t out_max)
{
    /* Read up to in_len bytes from in_ptr.
     * Write up to out_max bytes to out_ptr.
     * Return number of bytes written, or a negative HULL_ERR_* code. */
    if (out_max < 4) return HULL_ERR_OUTPUT;
    /* ... your logic ... */
    return 4;
}
```

`hull_compute.h` is freestanding (no libc dependency) and provides:

| API | Use |
|-----|-----|
| `HULL_EXPORT` / `HULL_VERSION_EXPORT` | Visibility macros — required on `hull_process` and `hull_version`. |
| `HULL_OK`, `HULL_ERR_OUTPUT`, `HULL_ERR_INPUT`, `HULL_ERR_INTERNAL` | Standard return codes. |
| `int32_t host_call(opcode, ptr, len)` | The single host import — for logging, shared-data access, and UDF callbacks. |
| `hull_log(msg, len)` | Convenience wrapper around `host_call(HULL_OP_LOG, ...)`. |
| `hull_segment_count()` / `hull_segment_addr(id)` / `hull_segment_size(id)` | Read shared data segments loaded via `compute.segment(...)`. |
| `hull_memcpy`, `hull_memset`, `hull_memcmp`, `hull_strlen` | Minimal libc replacements. |
| `hull_alloc(n)` / `hull_alloc_reset()` | 64 KB bump allocator scoped to one call. |
| `HULL_UDF_*` constants | UDF wire format for modules registered as SQL functions via `db.udf.register`. |

Modules cannot import anything else — no WASI, no `open`, no `socket`,
no `time`. They are pure functions from input bytes to output bytes
with a strict gas budget (default 100M instructions per call) and a
strict memory budget (default 2 MiB heap, configurable up to ~4 GiB
on WASM32 or 16 GiB on Memory64).

> **Keeping `hull_compute.h` current.** Hull owns this header; the
> canonical version is embedded in the `hull` binary and written into
> each module's directory by `hull compute new`. When you upgrade Hull
> the per-module copies will not change automatically. Run
> `hull compute refresh-header <name>` (or with no name for all
> modules) to overwrite each `compute/<name>/hull_compute.h` from the
> embedded canonical version.

### Step 3 — Build

```bash
hull compute build score        # one module
hull compute build              # all modules under compute/
```

`hull compute build` invokes `clang --target=wasm32-unknown-unknown -nostdlib`
with the flags Hull's runtime expects (`-Wl,--no-entry`, exports
`hull_process` + `hull_version` + `memory`, 128 KiB initial / 64 MiB max
linear memory). Toolchain expectations:

- **macOS**: `brew install llvm@18` (bundles `wasm-ld`). The build
  prefers `/opt/homebrew/opt/llvm@18/bin/clang` then falls back to
  Homebrew `llvm` and finally to system `clang`.
- **Linux**: `apt install clang lld` (or equivalent — `wasm-ld` must
  be in PATH).
- **No toolchain installed?** `hull compute build` errors with a hint;
  pre-compiled `.wasm` files can be committed and used directly.

Output: `compute/<name>.wasm`. The compiler removes everything you
don't reference (LTO + `-O2`), so a typical module is 500 bytes - 5 KB.

### Step 4 — Test

`compute/<name>/test_fixtures.json` is a JSON array of test cases:

```json
[
    {"name": "happy path", "input": "hello", "expect_status": 0},
    {"name": "empty input", "input": "", "expect_status": 0},
    {"name": "rejects nul", "input": "
]
```

Each fixture's `input` is fed to `compute.call("<name>", input)`. The
runner asserts the HTTP status matches `expect_status` (200 if `0`,
otherwise the literal value).

```bash
hull compute test score
```

Under the hood the runner generates a tempdir app with one route
(`GET /call?input=...`) and runs `hull test` against it. This means
your fixtures exercise the exact same `compute.call` codepath your
real handlers use — same gas metering, same instance pool, same
limits.

For one-shot invocations with arbitrary inputs (no fixture file
needed), use `hull agent compute-call <name> <input-file>`.

### Step 5 — Sanity-check

```bash
hull compute check score
```

Validates the WASM magic bytes, the WASM version, then loads the
module in WAMR with a trivial input and asserts the call returns 200.
This is the "yes, this module can actually run inside Hull" gate. If
`hull compute check` passes, `compute.call()` from your app code will
at least find the module.

### Step 6 — Build and embed

`hull build` automatically:

1. **Rebuilds stale sources.** Before any other build step, it scans
   `compute/<name>/<name>.c` for sources whose mtime is newer than the
   matching `.wasm`. Any stale source is recompiled inline using the
   same logic as `hull compute build`. Output is reported as
   `hull build: compiled N compute source(s): <names>`.
2. **AOT-compiles `.wasm` artifacts.** If `wamrc` is available (`make
   wamrc`), every `compute/*.wasm` is AOT-compiled to
   `compute/*.aot.<arch>` for near-native speed. For cosmocc fat
   binaries both `x86_64` and `aarch64` AOT files are produced.
3. **Embeds both into the binary** via the unified VFS array. At
   runtime, AOT is preferred over interpreter; `hull build --no-aot`
   skips AOT compilation.

Opt out of step 1 with `--no-build-compute` (for hermetic CI builds
that ship pre-committed `.wasm` artifacts). Opt out of step 2 with
`--no-aot`.

Once embedded, modules are loaded via `compute.call("<name>", input)`
from Lua or `compute.call("<name>", input)` from JS, just like any
other capability. The bytes-in-bytes-out contract is the entire API.

### Deployment

`hull agent deploy <app_dir>` reports per-module status in its JSON
output:

```json
{
  "files": { "compute": 3, ... },
  "compute_modules": [
    {"name": "score",       "wasm_size": 1023, "has_aot": true,  "has_source": true,  "source_stale": false},
    {"name": "transform",   "wasm_size": 718,  "has_aot": true,  "has_source": true,  "source_stale": false},
    {"name": "vendored",    "wasm_size": 135,  "has_aot": false, "has_source": false, "source_stale": false}
  ],
  "recommendations": [
    "Compute source newer than .wasm — run `hull compute build` or rebuild (hull build auto-rebuilds when clang is available)"
  ]
}
```

`hull deploy dockerfile`, `hull deploy systemd`, and `hull deploy fly`
don't need any compute-specific configuration: the `.wasm` (and any
AOT) files are already embedded in the binary at `hull build` time, so
the deployment artifact is whatever your build pipeline produces. The
generated configs treat compute modules transparently.

For pure compute services (no DB), combine with `HL_ENABLE_DB=0`:

```bash
make HL_ENABLE_DB=0          # ~3.66 MB binary, no SQLite
hull build myapp             # embeds compute/*.wasm + AOT
```

### Sharing data with modules

For multi-GB read-only datasets (routing graphs, ML weights, embeddings),
load via shared data segments instead of passing through input bytes:

```lua
compute.segment("router", "graph", fs.mmap("graph.bin"))
local out = compute.call("router", query)
```

Inside the module, query segments via `host_call(0x02, segment_id, 0/1)`:

```c
void *graph = (void *)(size_t)host_call(0x02, 0, 0);  /* segment 0 address */
int32_t graph_size = host_call(0x02, 0, 1);           /* segment 0 size */
```

Segments are page-aligned mmap regions in shared heaps. Multiple
worker threads read concurrently. Adding or removing segments drains
the instance pool and rebuilds it transparently.

### Stateful compute (persistent instances)

For stateful workloads (model weights you want to keep between calls,
pre-built indexes), use `compute.instance()` instead of `compute.call`:

```lua
local m = compute.instance("model", { heap = 64 * 1024 * 1024 })
m:call(query_1)   -- linear memory persists between calls
m:call(query_2)
m:close()
```

### Streaming I/O

For datasets larger than memory:

```lua
compute.stream("transform", { file = "input.csv" },
                            { file = "output.json" },
                            { chunk_size = 65536 })
```

The module exports `hull_process_chunk` instead of `hull_process` and
queries chunk metadata via `host_call(0x03, ...)`. See `examples/compute/`
for working modules.

### What about Rust?

C is the only supported source language today. Rust support
(`--lang=rust`) is on the roadmap — it requires Cargo + a
`#[no_mangle] extern "C"` template + `wasm32-unknown-unknown` target
+ a `panic_handler`. Until then, modules in any compiled language are
loadable as long as they expose `hull_process` and `hull_version` with
the documented signatures (`Rust`, `Zig`, `AssemblyScript`, `TinyGo`
all work — only the scaffolding shortcut is C-only).

### WASM Compute Overhead

WASM plugins run in isolated linear memory with no I/O. Both sync (`compute.call()`) and async (`compute.async.call()`) APIs are available — async yields to the event loop so other requests are served during execution. The overhead vs native C depends on execution mode:

| Mode | Compute-intensive | Memory-intensive | Notes |
|------|------------------:|------------------:|-------|
| **Fast interpreter** | ~54x | ~37x | Fast-interp with gas metering |
| **AOT** | ~1.2x | ~1.9x | Pre-compiled with `wamrc -O3` |

Measured on Apple M-series (aarch64, -O2) with large inputs (4 MB) where per-call setup is amortized. Compute workload: iterative hash compression (64 rounds/block, integer ALU + rotations). Memory workload: histogram + counting sort (3-pass over input, scattered writes).

AOT compilation is automatic during `hull build` when `wamrc` is available. Per-call setup overhead (~50 µs) dominates at small input sizes; at 4 MB+ inputs the ratios above reflect steady-state throughput.

```bash
make wamrc                         # one-time: build AOT compiler (requires LLVM)
make bench-wasm                    # run benchmarks (interpreter + AOT if .aot files present)
hull build myapp                   # auto-AOT compiles compute/*.wasm during build
hull build myapp --no-aot          # skip AOT, interpreter only
hull build myapp --target=x86_64   # cross-compile AOT for different arch
```

### GPU Compute

GPU compute (optional, `HL_ENABLE_GPU=1`) uses wgpu-native for massively parallel workloads via WGSL compute shaders. Features:

- **Dispatch + Pipeline** — single or multi-stage shader execution with shared named buffers
- **Persistent buffers** — keep data GPU-resident across requests (`gpu.buffer()`)
- **Fire-and-forget** — update GPU buffers in-place without readback (`output = false`)
- **GPU-side copy** — copy between persistent buffers without CPU roundtrip (`gpu.buffer_copy()`)
- **Zero-copy disk→GPU** — `fs.mmap()` data passes directly to GPU buffers
- **Shader files** — `gpu.load("name")` reads `shaders/<name>.wgsl` (dev: disk, build: embedded in binary via VFS)
- **GPU timeout** — 5-second deadline prevents shader hangs (`HL_GPU_TIMEOUT_MS`)
- **Unified buffer protocol** — WASM and GPU accept the same input types (string, MappedBuffer, WasmBuffer) for zero-copy data flow between disk, WASM, and GPU

**Performance** (cosine similarity, 128-dim vectors, Apple M1 Max):

| Vectors | Native C | WASM AOT | GPU | GPU vs AOT |
|---------|----------|----------|-----|------------|
| 64 | 7 µs | 7 µs | 2,630 µs | 0.0x |
| 1K | 118 µs | 108 µs | 2,630 µs | 0.0x |
| 16K | 1,830 µs | 2,534 µs | 2,629 µs | 1.0x |
| 64K | 7,270 µs | 10,969 µs | 2,653 µs | **4.1x** |

GPU latency is constant ~2.6ms. Use `gpu.pipeline()` for multi-stage compute (2.4x faster than separate dispatches). Use fire-and-forget for index updates that don't need results returned.

```bash
make fetch-wgpu                                            # download wgpu-native (SHA-256 verified)
make HL_ENABLE_GPU=1                                       # build with GPU
make bench-gpu HL_ENABLE_GPU=1                             # GPU vs WASM vs native benchmark
```

## Compute-only builds (`HL_ENABLE_DB=0`)

For pure compute deployments (REST endpoints wrapping WASM/GPU shaders, transform pipelines, signing services) where state lives elsewhere, drop SQLite entirely:

```bash
make HL_ENABLE_DB=0       # ~3.66 MB binary vs ~5.06 MB default (≈28% smaller)
```

Removed: SQLite + `db.*` + `migrate.*` + `worker_db` + DB-backed stdlib modules (session, ratelimit, idempotency, outbox, inbox, rbac, search) + the `hull migrate` and `hull agent db|migrate` subcommands.

Still works: HTTP routing, middleware, both runtimes, sandbox, `http.fetch`, `ws.*`, `fs.*`, `crypto.*`, `compute.*`, `gpu.*`, templates, static files, image codecs, SSE, timers, validation, CSV, i18n, CORS, ETag, health, JWT, stateless CSRF, form parsing, logger.

Combine with other flags freely, e.g. `make HL_ENABLE_DB=0 HL_ENABLE_TCC=0 HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu` for a GPU-focused compute service.

## Server Tuning

- **Response Compression** — gzip via miniz, automatic for bodies >= 860 bytes when `Accept-Encoding: gzip`. Disable with `--no-compress`.
- **Connection Pooling** — Outbound HTTP reuses TCP+TLS connections (32 pool, 4 per host, 60s idle). Automatic when `hosts` declared in manifest.
- **Redirect Following** — 3xx redirects followed automatically (up to 10 hops). Cross-origin auth headers stripped per RFC 7231.
- **Server Stats** — `server.stats()` (Lua) / `server.stats()` (JS) returns `{active_connections, max_connections, async_suspended, listen_paused}`.

| Flag | Default | Purpose |
|------|---------|---------|
| `--max-connections N` | 256 | Max concurrent connections |
| `--body-max-size SIZE` | 1m | Max request body size |
| `--read-timeout MS` | 30000 | Read timeout (milliseconds) |
| `--workers N` | 4 | Thread pool worker count |
| `--queue-capacity N` | 64 | Thread pool queue capacity |
| `--no-compress` | enabled | Disable gzip response compression |

## Examples

Example apps in both Lua and JavaScript:

| Example | What it demonstrates |
|---------|---------------------|
| [hello](examples/hello/) | Routing, query strings, route params, DB visits |
| [rest_api](examples/rest_api/) | CRUD API with JSON bodies and migrations |
| [auth](examples/auth/) | Session-based authentication with migrations |
| [jwt_api](examples/jwt_api/) | JWT Bearer authentication with refresh tokens |
| [crud_with_auth](examples/crud_with_auth/) | Task CRUD with per-user isolation and migrations |
| [middleware](examples/middleware/) | Request ID, logging, rate limiting, CORS, server stats |
| [webhooks](examples/webhooks/) | Webhook delivery with HMAC-SHA256 signatures |
| [templates](examples/templates/) | Template engine: inheritance, includes, filters |
| [todo](examples/todo/) | Full CRUD todo app with HTML frontend and migrations |
| [bench_db](examples/bench_db/) | SQLite performance benchmarks with migrations |
| [async_http](examples/async_http/) | Non-blocking HTTP requests via event loop |
| [timers](examples/timers/) | Background timers with `app.every()` and self-cancellation |
| [bench_template](examples/bench_template/) | Template engine performance benchmarks |
| [email](examples/email/) | SMTP email sending with templates |
| [cors_manifest](examples/cors_manifest/) | CORS via manifest + server stats API |
| [gpu_search](examples/gpu_search/) | GPU vector similarity search (dispatch + persistent buffers) |
| [gpu_pipeline](examples/gpu_pipeline/) | Multi-stage GPU pipeline (normalize → weight → reduce) |
| [compute_gpu_chain](examples/compute_gpu_chain/) | WASM→GPU zero-copy data flow (unified buffer protocol) |

```bash
# Run an example
./build/hull -p 8080 -d /tmp/test.db examples/hello/app.lua

# Run its tests
./build/hull test examples/hello
```

## Documentation

Full index with start-here guidance by role lives in [`docs/README.md`](docs/README.md).

**Top-level guides:**

| Document | When to read |
|---|---|
| [CLAUDE.md](CLAUDE.md) | Contributor guide — build commands, conventions, full module reference. The canonical project doc. |
| [AGENTS.md](AGENTS.md) | Quick reference for AI agents using `hull agent` CLI and stdlib. |
| [docs/agent_guide.md](docs/agent_guide.md) | **Full SDLC reference** — install → dev → test → build → sign → deploy → release. Every CLI command, every module API, common patterns/anti-patterns. ~1700 lines, deeply hyperlinked. |
| [docs/api/](docs/api/) | **Per-function API reference (Javadoc-style).** [`api/c.md`](docs/api/c.md) for C public headers, [`api/lua.md`](docs/api/lua.md) for Lua stdlib, [`api/js.md`](docs/api/js.md) for JS stdlib. Use when you need to look up a specific signature. |

**Core reference (`docs/`):**

| Document | Content |
|---|---|
| [docs/architecture.md](docs/architecture.md) | System layers, capability API, request flow, build pipeline |
| [docs/security.md](docs/security.md) | Threat model, sandbox phases, signature system, enforcement invariants |
| [docs/stability.md](docs/stability.md) | API stability tiers, semver mapping |
| [docs/known_limitations.md](docs/known_limitations.md) | Compile-time limit constants, override knobs |
| [docs/benchmark.md](docs/benchmark.md) | Performance methodology and measured numbers |
| [docs/release_signing.md](docs/release_signing.md) | Three-layer signature flow + release-key handling |
| [docs/api_review.md](docs/api_review.md) | Pre-v0.1.0 API surface review |
| [docs/roadmap.md](docs/roadmap.md) · [docs/roadmap_next.md](docs/roadmap_next.md) | What's built; what's next |

**Subsystem deep-dives:**

| Document | Content |
|---|---|
| [docs/wamr_architecture.md](docs/wamr_architecture.md) | WASM compute design — WAMR integration, ABI, pooling, segments, streaming, AOT, Memory64 |

**Audits (current state of record):**

| Document | Scope |
|---|---|
| [docs/audit_2026_05_15.md](docs/audit_2026_05_15.md) | Main audit — Phase 5 surface, 49 findings, all closed |
| [docs/audit_2026_05_15_phase6.md](docs/audit_2026_05_15_phase6.md) | Phase 6 (extended `hull agent` + MCP) — 21 findings, all closed |
| [docs/audit_2026_05_15_phase6_reaudit.md](docs/audit_2026_05_15_phase6_reaudit.md) | Re-audit of the Phase 6 fixes — 3 follow-ups, all closed |

**Strategic / positioning (non-developer audience):**

[docs/MANIFESTO.md](docs/MANIFESTO.md) · [docs/ASSESSMENT.md](docs/ASSESSMENT.md) · [docs/INVESTORS.md](docs/INVESTORS.md) · [docs/PERSONAS.md](docs/PERSONAS.md)

**Archive:** historical audits and completed roadmaps (architecture A–L refactor, db-vtable, WASM-improvement, v0-to-v1) live in [`docs/archive/`](docs/archive/) — preserved for reproducibility, not current state.

## Using Hull with AI Agents

Hull provides structured JSON interfaces for AI coding agents via the `hull agent` command. The same interfaces work for any automation — CI scripts, service orchestrators, or human developers who prefer structured output. **Twenty-six machine-readable subcommands** cover routes, database schema, test results, server status, HTTP responses, deployment readiness, capability analysis, request preview, one-shot eval, and more — no screen-scraping or log parsing required.

```
# Core introspection (Phase 1–5)
hull agent routes [app_dir]              # routes + middleware as JSON
hull agent db schema [app_dir] [-d path] # database tables and columns
hull agent db query "SQL" [app_dir]      # read-only SQL → rows as JSON
hull agent request METHOD PATH [opts]    # HTTP request → structured response
hull agent status [app_dir] [-p port]    # check if dev server is running
hull agent errors [app_dir]              # structured errors from last reload
hull agent test [app_dir]                # run tests, per-test pass/fail JSON
hull agent context --task=T [--level=L]  # task-relevant documentation
hull agent migrate [app_dir] [-d path]   # migration status
hull agent deploy [app_dir]              # deployment readiness analysis

# Extended introspection (Phase 6)
hull agent manifest [app_dir]            # effective manifest JSON
hull agent endpoint METHOD PATH [dir]    # request preview (no execution)
hull agent middleware METHOD PATH [dir]  # middleware stack for path
hull agent capabilities [app_dir]        # declared vs used analysis
hull agent validate <file>               # single-file syntax + sandbox check
hull agent vfs [app_dir]                 # list embedded files
hull agent compute [app_dir]             # WASM modules + AOT
hull agent gpu [app_dir]                 # WGSL shaders + GPU availability
hull agent perf [app_dir]                # runtime stats snapshot
hull agent logs [app_dir] [--tail N]     # tail .hull/dev.log
hull agent eval <code> [app_dir]         # one-shot Lua/JS snippet → JSON result
hull agent template <name> [data] [dir]  # render template via runtime
hull agent compute-call <mod> <in> [dir] # invoke WASM module on file input
hull agent schema-diff [app_dir]         # DB schema drift vs migrations
hull agent sql named <qname> [--params J] [dir]  # named query from queries.json
```

All 26 are also exposed via MCP (`hull mcp [app_dir]`) for IDE integrations like Cursor / Claude Code's MCP support.

Combined with `hull dev --agent` (which writes `.hull/dev.json` and `.hull/last_error.json` as sidecar files), agents get a complete feedback loop: edit code, check for errors, run tests, inspect the database, make HTTP requests, validate manifest coverage — all structured.

### Agent Development Workflow

The workflow is the same regardless of which AI coding tool you use:

```
1. hull new myapp && cd myapp        # scaffold project
2. hull dev --agent --audit app.lua  # start dev server (hot-reload + audit logging)
3. Agent edits code                  # dev server auto-reloads
4. hull agent status .               # did the reload succeed?
5. hull agent errors .               # if not, what broke?
6. hull agent test .                 # run tests, check results
7. hull agent request GET /health    # verify endpoint behavior
8. hull agent db schema .            # inspect current schema
9. hull build -o myapp .             # build standalone binary
```

Steps 3–8 repeat in a tight loop. The agent writes code, checks its work, and iterates — all through structured JSON interfaces.

### Claude Code

Claude Code reads [CLAUDE.md](CLAUDE.md) automatically on session start. Hull ships both `CLAUDE.md` (contributor guide with build commands, conventions, and API reference) and `AGENTS.md` (agent-specific guide with `hull agent` usage, app patterns, and stdlib reference). No additional configuration needed.

**Setup:**

```bash
# Install Claude Code
npm install -g @anthropic-ai/claude-code

# Start working on a Hull project
cd myapp
claude
```

Claude Code discovers the project structure through `CLAUDE.md` and uses its built-in Bash tool to run `hull` commands. It reads `AGENTS.md` when it needs agent-specific patterns.

**Example session:**

```
You: Create a task management API with CRUD endpoints and tests

Claude Code:
  1. Reads CLAUDE.md/AGENTS.md for Hull conventions
  2. Creates migrations/001_init.sql (tasks table)
  3. Writes app.lua with GET/POST/PUT/DELETE /tasks routes
  4. Writes tests/test_app.lua
  5. Runs: hull agent test .              → checks all tests pass
  6. Runs: hull agent routes .            → verifies route registration
  7. Runs: hull agent db schema .         → confirms schema matches expectations
```

**Optional: Custom skills** for repeated workflows. Create `.claude/skills/deploy/SKILL.md`:

```yaml
---
name: deploy
description: Build and deploy the Hull app
---
1. Run `hull agent test .` — abort if any tests fail
2. Run `hull build -o app .` to produce a standalone binary
3. Show the user the binary path and size
```

Then invoke with `/deploy` in Claude Code.

### OpenAI Codex CLI

Codex CLI reads `AGENTS.md` automatically (it's the [open standard](https://agents.md/) for agent instructions, supported by 60,000+ projects). Hull ships `AGENTS.md` with complete `hull agent` documentation, app patterns, and stdlib reference.

**Setup:**

```bash
# Install Codex CLI
npm install -g @openai/codex

# Start working on a Hull project
cd myapp
codex
```

Codex reads `AGENTS.md` on session start and runs `hull` commands through its shell tool.

**Optional: Project config** at `.codex/config.toml` for project-specific settings:

```toml
# .codex/config.toml
project_doc_max_bytes = 65536
```

**Optional: Read CLAUDE.md too** — Codex can be configured to read additional instruction files:

```toml
# .codex/config.toml
project_doc_fallback_filenames = ["CLAUDE.md"]
```

### OpenCode

OpenCode reads both `AGENTS.md` (primary) and `CLAUDE.md` (fallback) automatically. Hull ships both, so no configuration is needed.

**Setup:**

```bash
# Install OpenCode
curl -fsSL https://opencode.ai/install | bash

# Start working on a Hull project
cd myapp
opencode
```

**Optional: Custom commands** at `.opencode/commands/test.md`:

```yaml
---
description: Run Hull tests and show results
---
Run `hull agent test .` and analyze the results.
If any tests fail, read the failing test file and the relevant app code,
then fix the issue. Re-run tests until all pass.
```

Then invoke with `/test` in OpenCode.

### What Agents See

Every `hull agent` subcommand returns structured JSON. Here's what a typical agent interaction looks like:

```bash
$ hull agent routes .
```
```json
{
  "runtime": "lua",
  "routes": [
    {"method": "GET", "pattern": "/health"},
    {"method": "GET", "pattern": "/tasks"},
    {"method": "POST", "pattern": "/tasks"},
    {"method": "GET", "pattern": "/tasks/:id"},
    {"method": "DELETE", "pattern": "/tasks/:id"}
  ],
  "middleware": [
    {"method": "*", "pattern": "/api/*", "phase": "pre"}
  ]
}
```

```bash
$ hull agent test .
```
```json
{
  "runtime": "lua",
  "files": [
    {
      "name": "test_app.lua",
      "tests": [
        {"name": "GET /health returns ok", "status": "pass"},
        {"name": "POST /tasks creates task", "status": "pass"},
        {"name": "DELETE /tasks/:id removes task", "status": "fail",
         "error": "expected 204 got 200"}
      ]
    }
  ],
  "total": 3, "passed": 2, "failed": 1
}
```

The agent reads the structured error, opens the relevant handler, fixes the status code, and re-runs the test — all without human intervention.

```bash
$ hull agent db schema .
```
```json
{
  "tables": [
    {
      "name": "tasks",
      "columns": [
        {"name": "id", "type": "INTEGER", "pk": true},
        {"name": "title", "type": "TEXT", "notnull": true},
        {"name": "done", "type": "INTEGER", "default": "0"},
        {"name": "created_at", "type": "INTEGER"}
      ]
    }
  ]
}
```

```bash
$ hull agent request POST /tasks -d '{"title":"Buy milk"}' -H 'Content-Type: application/json'
```
```json
{
  "status": 201,
  "elapsed_ms": 3,
  "headers": {"Content-Type": "application/json", "Content-Length": "42"},
  "body": "{\"id\":1,\"title\":\"Buy milk\",\"done\":0}"
}
```

### Deployment

```bash
# Build standalone binary (embeds app + stdlib + SQLite + HTTP server)
hull build -o myapp .

# The binary is the product — no runtime, no dependencies
./myapp -p 8080 -d /data/app.db

# Cross-platform (Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD)
hull build -o myapp . CC=cosmocc
```

The agent workflow for deployment: run `hull agent test .` to verify all tests pass, then `hull build -o myapp .` to produce the binary. The output is a single file (~5 MB on aarch64 with the full default build; ~3.66 MB compute-only with `HL_ENABLE_DB=0`). Copy it anywhere and run it.

#### `hull deploy` — Config Generator

`hull deploy` generates deployment configs informed by the app's manifest. It writes files that standard tools consume — it never makes network calls or executes the generated configs.

```bash
hull deploy dockerfile myapp/              # Dockerfile + .dockerignore
hull deploy systemd myapp/ --name myapp    # deploy/myapp.service + deploy/install.sh
hull deploy fly myapp/ --region lax        # fly.toml (+ Dockerfile if missing)
```

| Target | Output | Consumed by |
|--------|--------|-------------|
| `dockerfile` | `Dockerfile` + `.dockerignore` | `docker build` |
| `systemd` | `deploy/&lt;name&gt;.service` + `deploy/install.sh` | `systemctl` |
| `fly` | `fly.toml` + `Dockerfile` (if missing) | `flyctl deploy` |

**Manifest-aware generation:** The generated configs automatically adapt based on the app's manifest and directory structure:
- **CA bundle** — included only when manifest declares `hosts` (outbound HTTPS)
- **ENV declarations** — from manifest `env` array (documentation + `docker run -e`)
- **VOLUME** — only when app has migrations (uses a database)
- **systemd hardening** — 17 security directives (NoNewPrivileges, ProtectSystem=strict, SystemCallFilter, etc.)
- **fly.toml mounts** — persistent volume only for database apps

```bash
# Common flags
hull deploy <target> [app_dir] --port 8080 --name myapp -o /tmp/out
hull deploy dockerfile myapp/ --distroless     # FROM distroless instead of scratch
hull deploy dockerfile myapp/ --sign           # add hull verify step in build stage
hull deploy systemd myapp/ --user webapp       # custom system user
hull deploy fly myapp/ --memory 512            # 512 MB VM

# Agent introspection
hull agent deploy myapp/                       # JSON deployment readiness analysis
```

## Building Hull

```bash
make                    # build hull binary
make test               # run 344 unit tests
make e2e                # end-to-end tests (all examples, both runtimes)
make e2e-migrate        # migration system tests
make e2e-templates      # template engine tests (40 tests, both runtimes)
make debug              # ASan + UBSan build
make msan               # MSan + UBSan (Linux clang only)
make check              # full validation (clean + ASan + test + e2e)
make analyze            # Clang static analyzer
make cppcheck           # cppcheck static analysis
make platform           # build libhull_platform.a
make platform-cosmo     # build multi-arch cosmo platform archives
make self-build         # reproducible build verification (hull→hull2→hull3)
make CC=cosmocc         # build with Cosmopolitan (APE binary)
make clean              # remove all build artifacts
```

## Status

Hull is approaching v0.1.0. The complete distribution lifecycle is in place:

| Phase | What | Status |
|-------|------|--------|
| D1 | Version + GitHub Actions release pipeline | Done |
| D2 | `install.sh` + `hull init` + `hull doctor` + shell completions | Done |
| D3 | Embedded TinyCC — zero-dependency `hull build` | Done |
| D4 | Embedded Mozilla CA bundle — zero-dependency HTTPS | Done |
| D5 | `hull update` — verified self-update from GitHub releases | Done |
| D6 | `hull.com` landing + docs site | External |

All core features are implemented and tested across Linux (gcc + clang), macOS, and Cosmopolitan APE; capability sandbox enforced via pledge/unveil (Linux/Cosmo/OpenBSD) or Seatbelt (macOS). See [docs/roadmap.md](docs/roadmap.md) for what's next after v0.1.0 (PostgreSQL backend, module ecosystem, HTTP/2, agent platform Phase 5).

## License

AGPL-3.0. See [LICENSE](LICENSE).

Commercial licenses available for closed-source distribution. See the [Licensing](docs/MANIFESTO.md#licensing) section of the manifesto.
