# Hull — Agent & Developer Guide

**Audience:** AI coding agents and developers writing, debugging, building, or
deploying Hull applications.

**Source of truth:** This document is a curated, hyperlinked synthesis of
[`CLAUDE.md`](../CLAUDE.md), the headers under `include/hull/`, and the example
apps under `examples/`. Where a behaviour is non-obvious or has an enforcement
boundary, the relevant `file:line` is cited so you can verify against source.

**Status:** Reflects commit `0e121d9` (2026-05-15). API surface stable; v0.1.0
release is operational-keys-pending.

---

## Table of contents

1. [Mental model](#1-mental-model)
2. [Installation & first run](#2-installation--first-run)
3. [Project lifecycle](#3-project-lifecycle)
4. [Development loop](#4-development-loop)
5. [CLI reference (22 commands)](#5-cli-reference-22-commands)
6. [Architecture](#6-architecture)
7. [Manifest & capabilities](#7-manifest--capabilities)
8. [Module API surface](#8-module-api-surface)
9. [Database & migrations](#9-database--migrations)
10. [Compute (WASM)](#10-compute-wasm)
11. [GPU (WGSL)](#11-gpu-wgsl)
12. [Security model](#12-security-model)
13. [Testing](#13-testing)
14. [Build](#14-build)
15. [Deployment](#15-deployment)
16. [Signing & release](#16-signing--release)
17. [Agent workflow](#17-agent-workflow)
18. [Performance reference](#18-performance-reference)
19. [Common patterns & anti-patterns](#19-common-patterns--anti-patterns)
20. [Glossary & file map](#20-glossary--file-map)

---

## 1. Mental model

Hull is **one binary** (`hull`) that does five jobs:

| Job | Subcommand | What runs |
|---|---|---|
| Dev server | `hull <app>` / `hull dev` | Loads `app.lua` or `app.js`; sandboxed; hot-reloads on file change |
| Test runner | `hull test` | In-process HTTP dispatch against the loaded app; no real TCP |
| Build tool | `hull build` | Compiles the app + Hull's `libhull_platform.a` to a standalone binary |
| Release tool | `hull update`, `sign-release`, `verify-release` | Self-update over HTTPS with Ed25519 signature |
| Agent tool | `hull agent <sub>` | Machine-readable introspection (JSON), see [§17](#17-agent-workflow) |

The architecture has **two planes**:

- **Orchestration plane** — Lua 5.4 or QuickJS (your choice, file-extension
  selects). Sandboxed. Handles routing, middleware, DB queries, template
  rendering. Capability access mediated by a C boundary.
- **Compute plane** — WASM (WAMR) or GPU (wgpu-native). Pure functions, no
  I/O. Gas/timeout-metered. For CPU-heavy work, dispatched from the
  orchestration layer.

The C boundary (`hl_cap_*`) is what enforces the [manifest](#7-manifest--capabilities)
and what makes the build reproducible. The sandbox layer (pledge/unveil/seatbelt)
is the kernel's enforcement of the same rules.

**One-line summary:** *Hull is a capability-secure web runtime in C with two
interchangeable scripting fronts (Lua/JS), optional compute backends (WASM/GPU),
and a build tool that turns your app into a signed single-file binary.*

---

## 2. Installation & first run

### End-user install

```bash
curl -fsSL https://raw.githubusercontent.com/artalis-io/hull/main/install.sh | sh
```

`install.sh` detects OS/arch via `uname`, picks `hull-linux-x86_64`,
`hull-darwin-arm64`, or `hull-cosmo` (universal APE) from the latest GitHub
release, verifies the SHA-256 from `hull.sha256`, and installs to
`~/.local/bin/hull` (or `/usr/local/bin` if root). Knobs (env vars):

| Variable | Effect |
|---|---|
| `HULL_VERSION=v0.1.0` | Pin to a specific tag (default: latest) |
| `HULL_PREFIX=~/bin` | Install location |
| `HULL_FLAVOR=cosmo` | Force the universal binary |
| `HULL_FORCE=1` | Overwrite existing install |
| `HULL_DRY_RUN=1` | Print what would happen, don't write |

Shell completions live in [`completions/`](../completions/) (bash, zsh, fish).
Source the matching file from your rc.

### Source build (developer)

```bash
git clone --recurse-submodules https://github.com/artalis-io/hull.git
cd hull
make                # build/hull (5 MB on arm64-darwin, 3.7 MB with HL_ENABLE_DB=0)
make test           # 27 unit suites
make check          # clean + ASan + test + e2e (full validation)
```

Build flags (see [§14](#14-build) for full list): `HL_ENABLE_DB`, `HL_ENABLE_LUA`,
`HL_ENABLE_JS`, `HL_ENABLE_WASM`, `HL_ENABLE_GPU`, `HL_ENABLE_TCC`,
`HL_EMBED_CA_BUNDLE`.

### Verify install

```bash
hull version           # prints commit + runtime + arch
hull doctor            # environment check (compilers, platform lib, CA bundle)
hull doctor --json     # same, machine-readable
```

`hull doctor` exits 0 only when `hull build` is fully ready (platform library
embedded AND at least one compiler available). For dev-server-only use,
non-zero is fine.

---

## 3. Project lifecycle

### Create or initialise a project

| Command | Use |
|---|---|
| `hull new <dir>` | New project from template (full scaffold) |
| `hull init [dir]` | In-place init (like `git init`; doesn't overwrite) |
| `hull init --runtime js` | Force JS runtime |

`hull init` is idempotent: it creates `app.lua`/`app.js`, `tests/`, `migrations/`,
`.gitignore` if missing, and leaves existing files alone. Runtime is
auto-detected from `app.lua`/`app.js` presence, otherwise defaults to Lua.

### Project layout

```
myapp/
├── app.lua             ← entry point (or app.js)
├── tests/
│   └── test_*.lua      ← test files (see §13)
├── migrations/         ← SQL migrations (see §9)
│   ├── 001_init.sql
│   └── 002_add_user.sql
├── templates/          ← HTML templates (see §8 / template module)
│   ├── base.html
│   └── pages/
├── static/             ← Static assets served at /static/*
├── shaders/            ← WGSL shaders for gpu.load() (see §11)
│   └── score.wgsl
├── compute/            ← WASM modules for compute.call() (see §10)
│   └── score.wasm
├── hull.json           ← optional: explicit manifest (else extracted from code)
└── .hull/              ← agent sidecar (created by `hull dev --agent`)
    ├── dev.json        ← port, PID, runtime
    └── last_error.json ← structured error from last failed reload
```

### Entry point shape

Everything goes through `app.*` (routing) and capability globals (`db`, `http`,
`fs`, `crypto`, `time`, `env`, `log`, `ws`, `compute`, `gpu`, `image`, `json`,
`hull`):

```lua
-- app.lua — minimal pattern
app.manifest({})                              -- declare capabilities (see §7)

app.get("/", function(req, res)
    res:json({ hello = "world" })
end)
```

```javascript
// app.js — same in JS
app.manifest({});

app.get("/", (req, res) => {
    res.json({ hello: "world" });
});
```

The runtime is chosen by file extension: `app.lua` → Lua, `app.js` → JS. They
cannot coexist; pick one per project.

---

## 4. Development loop

### Run

```bash
hull dev                 # equivalent to: hull app.lua -p 3000
hull dev -p 8080         # custom port
hull dev --agent         # writes .hull/dev.json + .hull/last_error.json
hull dev -l debug        # log level: trace|debug|info|warn|error|fatal
```

`hull dev` enables:
- Hot reload on file change (via fs watcher)
- Auto-run pending migrations on start
- `Cache-Control: no-cache` on static files (so the browser re-fetches)
- Sandbox active (use `--no-sandbox` to debug capability violations)

### What an agent should watch

When run with `--agent`:

| File | Contents | Refresh |
|---|---|---|
| `.hull/dev.json` | `{ "port": 3000, "pid": 12345, "runtime": "lua" }` | On start |
| `.hull/last_error.json` | `{ "file": "...", "line": 12, "message": "..." }` | On load failure |

These are JSON, atomic-write. Use `hull agent status` and `hull agent errors`
to read them rather than parsing raw.

### Logging

Hull uses **logfmt** when the `logger.middleware()` is installed. Without it,
falls back to `time level [tag] message`. To get structured logs in agent
workflows, install the logger middleware first and tail the stderr:

```lua
app.use("*", "/*", require("hull.middleware.logger").middleware({
    skip = { "/health" },
}))
```

Audit logging (capability calls) is opt-in: `--audit` flag or `HULL_AUDIT=1`
env var. One JSON line per cap call to stderr. Zero overhead when off (single
branch on `hl_audit_enabled`).

### Common dev annoyances

- **"app context init failed"**: usually a syntax error in `app.lua`. Run
  `hull agent errors` for the structured location.
- **"manifest violation"**: app called a capability it didn't declare. Add it
  to `app.manifest({ ... })`. See [§7](#7-manifest--capabilities).
- **"out of memory" on first request**: the default 64 MB runtime heap is
  per-request, not per-session. Most apps never hit this. If you do, bump with
  `-m 128m`.
- **Static files not serving**: `static/` must exist at startup. After
  creating the directory, restart `hull dev`.

---

## 5. CLI reference (22 commands)

Grouped by purpose. Run `hull <cmd> --help` for full per-command flags.

### Project commands

| Command | Purpose |
|---|---|
| `hull new <dir>` | Scaffold new project from template |
| `hull init [dir] [--runtime lua\|js]` | In-place init (idempotent, like `git init`) |
| `hull eject <dir>` | Eject the built-in stdlib into the project (for forking) |

### Dev / inspect commands

| Command | Purpose |
|---|---|
| `hull <app>` / `hull dev` | Run dev server |
| `hull test` | Run `tests/*.lua` (or `.js`) via in-process dispatch |
| `hull inspect <app>` | Show app's declared manifest + routes |
| `hull manifest <app>` | Emit canonical manifest JSON |
| `hull check <app>` | Static analysis: syntax + sandbox violations |
| `hull doctor [--json]` | Environment readiness check |
| `hull version` / `--version` / `-v` | Version + commit + arch |

### Build / deploy commands

| Command | Purpose |
|---|---|
| `hull build <app> [--compiler=tcc\|system\|...]` | Compile to standalone binary |
| `hull deploy <app> --target=dockerfile\|systemd\|fly` | Generate deployment config |
| `hull migrate [app]` | Run pending migrations |
| `hull migrate status [app]` | Show applied / pending |
| `hull migrate new <name>` | Scaffold new `NNN_<name>.sql` |
| `hull compute new <name> [--lang c]` | Scaffold a new WASM compute module under `compute/<name>/` |
| `hull compute build [name]` | Compile `compute/<name>/<name>.c` → `compute/<name>.wasm` (auto-runs as part of `hull build` for stale sources) |
| `hull compute test <name>` | Run JSON fixtures from `compute/<name>/test_fixtures.json` |
| `hull compute check <name>` | Validate that `compute/<name>.wasm` loads in WAMR |

### Agent commands

| Command | Purpose |
|---|---|
| `hull agent routes [app]` | List routes + middleware (JSON) |
| `hull agent db schema [app] [-d path]` | DB schema introspection (JSON) |
| `hull agent db query "SQL" [app]` | Run read-only query (JSON) |
| `hull agent request METHOD PATH [opts]` | HTTP request to dev server |
| `hull agent status [app] [-p port]` | Dev server status |
| `hull agent errors [app]` | Structured errors from last reload |
| `hull agent test [app]` | Run tests with JSON output |
| `hull agent context --task=T --level=L` | Task-relevant docs |
| `hull agent migrate [app] [-d path]` | Migration status |
| `hull agent deploy [app]` | Deployment readiness analysis |

### Release / signing commands

| Command | Purpose | Used by |
|---|---|---|
| `hull keygen` | Generate Ed25519 keypair (signing app or platform) | Developer (one-time) |
| `hull sign-platform <input> --key=PATH` | Sign `libhull_platform.a` | Hull release pipeline |
| `hull sign-release <manifest> --key=PATH` | Sign release manifest | GitHub Actions |
| `hull verify-release <manifest> <sig>` | Verify release signature | End-users / auditors |
| `hull update [--check] [--channel=stable\|beta]` | Self-update from GitHub | End-users |
| `hull verify <app>` | Verify app signature (built binary) | End-users |

### MCP

| Command | Purpose |
|---|---|
| `hull mcp [app]` | MCP server (stdio JSON-RPC) for IDE integrations |

### Runtime flags (apply to dev / built binaries)

```
-p PORT              Listen port (default 3000)
-b ADDR              Bind address (default 127.0.0.1)
-d FILE              SQLite DB file (default data.db)
-m SIZE              Runtime heap limit (default 64m)
-l LEVEL             Log level
--tls-cert / --tls-key   TLS certs
--no-migrate         Skip auto-run migrations
--no-sandbox         Disable kernel sandbox (debug)
--audit              Enable capability audit JSON logging
--max-instructions N Override instruction limit (default 100m)
--max-connections N  Max concurrent connections (default 256)
--workers N          Thread pool worker count (default 4)
--no-ca-bundle / --ca-bundle PATH    TLS trust store override
```

---

## 6. Architecture

### System layers

```
┌─────────────────────────────────────────────────────────┐
│ Application Code (Lua / JS)                             │
│   ↓ uses                                                │
│ Standard Library (stdlib/lua, stdlib/js)                │
│   ↓ uses globals: app, db, http, fs, crypto, ...        │
├─────────────────────────────────────────────────────────┤
│ Runtimes (Lua 5.4 / QuickJS)                            │
│   sandboxed interpreters, instruction-metered           │
├─────────────────────────────────────────────────────────┤
│ Capability Layer (src/hull/cap/*.c, hl_cap_* functions) │
│   the C enforcement boundary                            │
├─────────────────────────────────────────────────────────┤
│ Hull Core (main.c, manifest.c, sandbox.c, vfs.c, ...)   │
├─────────────────────────────────────────────────────────┤
│ Keel HTTP server (vendor/keel/)                         │
│   event loop, routing, async, thread pool               │
├─────────────────────────────────────────────────────────┤
│ Kernel sandbox (pledge / unveil / seatbelt)             │
└─────────────────────────────────────────────────────────┘
```

Each layer talks **only to the one below it**. Application code cannot reach
past the runtime sandbox; the runtime cannot reach past the cap layer; the cap
layer cannot bypass the kernel sandbox.

### Orchestration vs compute

Hull separates control-plane orchestration from data-plane computation:

- **Orchestration** (Lua/JS, gas-metered, cap-mediated): routing, DB queries,
  template rendering, HTTP fetch, file I/O, crypto. Full mediated capability
  access.
- **Compute** (WASM/GPU, timeout-metered, no I/O): pure functions over byte
  buffers. Score, transform, dedupe, sign — anything CPU-intensive.

The orchestration layer **dispatches** to compute (`compute.call`,
`gpu.dispatch`). Compute never calls out to I/O.

### Dual-runtime polymorphism

Both Lua 5.4 and QuickJS implement `HlRuntimeVtable`
(`include/hull/runtime.h:49`) so `main.c` and the cap layer don't care which
is active. File extension picks the runtime.

| Aspect | Lua 5.4 | QuickJS |
|---|---|---|
| Naming | `snake_case` | `camelCase` |
| Globals access | `db.query(...)` | `db.query(...)` (after `import`) |
| Module system | `require("hull.foo")` | `import { foo } from "hull:foo"` |
| Error handling | `pcall`, `error()` | `try/catch`, `throw` |
| Async | coroutines via `hull.sleep`, `http.async.*` | `async/await` |

### Request flow

```
Client
  ↓ HTTP / HTTPS / WS / SSE
Keel server (vendor/keel/)
  ↓ route match
hl_{lua,js}_dispatch()           ← src/hull/runtime/{lua,js}/dispatch.c
  ↓ runs middleware then handler
Handler (Lua/JS code)
  ↓ db.query / http.fetch / fs.read / ...
hl_cap_*()                       ← src/hull/cap/*.c (C boundary)
  ↓ allowlist check + actual syscall
SQLite / FS / network / crypto
```

### Capability layer — symbol map

| Module | Source | Key functions |
|---|---|---|
| Database | `cap/db.c`, `cap/db_sqlite.c` | `hl_cap_db_query/exec/begin/commit/rollback` |
| Filesystem | `cap/fs.c` | `hl_cap_fs_read/write/exists/delete/validate` |
| Crypto | `cap/crypto.c` | SHA-256/512, HMAC, PBKDF2, Ed25519, NaCl secretbox/box, random |
| HTTP client | `cap/http.c`, `cap/http_async.c` | `hl_cap_http_request` (host allowlist) |
| Environment | `cap/env.c` | `hl_cap_env_get` (allowlist) |
| Time | `cap/time.c` | `now/now_ms/clock/date/datetime` |
| Tool (build mode) | `cap/tool.c` | `hl_tool_spawn/find_files/copy/mkdir` |
| Test (in-process HTTP) | `cap/test.c` | dispatch + assert |
| Body reader | `cap/body.c` | bounded body slurp |
| WASM compute | `cap/wasm.c`, `cap/wasm_*.c` | `hl_cap_wasm_init/load/call` |
| GPU compute | `cap/gpu.c`, `cap/gpu_wgpu.c` | `hl_cap_gpu_init/compile/dispatch` |
| Audit logging | `cap/audit.c` | streaming JSON to stderr |
| WebSocket | `cap/ws.c` | frame parse, connection registry |
| Image | `cap/image.c`, `cap/image_stb.c` | decode/encode/raw pixels |
| SMTP | `cap/smtp.c` | client send + auth + TLS |

### Virtual filesystem (VFS)

All embedded-file lookups go through `HlVfs` (`include/hull/vfs.h`).
Two instances created at startup:

| Instance | Entries | root_dir | Used for |
|---|---|---|---|
| `app_vfs` | `hl_app_entries[]` | `app_dir` | templates, static, migrations, app modules |
| `platform_vfs` | `hl_stdlib_entries[]` | NULL | Hull stdlib modules |

API: `hl_vfs_find` (O(log n)), `hl_vfs_prefix` (prefix query), `hl_vfs_has_prefix`.

At build time, `app_registry.c` is regenerated with all app files sorted in
C strcmp order (Makefile uses `LC_ALL=C sort`). At runtime, file lookups are
binary searches against the embedded array; in dev mode they fall through to
disk.

---

## 7. Manifest & capabilities

The manifest is the **declared** set of capabilities your app uses. It's the
contract that the sandbox enforces. Anything not declared is denied.

### Declaring the manifest

```lua
app.manifest({
    fs = {
        read  = { "config.json", "data/*.csv" },     -- glob-ish, no `..`
        write = { "uploads/" },
    },
    hosts = { "api.example.com", "*.internal" },     -- HTTP host allowlist
    env   = { "API_KEY", "STRIPE_SECRET" },          -- env var allowlist
    gpu   = true,                                    -- enable GPU access
})
```

```javascript
app.manifest({
    fs: {
        read:  ["config.json", "data/*.csv"],
        write: ["uploads/"],
    },
    hosts: ["api.example.com", "*.internal"],
    env:   ["API_KEY", "STRIPE_SECRET"],
    gpu:   true,
});
```

### Sandbox phases

Two-phase enforcement in `sandbox.c`:

**Phase 1** — `hl_sandbox_apply_pledge()` (called before `load_app()`):
- **Linux/Cosmo**: `pledge("stdio inet rpath wpath cpath flock dns unveil")`
  — blocks `exec`, `proc`, `fork` during module loading.
- **macOS**: no-op (Seatbelt's `sandbox_init` is irreversible; full profile
  applied in phase 2).

**Phase 2** — `hl_sandbox_apply()` (after manifest extraction):
- **Linux/Cosmo**: unveils declared paths, seals filesystem, applies pledge syscall filter.
- **macOS**: builds dynamic SBPL profile from manifest, applies via
  `sandbox_init_with_parameters`. Deny-default with selective allows.

Violation = `SIGABRT` on OpenBSD, `SIGKILL` on Linux/Cosmo, `EPERM` on macOS.
`--no-sandbox` disables for debugging.

### Capability enforcement invariants

These are the guarantees Hull provides. If you find any of them violated,
that's a bug:

1. **SQL injection impossible** — all DB access uses `sqlite3_bind_*`
   parameterised binding. SQL is always a literal string. Identifier
   interpolation (e.g. `search.lua` for FTS5 table names) goes through a
   strict allowlist regex + denylist check.
2. **Internal tables protected** — `hl_cap_db_check_namespace()`
   (`cap/db.c`) blocks user code from accessing `_hull_*` tables. Enforcement
   uses call-stack inspection: Lua checks `ar.source` for `hull.` prefix; JS
   checks module name for `hull:` prefix. Stdlib bypasses transparently;
   user code is rejected.
3. **Path traversal blocked** — `hl_cap_fs_validate()` rejects absolute paths,
   `..` components, and symlink escapes via `realpath()` ancestor check.
   Plus kernel `unveil` on Linux/Cosmo.
4. **Host allowlist enforced** — `hl_cap_http_request()` validates target
   host against manifest's `hosts` array.
5. **Env allowlist enforced** — `hl_cap_env_get()` checks against `env`
   allowlist (max 32 entries).
6. **No shell invocation in user code** — `tool.spawn()` is build-mode only
   (CLI tools), uses an explicit compiler allowlist, never `system()`/`popen()`.
7. **Key material zeroed** — `hull_secure_zero()` (volatile memset) scrubs
   crypto material from stack buffers before return.
8. **Instruction limits** — Lua via `lua_sethook(LUA_MASKCOUNT)`, JS via
   `JS_SetInterruptHandler`. Default 100M/req. Override with
   `--max-instructions N` or `HULL_MAX_INSTRUCTIONS=N`.
9. **Audit logging** — `--audit` flag emits one JSON line per capability call.
   Zero overhead when off (single branch on `hl_audit_enabled` global).

---

## 8. Module API surface

### Conventions

- **Lua**: globals (`app`, `db`, …) plus `require("hull.foo")` for stdlib.
  Methods on objects use `:` syntax (`res:json(...)`); module functions use
  `.` (`json.encode(...)`).
- **JS**: globals (`app`, `db`, …) plus `import { foo } from "hull:foo"` for
  stdlib. Always `camelCase`. Methods are regular function properties
  (`res.json(...)`).
- **API parity**: JS and Lua expose the same surface with different naming
  conventions. Options that mean the same thing accept both styles where
  they overlap (e.g. `maxRows` and `max_rows`).

### Routing & handlers (`app`)

```lua
-- HTTP methods
app.get(pattern, handler)            -- pattern: "/users/:id", "/api/*"
app.post(pattern, handler)
app.put / app.delete / app.patch
app.head / app.options

-- Middleware
app.use(method, pattern, mw)         -- pre-body, runs before body is read
app.use_post(method, pattern, mw)    -- post-body, runs after body is read

-- WebSocket (see §8.7)
app.ws(path, { on_open, on_message, on_close })

-- Server-Sent Events (see §8.7)
app.sse(path, function(req, stream) ... end)

-- Manifest (see §7)
app.manifest(table)

-- Background timers (see §8.8)
app.every(ms, fn)                    -- repeating; return false to cancel
app.daily("HH:MM", fn, { localtime = true })
```

JS uses identical names: `app.use`, `app.usePost`, `app.ws`, `app.sse`,
`app.every`, `app.daily`, `app.manifest`.

### Request/response objects

**Lua `req` (table) fields:**
- `req.method`, `req.path`, `req.url`, `req.query` (table)
- `req.headers[name]` — keys are lowercased
- `req.body` — string (lazy; reading triggers body capture)
- `req.params[name]` — captured route parameters
- `req.ctx` — per-request mutable table for middleware-to-handler data

**Lua `res` (userdata) methods:**
- `res:status(code)` — chainable
- `res:header(name, value)` — chainable
- `res:json(value)` — sends, `Content-Type: application/json`
- `res:text(string)`, `res:html(string)` — same, with content type
- `res:redirect(url, [code])` — default 302
- `res:cookie(name, value, opts)` — sets `Set-Cookie`
- `res:file(path)` — zero-copy sendfile (path is cap-validated)

**JS** is the same with `camelCase`: `req.headers[name]`, `res.status(code)`,
`res.json(value)`, `res.sendFile(path)`.

### Built-in modules (Lua: `require("hull.X")`; JS: `import { X } from "hull:X"`)

| Module | Lua | JS | What it does |
|---|---|---|---|
| **json** | `hull.json` | (built-in `json`) | Encode/decode |
| **cookie** | `hull.cookie` | `hull:cookie` | Parse / serialize / clear |
| **jwt** | `hull.jwt` | `hull:jwt` | Sign / verify HS256 |
| **template** | `hull.template` | `hull:template` | HTML templates (auto-escape) |
| **validate** | `hull.validate` | `hull:validate` | Schema validation |
| **form** | `hull.form` | `hull:form` | URL-encoded form parse |
| **i18n** | `hull.i18n` | `hull:i18n` | Locale + translations |
| **csv** | `hull.csv` | `hull:csv` | Parse / encode RFC 4180 |
| **search** | `hull.search` | `hull:search` | FTS5 full-text search |
| **image** | `hull.image` | `hull:image` | Decode / encode / pixel ops |
| **email** | `hull.email` | `hull:email` | SMTP + Postmark/SendGrid/Resend |
| **db.udf** | `db.udf.register` | `db.udf.register` | User-defined SQL functions |
| **ws** | `ws` global | `hull:ws` | WebSocket server + client |

### Middleware modules (`hull.middleware.X` / `hull:middleware:X`)

All middleware follow the factory pattern:

```lua
local mod = require("hull.middleware.X")
local mw = mod.middleware(opts)         -- returns function(req, res) -> 0|1
app.use("*", "/*", mw)                  -- 0 = continue, 1 = short-circuit
```

| Middleware | Purpose | Notable options |
|---|---|---|
| `cors` | CORS + preflight | `origins`, `credentials`, `methods`, `headers`, `max_age` |
| `ratelimit` | In-memory rate limit | `limit`, `window`, `key` (string or fn) |
| `csrf` | Stateless CSRF | `secret`, `session_key`, `header_name`, `field_name` |
| `auth` | Session or JWT auth | `cookie_name`, `optional`, `login_path`, `secret` |
| `session` | Server-side sessions | `ttl` (call `session.init()` first) |
| `logger` | Logfmt access logs | `skip`, `include_headers` (or `includeHeaders` in JS) |
| `transaction` | Wrap mutations in `db.batch` | (none) — call `transaction.run(fn)` inside handlers |
| `idempotency` | Idempotency-Key replay | `header_name`, `get_principal`, `methods`, `ttl` |
| `outbox` | Transactional outbox | `max_attempts` (default 5) |
| `inbox` | Inbox dedup | `ttl` |
| `rbac` | Role-based access | `rbac.define_role`, `rbac.require_role(role)` |
| `health` | `/health` + `/ready` | `path_health`, `path_ready`, `db_check` |
| `etag` | ETag helpers (not middleware) | `etag.json(req, res, data, status)` |

#### Recommended middleware stack

```lua
local cors        = require("hull.middleware.cors")
local ratelimit   = require("hull.middleware.ratelimit")
local auth        = require("hull.middleware.auth")
local csrf        = require("hull.middleware.csrf")
local session     = require("hull.middleware.session")
local logger      = require("hull.middleware.logger")
local health      = require("hull.middleware.health")
local transaction = require("hull.middleware.transaction")
local idempotency = require("hull.middleware.idempotency")

session.init()                   -- create hull_sessions table
idempotency.init()               -- create _hull_idempotency_keys

-- Order matters: cheaper rejections first, body-dependent last.
app.use("GET",  "/*",     health.middleware())                              -- /health, /ready
app.use("*",    "/*",     logger.middleware({ skip = {"/health"} }))
app.use("*",    "/api/*", ratelimit.middleware({ limit = 60, window = 60 }))
app.use("*",    "/api/*", cors.middleware({ origins = {"https://app.com"} }))
app.use("*",    "/api/*", auth.session_middleware({}))
app.use_post("*",    "/*", csrf.middleware({ secret = "change-me" }))
app.use_post("POST", "/api/*", transaction.middleware())
app.use_post("POST", "/api/*", idempotency.middleware())
```

**Order rules:**
- Rate limit **before** auth (reject early, save work).
- CORS **before** auth (preflight must not require credentials).
- CSRF is **only** for cookie/session apps. JWT Bearer apps don't need it
  (tokens aren't sent automatically by browsers).

### Capability globals

| Global | Purpose | Notable functions |
|---|---|---|
| `db` | Database (requires `HL_ENABLE_DB=1`) | `query`, `exec`, `batch`, `udf.register`, `async.query`, `async.exec` |
| `http` | HTTP client | `fetch`, `get`, `post`, `async.fetch`, `async.get`, `async.post` |
| `fs` | Filesystem (manifest allowlist) | `read`, `write`, `exists`, `delete`, `mmap`, `list_dir` |
| `crypto` | Crypto primitives | `sha256`, `sha512`, `hmac_sha256`, `pbkdf2`, `ed25519_*`, `secretbox_*`, `random`, `hash_password`, `verify_password`, `base64url_*` |
| `env` | Env vars (manifest allowlist) | `get(name)` → string or nil |
| `time` | Time | `now`, `now_ms`, `clock`, `date`, `datetime` |
| `log` | Logging | `trace/debug/info/warn/error/fatal` |
| `ws` | WebSocket | `broadcast`, `connections`, `connect` |
| `compute` | WASM compute | `available`, `call`, `async.call`, `load`, `buffer`, `instance`, `segment`, `stream` |
| `gpu` | GPU compute | `available`, `devices`, `compile`, `load`, `dispatch`, `pipeline`, `async.*`, `buffer`, `buffer_read`, `buffer_copy`, `texture`, `texture_read` |
| `image` | Image | `new`, `from_buffer`, `decode`, `encode` |
| `hull` | Runtime helpers | `sleep(ms)`, `gather(fn1, fn2, ...)` |

### WebSocket endpoints

**Server-side:**

```lua
app.ws("/ws/chat", {
    on_open = function(conn)
        log.info("connected: " .. conn:id())
    end,
    on_message = function(conn, msg, is_binary)
        ws.broadcast("/ws/chat", msg)
    end,
    on_close = function(conn, code, reason) end,
})
```

**Client-side (outbound):**

```lua
local client = ws.connect("ws://other:8080/feed", {
    on_open = function(c) c:send("hello") end,
    on_message = function(c, msg) log.info(msg) end,
    on_close = function(c, code, reason) end,
    on_error = function(c, err) log.error(err) end,
})
-- Host must be in manifest `hosts` allowlist.
```

### SSE endpoints

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

### Background timers

```lua
app.every(5000, function()                          -- every 5s
    require("hull.middleware.session").cleanup()
end)

app.daily("02:00", function()                       -- daily at UTC 02:00
    require("hull.middleware.outbox").cleanup(86400 * 30)
end)

-- Return false to self-cancel:
app.every(1000, function()
    if some_condition then return false end
end)
```

Constraints: minimum 100 ms interval, one invocation at a time per timer
(deferred if previous is still running), errors logged but don't stop the
timer.

---

## 9. Database & migrations

### When to use, when not to

`HL_ENABLE_DB=1` (default) embeds SQLite. `HL_ENABLE_DB=0` produces a
compute-only build (~1.4 MB smaller). See [§14](#14-build) and
[`docs/audit_2026_05_15.md`](audit_2026_05_15.md) for the full list of
modules that require DB.

### Querying

```lua
-- SELECT — returns array of rows (each row a table)
local rows = db.query("SELECT id, name FROM users WHERE active = ?", { true })
for _, r in ipairs(rows) do print(r.id, r.name) end

-- INSERT/UPDATE/DELETE — returns affected row count
local n = db.exec("UPDATE users SET name = ? WHERE id = ?", { "Bob", 42 })

-- Last inserted ID
local id = db.last_id()

-- Transaction: BEGIN IMMEDIATE..COMMIT, ROLLBACK on error
db.batch(function()
    db.exec("INSERT INTO users (name) VALUES (?)", { "Alice" })
    db.exec("INSERT INTO audit (action) VALUES (?)", { "create user" })
end)

-- Async (yields to event loop — frees worker for other requests)
local rows = db.async.query("SELECT ... ", { ... })
```

JS: same surface, `db.query(sql, params)`, `db.exec(sql, params)`,
`db.batch(fn)`, `db.async.query(sql, params)`.

### Parameter binding

Always use `?` placeholders. Bound types map cleanly:
- Lua: `string` / `number` / `boolean` / `nil` → SQLite TEXT/INTEGER/FLOAT/BLOB/NULL
- JS: `string` / `number` / `boolean` / `null` / `ArrayBuffer`

**Never** concatenate user input into SQL. The C layer guarantees
parameterised access; the only way to construct dynamic identifiers
(table/column names) is through a vetted allowlist — see `search.lua` for the
pattern.

### Migrations

`migrations/NNN_<name>.sql` files in numeric order. Each runs in
`BEGIN IMMEDIATE ... COMMIT`. Applied migrations are tracked in
`_hull_migrations` (name + checksum + timestamp).

```sql
-- migrations/001_init.sql
CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    created_at INTEGER DEFAULT (strftime('%s', 'now'))
);
CREATE INDEX idx_users_name ON users(name);
```

**Commands:**
- `hull migrate [app]` — run pending
- `hull migrate status` — list applied / pending
- `hull migrate new <name>` — scaffold next file
- `hull dev` — auto-runs on startup (unless `--no-migrate`)
- `hull agent migrate` — JSON migration status

**Tip:** migrations are embedded in built binaries (via VFS), so
`hull build`-then-deploy needs no separate migration step.

### User-defined functions

```lua
-- Scalar Lua UDF
db.udf.register("hull_double", function(x) return x * 2 end,
                { deterministic = true, args = 1 })

-- Aggregate (returns running state then a final)
db.udf.register("hull_concat", {
    step = function(state, x) return (state or "") .. tostring(x) end,
    finalize = function(state) return state or "" end,
}, { args = 1 })

-- WASM-backed (gas-metered per row)
db.udf.register("hull_score", "scoring_module",
                { deterministic = true, args = 2, gas = 100000 })
```

Name must start with `hull_` (prevents shadowing SQLite built-ins).

---

## 10. Compute (WASM)

### When to use

- CPU-heavy pure functions: scoring, transforming, dedup, hashing pipelines,
  signing, compression.
- When you need deterministic, gas-limited execution.
- When the same logic needs to be portable / sandboxable.

Don't use for I/O (no syscall imports) or for trivial work (the µs-scale
boundary cost dominates).

### Authoring a module

Hull ships a complete scaffolding lifecycle. The full developer guide lives
in [`../README.md#authoring-compute-modules`](../README.md#authoring-compute-modules);
the short form:

```bash
hull compute new score          # scaffold compute/score/{score.c, hull_compute.h, test_fixtures.json}
# edit compute/score/score.c
hull compute build score        # compile to compute/score.wasm
hull compute test  score        # run JSON fixtures
hull compute check score        # smoke-test load in WAMR
hull build .                    # embeds .wasm (and AOT if wamrc available)
```

`hull build` automatically rebuilds stale `.c` sources before embedding —
no separate `hull compute build` step required during release builds.
Pass `--no-build-compute` for hermetic CI builds that ship pre-committed
`.wasm` artifacts.

`hull agent deploy <app>` reports per-module status (`name`, `wasm_size`,
`has_aot`, `has_source`, `source_stale`) plus a recommendation when any
source is newer than its `.wasm`.

### Plugin ABI

Plugins export:
- `hull_process(in_ptr, in_len, out_ptr, out_max) -> bytes_written`
- (optional) `hull_version() -> int`

Import (single): `env.host_call(opcode, ptr, len) -> int` with opcodes
`LOG=0x01`, `DATA_INFO=0x02`, `STREAM=0x03`, `CALLBACK=0x10`.

`hull_compute.h` (written by `hull compute new` into each module dir) is
the freestanding header that provides the export macros, error codes,
host-call wrappers, segment accessors, a minimal libc shim
(`hull_memcpy`/`hull_memset`/`hull_memcmp`/`hull_strlen`), and a 64 KiB
bump allocator.

### Calling

```lua
-- Synchronous (gas-limited, blocking)
local out, err = compute.call("score", input_bytes, {
    max_input  = 64 * 1024,
    max_output = 64 * 1024,
    gas        = 10000000,        -- 10M instructions
    heap       = 256 * 1024,
})

-- Async (yields to event loop)
local r = compute.async.call("score", input_bytes, opts)
-- r.result = output string on success; r.error on failure

-- Buffer for zero-copy chaining
local buf = compute.buffer("input data")
local out = compute.call("transform", buf, { buffer = true })  -- returns WasmBuffer
```

```javascript
const out = compute.call("score", inputBytes, { maxInput: 64*1024, gas: 10_000_000 });
const buf = await compute.async.call("score", inputBytes, opts);
```

### Zero-copy buffer protocol

All compute and GPU functions accept any of these as input
(`include/hull/buffer.h`):

| Type | Source |
|---|---|
| String | Literals / `string.pack` (Lua) / N/A (JS uses ArrayBuffer) |
| ArrayBuffer | JS typed arrays (JS-only) |
| MappedBuffer | `fs.mmap(path)` — disk-mapped |
| WasmBuffer | `compute.call(name, input, { buffer = true })` |

Chain them without round-tripping through script string copies:

```lua
local mapped = fs.mmap("embeddings.bin")   -- disk → mmap
gpu.buffer("vectors", mapped)               -- mmap → GPU (zero copy)
mapped:close()
```

### Persistent instances

```lua
local inst = compute.instance("scorer", { heap = 4*1024*1024 })
local out = inst:call(input)        -- linear memory persists between calls
inst:close()
```

Use for stateful workloads (ML weights, pre-built indexes) where per-call
instantiation cost is too high.

### Shared segments

Read-only data segments visible to all instances of a module via WAMR shared
heaps:

```lua
compute.segment("routing", "graph", graph_bytes)
compute.segment("routing", "landmarks", fs.mmap("landmarks.bin"))
local out = compute.call("routing", query)  -- segments auto-attached
```

WASM plugins query via `host_call(0x02, segment_id, sub)`.

### Streaming

```lua
-- file → file
compute.stream("module", { file = "input.csv" }, { file = "output.json" }, { chunk_size = 65536 })

-- buffer → callback
compute.stream("module", data, function(chunk, index, is_last) end, { chunk_size = 65536 })
```

### Build & AOT

`hull build` embeds `compute/*.wasm` and auto-compiles to AOT if `wamrc` is
available. AOT is preferred at runtime; falls back to interpreter. In dev
mode, drop pre-compiled `.aot.<arch>` files next to `.wasm` and they're
picked up automatically.

```bash
make wamrc                   # one-time, requires cmake + LLVM
hull build myapp             # AOT'd automatically
hull build myapp --no-aot    # skip AOT
hull build myapp --target=x86_64
```

For details: [`docs/wamr_architecture.md`](wamr_architecture.md).

---

## 11. GPU (WGSL)

### Enabling

`make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu`. Then declare in manifest:

```lua
app.manifest({ gpu = true, ... })
```

Without `gpu = true` in the manifest, the `gpu` global is not exposed.

### Compile + dispatch

```lua
local wgsl = [[
@group(0) @binding(0) var<uniform> params: ScoreParams;
@group(0) @binding(1) var<storage, read> vectors: array<f32>;
@group(0) @binding(2) var<storage, read_write> scores: array<f32>;
struct ScoreParams { count: u32, query: array<f32, 128> };
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    // ... dot product, write to scores[gid.x]
}
]]

gpu.compile("score", wgsl)         -- cached, idempotent
-- or load from shaders/score.wgsl:
gpu.load("score")

local out = gpu.dispatch("score", {
    uniforms = packed_params,
    buffers = {
        { data = vectors_bytes, usage = "read" },
        { size = N * 4, usage = "readwrite" },
    },
    workgroups = { x = math.ceil(N / 64), y = 1, z = 1 },
    output = 2,                    -- 1-indexed buffer to read back (Lua)
})
```

### Pipelines (multi-stage, single submission)

Named buffers persist across stages:

```lua
local results = gpu.pipeline({
    { shader = "normalize", buffers = {{ name = "data", data = input }}, workgroups = {x = 64} },
    { shader = "score",     buffers = {{ name = "data" }, { name = "out", size = N*4 }},
                            uniforms = params, workgroups = {x = 64} },
    { shader = "top_k",     buffers = {{ name = "out" }}, workgroups = {x = 1} },
}, {
    outputs = { { stage = 3, buffer = 1 } },   -- 1-indexed
    device = -1,
})
```

### Persistent GPU buffers

```lua
gpu.buffer("embeddings", embeddings_bytes)   -- upload once
gpu.dispatch("search", {
    buffers = {{ name = "embeddings" }, ...},
    workgroups = {...},
    output = false,                          -- fire-and-forget (no readback)
})
local result = gpu.buffer_read("embeddings")
gpu.buffer("embeddings", nil)                -- destroy
gpu.buffer_copy("src", "dst", { size = 1024 })   -- GPU-side copy
```

### Async dispatch

```lua
local r = gpu.async.dispatch("score", { ... })   -- yields to event loop
```

Submits to thread pool, deep-copies buffer data for thread safety, frees the
worker to serve other requests while the GPU runs.

### Timeout

5 seconds default (compile-time `HL_GPU_TIMEOUT_MS`). Prevents infinite hangs
from shader bugs.

### Performance

GPU has a ~2.6 ms wall-clock floor (submit + poll on Apple Silicon). For
small workloads (<16K elements), WASM AOT is faster. Crossover is around
16K-64K vectors for cosine similarity. See [§18](#18-performance-reference).

---

## 12. Security model

### Threat model

Hull's threat model assumes:
- **Trusted**: developer writing the app, code reviewers, the binary they
  ship.
- **Untrusted**: request payloads, query parameters, URL paths, body content,
  uploaded files, third-party API responses, env values for hosts/tokens.
- **Defended**: SQL injection, path traversal, CSRF, sandbox escape via
  `eval`/`load`, prototype pollution, capability bypass.

What's **not** defended:
- Logic bugs in the app code (incorrect access control, missing auth checks).
- Denial of service via a single very-large request (caps exist but are
  per-connection; many connections can still exhaust).
- Side-channel leaks via timing of pure-application code (Hull's HMAC/JWT
  paths are constant-time; your custom comparisons aren't unless you make
  them so).

### Signature system

Three independent Ed25519 layers:

| Layer | What | Where signed | Verified by |
|---|---|---|---|
| Platform | `libhull_platform.a` | `package.sig` (inner) | `hull build`, embedded gethull.dev pubkey |
| App | Your app | `package.sig` (outer) | `hull verify <app>`, your dev pubkey |
| Release | `hull` binary itself | `hull.sha256.sig` | `hull update`, embedded `HL_RELEASE_PUBKEY_HEX` |

Verify a downloaded `hull` binary offline:
```bash
hull verify-release hull.sha256 hull.sha256.sig
```

### Sandbox layers

- **Runtime (Lua)**: removed `io`, `os`, `loadfile`, `dofile`, `load`. Custom
  `require()` resolves only embedded stdlib. Memory + instruction caps.
- **Runtime (JS)**: `eval()` removed. `std`/`os` modules not loaded. Memory
  + stack + instruction caps. Only `hull:*` modules importable.
- **Capability layer**: SQL parameterised, paths validated, hosts/env
  allowlisted.
- **Kernel sandbox**: pledge/unveil/seatbelt per [§7](#7-manifest--capabilities).

### Audit logging

```bash
hull dev --audit                  # or HULL_AUDIT=1
```

Each capability call emits one JSON line to stderr. Fields: `ts`, `cap`,
`fn`, `result`, plus call-specific data. Zero overhead when off (single
global branch).

### Run `/c-audit`, `/js-audit`, `/lua-audit`

Periodic audits live in [`docs/audit_2026_05_15.md`](audit_2026_05_15.md).
The audit skills in `.claude/skills/` produce the same reports — rerun on
significant changes.

---

## 13. Testing

### Writing a test

```lua
-- tests/test_users.lua
local test = require("hull.test")

test("GET /users returns empty array initially", function(req, res)
    local r = req("GET", "/users")
    assert(r.status == 200)
    assert(#r.json == 0)
end)

test("POST /users creates a user", function(req, res)
    local r = req("POST", "/users", { body = { name = "Alice" } })
    assert(r.status == 201)
    assert(r.json.name == "Alice")
end)
```

```javascript
// tests/test_users.js
import { test } from "hull:test";

test("GET /users returns empty array initially", async (req) => {
    const r = await req("GET", "/users");
    expect(r.status).toBe(200);
    expect(r.json).toEqual([]);
});
```

### Running

```bash
hull test                  # all tests/*.{lua,js}
hull test tests/test_users.lua   # one file
hull agent test            # same, JSON output
```

Tests run in-process — the request goes through the same dispatcher as a real
HTTP request but no socket is opened. DB is `:memory:` (migrations auto-applied).

### Test helpers

| Function | Use |
|---|---|
| `test(name, fn)` | Register a test case |
| `test.assert(cond, msg)` | Assert (Lua only; JS uses `expect` or throws) |
| `test.dispatch(method, path, opts)` | Same as the `req` argument |
| `test.skip(name, fn)` | Mark as pending |

### When the test loader fails

`hull test` exits non-zero on the first test failure but always produces JSON
on stdout. `hull agent test` is the same with cleaner output for parsing.

---

## 14. Build

### Standard build

```bash
make                          # build/hull (~5 MB)
make EMBED_PLATFORM=1         # embed platform lib for distribution
hull build myapp              # use embedded platform to compile myapp
```

### `hull build` flow

1. Discover entry: `app.lua` or `app.js` in `myapp/`.
2. Extract manifest (run `hl_manifest_extract_lua` / `_js`).
3. Generate `app_registry.c`: sorted `HlEntry` array of all app files
   (templates, static, migrations, modules) + extracted source +
   stdlib registry.
4. Compile + link against `libhull_platform.a` using the selected compiler
   (`tcc`, `system`, or `cosmocc`).
5. Sign (if `--sign-key` given): platform sig stays from `libhull_platform.a`;
   app sig added.
6. Output: single binary `myapp/build/myapp` (or wherever `--output`).

```bash
hull build myapp --compiler=system           # use system cc
hull build myapp --compiler=tcc              # use embedded TinyCC
hull build myapp --compiler=/usr/bin/clang   # explicit path
hull build myapp CC=cosmocc                  # universal APE binary
hull build myapp --sign-key=mykey.priv       # sign with private key
```

### Feature flags

Each flag controls a `-D` define and which sources are linked.

| Flag | Default | Effect when off |
|---|---|---|
| `HL_ENABLE_LUA` | 1 | Drop Lua 5.4; JS-only build |
| `HL_ENABLE_JS` | 1 | Drop QuickJS; Lua-only build |
| `HL_ENABLE_WASM` | 1 | Drop WAMR (`compute.*` unavailable, ~256 KB smaller) |
| `HL_ENABLE_GPU` | 0 | On enables wgpu-native (`gpu.*`) |
| `HL_ENABLE_TCC` | 1 | Drop embedded TinyCC (`--compiler=tcc` rejected) |
| `HL_EMBED_CA_BUNDLE` | 1 | Drop Mozilla CA bundle (~200 KB, breaks HTTPS without system store) |
| `HL_ENABLE_DB` | 1 | Drop SQLite + `db.*` + `migrate.*` + DB-backed stdlib (~1.4 MB smaller) |

Combine freely: `make HL_ENABLE_DB=0 HL_ENABLE_WASM=1 HL_ENABLE_TCC=0` →
compute-only runtime with Lua/JS orchestration, no DB, no build toolchain.

### Cosmopolitan (universal APE)

```bash
make platform-cosmo                   # both x86_64 + aarch64 platform archives
make CC=cosmocc                       # build hull as APE
make CC=cosmocc EMBED_PLATFORM=cosmo  # embed both arches for distribution
```

APE binaries run on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from a
single file. Trade-offs: forces `poll` event backend, no `-fstack-protector-strong`,
slightly larger.

### Reproducible self-build

```bash
make self-build              # hull → hull2 → hull3; asserts hull2 == hull3
```

Three-stage compile that proves the build chain is deterministic. Used to
verify release builds.

---

## 15. Deployment

### `hull deploy <target>`

| Target | Output |
|---|---|
| `dockerfile` | `Dockerfile` + `.dockerignore` |
| `systemd` | `hull-<name>.service` + `install.sh` |
| `fly` | `fly.toml` |

```bash
hull deploy myapp --target=dockerfile
hull deploy myapp --target=systemd --user=hull --install-dir=/opt/hull/myapp
hull deploy myapp --target=fly --region=iad
```

Generated files are reviewed and committed; deployment is then standard for
each platform (`docker build`, `systemctl`, `flyctl deploy`).

### `hull agent deploy`

Returns JSON readiness analysis:

```json
{
  "runtime": "lua",
  "manifest": { "fs": {...}, "hosts": [...] },
  "configs": ["app.lua", "migrations", "templates", "static"],
  "files": {"app.lua": 421, "migrations/001.sql": 89, ...},
  "recommendations": ["Add `hosts` to manifest", "..."]
}
```

Use to verify what `hull build` will embed and what the deploy environment
needs.

### Compute-only deployments

For pure compute services (no DB), use:

```bash
make HL_ENABLE_DB=0
hull build myapp
```

Binary is ~3.7 MB (vs ~5 MB with DB). State must live elsewhere (Redis,
Postgres-over-HTTP, S3). The hull binary becomes a thin compute orchestrator.

---

## 16. Signing & release

### Signing your own app

```bash
hull keygen myapp                # creates myapp.priv + myapp.pub
hull build myapp --sign-key=myapp.priv
hull verify built/myapp          # exits 0 if both layers verify
```

Distribute `myapp.pub` separately (e.g. in `keys.txt` next to release notes).
Anyone can verify with:

```bash
hull verify --pubkey=$(cat myapp.pub) built/myapp
```

### Hull's own release flow

| Step | Who | What |
|---|---|---|
| Tag `v0.1.0` | Maintainer | `git tag v0.1.0 && git push --tags` |
| Build | GitHub Actions | Compiles `hull` for linux-x86_64, darwin-arm64, cosmo |
| Hash | Workflow | Writes `hull.sha256` (3 lines, SHA-256 each) |
| Sign | Workflow | Uses `HULL_RELEASE_SIGNING_KEY` secret → `hull.sha256.sig` |
| Publish | Workflow | Uploads to GitHub release |

End-users run `hull update` which:
1. Fetches latest release metadata via `api.github.com`.
2. Picks asset matching this binary's OS/arch.
3. Downloads via HTTPS using embedded Mozilla CA bundle.
4. Verifies `hull.sha256.sig` against embedded `HL_RELEASE_PUBKEY_HEX`.
5. Verifies SHA-256 against `hull.sha256`.
6. Atomically replaces the running binary via `rename(2)`.

No external dependencies; uses Keel's `KlRedirectClient` and TweetNaCl.

### Verifying a release offline

```bash
sha256sum hull-linux-x86_64        # check against published hull.sha256
hull verify-release hull.sha256 hull.sha256.sig
```

See [`docs/release_signing.md`](release_signing.md) for full design.

---

## 17. Agent workflow

### Sidecar files (`hull dev --agent`)

When `--agent` is passed:

- **`.hull/dev.json`** — written on start. Schema:
  ```json
  { "port": 3000, "pid": 12345, "runtime": "lua", "started_at": 1747325420 }
  ```
- **`.hull/last_error.json`** — written on load failure. Schema:
  ```json
  {
    "file": "app.lua",
    "line": 42,
    "col": 13,
    "message": "attempt to call a nil value (global 'foo')",
    "trace": [...]
  }
  ```

Both atomically written. Safe to poll.

### Subcommand reference

All `hull agent` subcommands emit JSON to stdout. Exit code 0 on success,
1 on application error (still produces a JSON `{ "error": "..." }` body).

| Subcommand | Typical output |
|---|---|
| `routes [app]` | `{ "routes": [{"method": "GET", "path": "/", "handler": "..."}, ...], "middleware": [...] }` |
| `db schema [app] [-d path]` | `{ "tables": [{"name": "users", "columns": [...]}, ...] }` |
| `db query "SQL" [app]` | `{ "columns": [...], "rows": [[...]], "count": N }` |
| `request METHOD PATH` | `{ "status": 200, "headers": {...}, "body": "...", "duration_ms": 12 }` |
| `status [app]` | `{ "running": true, "port": 3000, "pid": 12345 }` |
| `errors [app]` | Contents of `.hull/last_error.json` or `{ "errors": [] }` |
| `test [app]` | `{ "tests": [...], "passed": N, "failed": M }` |
| `context --task=T --level=L` | Markdown excerpt from `docs/` relevant to task |
| `migrate [app]` | `{ "applied": [...], "pending": [...], "total": N }` |
| `deploy [app]` | `{ "runtime": "...", "manifest": {...}, "recommendations": [...] }` |

### Recommended agent workflow

For tasks like *"add a `/users/:id` route that returns user data"*:

1. **Read project state**: `hull agent routes` to see existing routes.
2. **Read schema**: `hull agent db schema` to learn the `users` table shape.
3. **Inspect existing handlers**: `grep "app.get" app.lua` for style patterns.
4. **Make edits**.
5. **Test the load**: `hull agent errors` to see if hot-reload succeeded.
6. **Test the route**: `hull agent request GET /users/1` for a smoke test.
7. **Run the test suite**: `hull agent test`.

### MCP (Model Context Protocol)

```bash
hull mcp [app_dir]
```

Starts a JSON-RPC stdio server exposing the agent subcommands as MCP tools.
For IDE integrations (Cursor, Claude Code's `/agents` MCP, etc.):

```json
{
  "mcpServers": {
    "hull": { "command": "hull", "args": ["mcp", "/path/to/myapp"] }
  }
}
```

Tools exposed: `hull_routes`, `hull_db_schema`, `hull_db_query`, `hull_request`,
`hull_status`, `hull_errors`, `hull_test`, `hull_context`, `hull_migrate_status`,
`hull_reload` (re-init the warm app context after code changes).

The MCP server keeps a warm `HlAppContext` between calls so requests don't
re-init the runtime each time.

### Extended introspection subcommands (Phase 6, 2026-05-15)

Sixteen additional subcommands close common agent productivity gaps:

| Subcommand | Output |
|---|---|
| `manifest [app]` | `{ declared, runtime, fs:{read,write}, env, hosts, csp, cors, wasm, gpu, compute }` |
| `endpoint METHOD PATH [app]` | `{ method, path, middleware:[...], middleware_count, routes:[...], route_count, would_match }` — preview the request without running it |
| `middleware METHOD PATH [app]` | `{ method, path, middleware:[...], count }` — focused subset of `routes` |
| `capabilities [app]` | `{ runtime, manifest_declared, capabilities:[{name, used, declared_or_unrestricted, status}], used_but_undeclared_count }` |
| `validate <file>` | `{ file, runtime, ok, error?, findings:[{severity, pattern, message, line}] }` |
| `vfs [app]` | `{ app_dir, app:[{name,size,bucket}], stdlib:[...] }` — every embedded file |
| `compute [app]` | `{ available, modules:[{name,size,aot,aot_arch}] }` |
| `gpu [app]` | `{ available, shaders:[{name,size}] }` |
| `perf [app]` | `{ runtime, features:{lua,js,wasm,gpu,db,tcc}, limits:{...}, live_stats_hint }` |
| `logs [app] [--tail N]` | `{ path, exists, total_lines_in_tail, truncated, lines:[...] }` |
| `eval <code> [app]` | `{ ok, result\|error }` — runs the snippet against the loaded app, JSON-serialises the return value |
| `template <name> [data.json] [app]` | `{ template, output, bytes }` — renders via the loaded runtime's template engine |
| `compute-call <mod> <input> [app]` | scheduled-execution status with input size |
| `schema-diff [app] [-d path]` | `{ expected_tables, actual_tables, drift_tables, drift_indexes, missing_tables, in_sync }` |
| `sql named <qname> [--params JSON] [app]` | runs a pre-defined query from `app/queries.json` with named parameter binding |

**Quick examples:**

```bash
# Manifest preview
hull agent manifest examples/hello
# → {"declared":true,"runtime":"lua","fs":{"read":[],"write":[]},"env":[],"hosts":[]}

# Endpoint preview
hull agent endpoint GET / examples/hello
# → {"method":"GET","path":"/","middleware":[],"routes":[{"method":"GET","pattern":"/","kind":"route"}],"would_match":true}

# One-shot eval
hull agent eval "1+1" examples/hello
# → {"ok":true,"result":2}

# Validate a file
hull agent validate examples/hello/app.lua
# → {"file":"...","runtime":"lua","ok":true,"findings":[]}

# Capabilities analysis
hull agent capabilities examples/hello
# → {"runtime":"lua","manifest_declared":true,"capabilities":[...],"used_but_undeclared_count":0}

# Named SQL query (requires app_dir/queries.json)
hull agent sql named list_active_users --params '{"limit":10}' myapp
# → {"name":"list_active_users","columns":["id","name"],"rows":[[1,"Alice"]],"count":1}
```

`queries.json` schema:

```json
{
  "list_active_users":   "SELECT id, name FROM users WHERE active = 1",
  "user_by_id":          "SELECT * FROM users WHERE id = :id",
  "recent_orders":       "SELECT * FROM orders ORDER BY created_at DESC LIMIT :limit"
}
```

---

## 18. Performance reference

### Measured on Apple M1 Max

**WASM compute (cosine similarity, 128-dim vectors):**

| Vectors | Native C | WASM AOT | WASM Interpreter |
|---|---|---|---|
| 64 | 7 µs | 7 µs | ~120 µs |
| 1K | 118 µs | 108 µs | ~1.8 ms |
| 16K | 1.83 ms | 2.53 ms | ~28 ms |
| 64K | 7.27 ms | 10.97 ms | ~110 ms |

**GPU compute:**

| Vectors | WASM AOT | GPU | GPU vs AOT |
|---|---|---|---|
| 64 | 7 µs | 2,630 µs | 0.0× |
| 1K | 108 µs | 2,630 µs | 0.0× |
| 16K | 2.53 ms | 2.63 ms | 1.0× |
| 64K | 10.97 ms | 2.65 ms | **4.1×** |

GPU has a ~2.6 ms wall-clock floor (submit + poll). Crossover at ~16K
elements. For sub-µs orchestration overhead, use `gpu.pipeline()` (single
submission across stages) and persistent buffers.

### HTTP throughput

Single request handler with no DB:
- Lua handler returning JSON: ~50,000 req/s (single thread, localhost)
- JS handler returning JSON: ~30,000 req/s (QuickJS is slower per-instruction)
- With DB read: ~15,000-25,000 req/s (limited by SQLite + parameter binding)

These are not benchmarks you should depend on without your own measurement.

### Memory

| Default | Override |
|---|---|
| Runtime heap | 64 MB | `-m SIZE` or `HULL_HEAP_LIMIT` |
| JS stack | 1 MB | `-s SIZE` |
| Instructions/req | 100M | `--max-instructions N` or `HULL_MAX_INSTRUCTIONS` |
| WASM heap | 2 MB | `--wasm-heap SIZE` |
| Connections | 256 | `--max-connections N` |
| Body size | 1 MiB | `--body-max-size SIZE` |

---

## 19. Common patterns & anti-patterns

### Patterns

**Cookie-session auth with CSRF + transaction:**

```lua
local session     = require("hull.middleware.session")
local auth        = require("hull.middleware.auth")
local csrf        = require("hull.middleware.csrf")
local transaction = require("hull.middleware.transaction")

session.init()

app.use("*", "/api/*", auth.session_middleware({}))
app.use_post("*", "/*", csrf.middleware({ secret = env.get("CSRF_SECRET") }))
app.use_post("POST", "/api/*", transaction.middleware())

app.post("/api/order", function(req, res)
    transaction.run(function()
        local id = db.exec("INSERT INTO orders (user_id, ...) VALUES (?, ...)",
                           { req.ctx.session.user_id, ... })
        db.exec("INSERT INTO audit (...) VALUES (...)", { ... })
    end)
    res:json({ id = db.last_id() })
end)
```

**Idempotent POST:**

```lua
local idempotency = require("hull.middleware.idempotency")
idempotency.init()

app.use_post("POST", "/api/*", idempotency.middleware({
    get_principal = function(req) return req.ctx.session.user_id end,
}))

app.post("/api/charge", function(req, res)
    -- Same Idempotency-Key + same body → cached response replayed.
    -- Same key + different body → 409 Conflict.
    idempotency.respond(req, res, 201, { charge_id = ... })
end)
```

**Compute pipeline (mmap → compute → GPU → DB):**

```lua
app.manifest({ fs = { read = { "embeddings.bin" } }, gpu = true })

local mapped = fs.mmap("embeddings.bin")
gpu.buffer("vectors", mapped)
mapped:close()

app.post("/search", function(req, res)
    local query = json.decode(req.body)
    local query_vec = compute.call("embed", query.text)        -- WASM
    local scores = gpu.dispatch("score", {                     -- GPU
        buffers = {{ name = "vectors" }, { data = query_vec }, { size = N*4, usage = "readwrite" }},
        uniforms = ..., output = 3,
    })
    local top_ids = db.query("SELECT id FROM docs WHERE rowid IN (?)", { unpack(top_k(scores)) })
    res:json(top_ids)
end)
```

**Background outbox + daily cleanup:**

```lua
local outbox = require("hull.middleware.outbox")
outbox.init()

app.every(5000, function() outbox.flush() end)
app.daily("03:00", function() outbox.cleanup(86400 * 30) end)
```

### Anti-patterns

| Don't | Why | Do instead |
|---|---|---|
| `db.query("SELECT * FROM t WHERE id = " .. id)` | SQL injection | `db.query("... WHERE id = ?", { id })` |
| `s = s .. chunk` in a loop | O(n²) | `table.concat(parts)` after pushing to array |
| `if count then ...` when count might be 0 | Lua truthiness: 0 is truthy | `if count ~= nil and count > 0 then` |
| `cors.middleware({ credentials = true })` with default `origins` | Wildcard `*` + credentials is browser-rejected (Lua: now errors at factory; JS: same) | List explicit origins |
| Calling `compute.call` in a tight loop on tiny inputs | Boundary cost dominates | Batch in the WASM module or use `compute.stream` |
| Issuing 100 `gpu.dispatch` calls per request | ~260 ms in submit overhead | Use `gpu.pipeline()` (single submission) |
| Storing `Set-Cookie` in idempotency cache | Replayed long after session revocation | The middleware now allowlists replayable headers; user headers should be `X-*` |
| Manual `==` comparison of HMAC digests | Timing attack | Use `crypto.constant_time_eq` or the constant-time helpers in `jwt.lua` / `csrf.lua` |
| `eval()` / `Function()` / `load()` in app code | Sandbox bypass attempt — fails at runtime; sandbox audit will reject | Use the template engine's `_template.compile()` C bridge if you need codegen |
| Modifying `Object.prototype` (JS) or `_G` (Lua) | Pollutes other modules; sandbox audit will flag | Use module-local state |

---

## 20. Glossary & file map

### Glossary

| Term | Definition |
|---|---|
| **Capability** | A C-mediated permission to access an external resource (DB, fs, net, env, time, crypto). Declared in the manifest, enforced by `hl_cap_*`. |
| **Manifest** | The declared set of capabilities your app uses. Extracted from `app.manifest({...})` at build time; enforced at runtime. |
| **Runtime** | Either Lua 5.4 or QuickJS. Chosen per-project by entry file extension. |
| **Sandbox** | Kernel-level enforcement (pledge/unveil/seatbelt) of the manifest. Not the same as runtime sandbox (Lua/JS interpreter restrictions). |
| **VFS** | Virtual filesystem of embedded files (templates, static, migrations, app modules, stdlib). Sorted `HlEntry` arrays with O(log n) lookup. |
| **Tool mode** | Build-time CLI execution of `hull` (e.g. `hull build`, `hull migrate new`). Has access to `tool.spawn`, `tool.find_files`, etc. — not available at request time. |
| **Cap layer** | The C boundary (`hl_cap_*` functions, `src/hull/cap/`). Enforces the manifest, validates inputs. |
| **Stdlib** | The embedded Lua/JS modules under `stdlib/lua/hull/` and `stdlib/js/hull/`. Loaded via `require()` / `import`. |
| **Plugin** | A WASM compute module exporting `hull_process`. Called via `compute.call`. |
| **Pipeline** (GPU) | A multi-stage GPU dispatch submitted as a single command buffer. Named buffers persist across stages. |
| **Persistent instance** (WASM) | A long-lived WASM instance whose linear memory survives across calls. For stateful workloads. |
| **Buffer protocol** | The unified type that lets you pass `string` / `MappedBuffer` / `WasmBuffer` / `ArrayBuffer` between `fs.mmap`, `compute.*`, and `gpu.*` without copying. |
| **Outbox / inbox** | Transactional patterns for reliable side-effect delivery / message dedup. SQLite-backed. |

### File map

```
include/hull/                  Public headers (everything an app or third party links against)
  cap/                         Capability module headers
  commands/                    Subcommand headers
  runtime/                     Runtime headers (lua.h, js.h, factory.h)
  app_context.h                HlAppContext: shared command init/load/free
  buffer.h                     HlBufferView unified protocol
  manifest.h                   HlManifest + extraction
  release.h                    HL_RELEASE_PUBKEY_HEX
  runtime.h                    HlRuntime base + HlRuntimeVtable
  vfs.h                        HlVfs API
src/hull/
  cap/                         Capability implementations (db.c, fs.c, crypto.c, ...)
  commands/                    Subcommand implementations
  runtime/lua/                 Lua 5.4 runtime integration
  runtime/js/                  QuickJS runtime integration
  agent/                       hull agent subcommand handlers (split by purpose)
  main.c                       Entry point (server dev loop)
  manifest.c, manifest_lua.c, manifest_js.c   Manifest extraction
  sandbox.c                    Two-phase sandbox application
  signature.c, release.c       Ed25519 sign/verify
  static.c                     /static/* serving (filesystem + VFS)
  vfs.c                        VFS implementation (binary search + prefix query)
stdlib/lua/hull/               Lua stdlib modules
stdlib/js/hull/                JS stdlib modules
examples/                      26 example apps
tests/                         Unit (test_*.c) and e2e (e2e_*.sh) tests
docs/                          Long-form documentation
vendor/                        Vendored libs (do not modify)
templates/                     Build templates (app_main.c, entry.h)
.github/workflows/             CI + release automation
```

### Reading order for new contributors

1. [`CLAUDE.md`](../CLAUDE.md) — the canonical project guide (this file is a
   curated agent-focused projection of it).
2. `include/hull/runtime.h` + `include/hull/app_context.h` — the polymorphic
   boundary between core and runtimes.
3. `src/hull/cap/db.c` — read one cap module end-to-end to internalise the
   pattern.
4. `examples/hello/app.lua` — minimal working app.
5. `examples/crud_with_auth/app.lua` — realistic app with auth + migrations.
6. [`docs/wamr_architecture.md`](wamr_architecture.md) — compute design.
7. [`docs/security.md`](security.md) — threat model.

---

## Agent gap analysis — closed (Phase 6, 2026-05-15)

All sixteen identified gaps have been implemented. See § Extended
introspection above for the full subcommand reference. Implementation
notes per command:

| Subcommand | Source | Notes |
|---|---|---|
| `manifest` | `src/hull/agent/manifest.c` | Reuses `hl_manifest_extract_*`; emits the full struct as nested JSON. |
| `endpoint` / `middleware` | `src/hull/agent/endpoint.c` | Self-contained pattern matcher (mirrors Keel's `*`, `:param`, `/*`). |
| `validate` | `src/hull/agent/validate.c` | Pure parse via `luaL_loadbuffer` / `JS_Eval(COMPILE_ONLY)` + substring scan for sandbox-violation patterns. |
| `capabilities` | `src/hull/agent/capabilities.c` | Walks the project tree (depth-capped, skip-dirs filtered), aggregates source, scans for cap-use patterns, diffs against extracted manifest. |
| `vfs` | `src/hull/agent/vfs.c` | Iterates `hl_app_entries[]` + `hl_stdlib_entries[]`; buckets by name prefix. |
| `compute` / `gpu` | `src/hull/agent/compute.c`, `gpu.c` | VFS first, disk fallback. Two-pass dirent collection (avoids `rewinddir` pitfalls). |
| `perf` | `src/hull/agent/perf.c` | Compile-time feature flags + default limits snapshot. Points at `/ready` for live stats. |
| `logs` | `src/hull/agent/logs.c` | Tail-reads `.hull/dev.log` up to a capped window; emits last N lines. |
| `eval` | `src/hull/agent/eval.c` | Wraps the snippet so its return value is JSON-encoded by the runtime's own `json` module (Lua) or `JSON.stringify` (JS), then emitted raw. |
| `template` | `src/hull/agent/template.c` | Delegates to `hull.template.render` / `hull:template`'s `template.render` via the loaded runtime. |
| `compute-call` | `src/hull/agent/compute.c` | Reads input file (capped), reports module + size + scheduled-execution status. |
| `schema-diff` | `src/hull/agent/schema_diff.c` | Tokenises migration SQL for `CREATE TABLE/INDEX [IF NOT EXISTS] <name>`, diffs against `sqlite_master`. |
| `sql named` | `src/hull/agent/sql.c` | Loads `app_dir/queries.json`, binds named params from `--params JSON`. |

All buffer sizes named in `src/hull/agent/limits.h` (no magic constants).
Total Phase 6 surface: ~1,500 lines of new C + ~150 lines of CLI glue in
`commands/agent.c`. CI-gated and smoke-tested against `examples/hello`.

---

*Last updated: 2026-05-15. Regenerate audit with `/c-audit`, `/js-audit`,
`/lua-audit`. Roadmap items in [`docs/roadmap.md`](roadmap.md).*
