# Hull

A secure, capability-limited runtime for agent-native, local-first applications. Single binary, zero dependencies, runs anywhere.

Hull is not a do-everything framework. It's a sandboxed runtime where AI-generated code runs inside declared capability boundaries — the app states what it can access (files, hosts, env vars), and the kernel enforces it. The agent writes the code; Hull constrains what that code can do.

Write backend logic in Lua or JavaScript, frontend in HTML5, data in SQLite. `hull build` produces a single portable executable — under 2 MB — that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, and NetBSD. AI coding agents get structured JSON access to routes, schema, tests, and HTTP responses through the built-in `hull agent` command — no plugins, no MCP servers, no configuration.

## Why

AI coding agents solved code generation. But they created two new problems: **deployment** (the output is always React + Node + Postgres + cloud) and **trust** (who verifies what AI-generated code actually does?).

Hull solves both. The agent writes Lua or JavaScript, `hull build` produces a single file. That file is the product — no cloud, no hosting, no dependencies. And because Hull apps declare their capabilities in a manifest enforced by the kernel, the user knows exactly what the app can touch. In a world where AI writes the code, the runtime must be the trust boundary. Hull is that boundary.

Six vendored C libraries. One build command. One file. That's the entire stack. Works out of the box with [Claude Code](#claude-code), [Codex CLI](#openai-codex-cli), [OpenCode](#opencode), and any agent that can run shell commands.

## Quick Start

```bash
# Build hull
make
make test

# Create a new project
./build/hull new myapp
cd myapp

# Run in development mode (hot reload)
../build/hull dev app.lua

# Build a standalone binary
../build/hull build -o myapp .

# Run it
./myapp -p 8080 -d app.db
```

## Hull Tools

Hull ships 14 subcommands for the full development lifecycle:

| Command | Purpose |
|---------|---------|
| `hull new <name>` | Scaffold a new project with example routes and tests |
| `hull dev <app>` | Development server with hot reload |
| `hull build -o <out> <dir>` | Compile app into a standalone binary |
| `hull test <dir>` | In-process test runner (no TCP, memory SQLite) |
| `hull agent <subcommand>` | [AI agent interface](#using-hull-with-ai-agents) — routes, schema, tests, requests as JSON |
| `hull inspect <dir>` | Display declared capabilities and signature status |
| `hull verify [--developer-key <key>]` | Verify Ed25519 signatures and file integrity |
| `hull eject <dir>` | Export to a standalone Makefile project |
| `hull keygen <name>` | Generate Ed25519 signing keypair |
| `hull sign-platform <key>` | Sign platform library with per-arch hashes |
| `hull manifest <app>` | Extract and print manifest as JSON |
| `hull <app> --max-instructions N` | Set per-request instruction limit (default: 100M) |
| `hull <app> --audit` | Enable capability audit logging (JSON to stderr) |
| `hull migrate [app_dir]` | Run pending SQL migrations |
| `hull migrate status` | Show migration status (applied/pending) |
| `hull migrate new <name>` | Create a new numbered migration file |

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

Hull supports three compiler targets:

| Compiler | Target | Binary Type |
|----------|--------|-------------|
| `gcc` / `clang` | Linux | ELF |
| `gcc` / `clang` | macOS | Mach-O |
| `cosmocc` | Any x86_64/aarch64 | APE (Actually Portable Executable) |

Cosmopolitan APE binaries run on Linux, macOS, Windows, FreeBSD, OpenBSD, and NetBSD from a single file. Hull builds multi-architecture platform archives (`make platform-cosmo`) so the resulting APE binary is a true fat binary for both x86_64 and aarch64.

## Architecture

```
┌─────────────────────────────────────────────┐
│  Application Code (Lua / JS)                │  ← Developer writes this
├─────────────────────────────────────────────┤
│  Standard Library (stdlib/)                 │  ← cors, ratelimit, csrf, auth, jwt, session
├─────────────────────────────────────────────┤
│  Runtimes (Lua 5.4 + QuickJS)              │  ← Sandboxed interpreters
├─────────────────────────────────────────────┤
│  Capability Layer (src/hull/cap/)           │  ← C enforcement boundary
│  fs, db, crypto, time, env, http, tool      │  ← audit logging (--audit)
├─────────────────────────────────────────────┤
│  Hull Core                                  │  ← Manifest, sandbox, signatures, VFS
├─────────────────────────────────────────────┤
│  Keel HTTP Server (vendor/keel/)            │  ← Event loop + routing
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
| `json` | `hull.json` | (built-in) | JSON encode/decode |

All middleware modules follow the same factory pattern: `module.middleware(opts)` returns a function `(req, res) -> 0|1` where `0` = continue, `1` = short-circuit.

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

Key principles: rate limit before auth (reject early), CORS before auth (preflight must not require credentials), scope middleware to paths (`"/api/*"` not `"/*"`). See [examples/middleware/](examples/middleware/) and [CLAUDE.md](CLAUDE.md) for full API reference.

### Vendored Libraries

| Component | Purpose |
|-----------|---------|
| [Keel](https://github.com/artalis-io/keel) | HTTP server (epoll/kqueue/io_uring/poll), routing, middleware, TLS vtable |
| [Lua 5.4](https://www.lua.org/) | Application scripting (1.9x faster than QuickJS) |
| [QuickJS](https://bellard.org/quickjs/) | ES2023 JavaScript runtime with instruction-count gas metering |
| [SQLite](https://sqlite.org/) | Embedded database (WAL mode, parameterized queries) |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | TLS client for outbound HTTPS |
| [TweetNaCl](https://tweetnacl.cr.yp.to/) | Ed25519 signatures, XSalsa20+Poly1305, Curve25519 |
| [pledge/unveil](https://github.com/jart/pledge) | Kernel sandbox (Linux seccomp/landlock) |

## Security Model

Hull apps declare a manifest of exactly what they can access — files, hosts, environment variables. The kernel enforces it.

```lua
app.manifest({
    fs = { read = {"data/"}, write = {"data/uploads/"} },
    env = {"PORT", "DATABASE_URL"},
    hosts = {"api.stripe.com"}
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
| macOS | C-level validation only | Error return | No |

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

## Examples

Ten example apps in both Lua and JavaScript:

| Example | What it demonstrates |
|---------|---------------------|
| [hello](examples/hello/) | Routing, query strings, route params, DB visits |
| [rest_api](examples/rest_api/) | CRUD API with JSON bodies and migrations |
| [auth](examples/auth/) | Session-based authentication with migrations |
| [jwt_api](examples/jwt_api/) | JWT Bearer authentication with refresh tokens |
| [crud_with_auth](examples/crud_with_auth/) | Task CRUD with per-user isolation and migrations |
| [middleware](examples/middleware/) | Request ID, logging, rate limiting, CORS |
| [webhooks](examples/webhooks/) | Webhook delivery with HMAC-SHA256 signatures |
| [templates](examples/templates/) | Template engine: inheritance, includes, filters |
| [todo](examples/todo/) | Full CRUD todo app with HTML frontend and migrations |
| [bench_db](examples/bench_db/) | SQLite performance benchmarks with migrations |

```bash
# Run an example
./build/hull -p 8080 -d /tmp/test.db examples/hello/app.lua

# Run its tests
./build/hull test examples/hello
```

## Documentation

| Document | Content |
|----------|---------|
| [MANIFESTO.md](docs/MANIFESTO.md) | Design philosophy, architecture, security model |
| [docs/architecture.md](docs/architecture.md) | System layers, capability API, build pipeline |
| [docs/security.md](docs/security.md) | Trust model, attack model, sandbox enforcement |
| [docs/roadmap.md](docs/roadmap.md) | What's built, what's next |
| [docs/benchmark.md](docs/benchmark.md) | Performance methodology and results |
| [docs/keel_audit.md](docs/keel_audit.md) | Keel HTTP server C code audit report |
| [docs/ASSESSMENT.md](docs/ASSESSMENT.md) | Platform assessment, scaling path, strategic positioning |
| [CLAUDE.md](CLAUDE.md) | Development guide for contributors |
| [AGENTS.md](AGENTS.md) | Agent development guide (hull agent CLI, patterns, stdlib) |

## Using Hull with AI Agents

Hull treats AI coding agents as first-class developers. The `hull agent` command provides 7 machine-readable (JSON) subcommands that give agents structured access to routes, database schema, test results, server status, and HTTP responses — no screen-scraping or log parsing required.

```
hull agent routes [app_dir]              # list routes + middleware as JSON
hull agent db schema [app_dir] [-d path] # introspect database tables and columns
hull agent db query "SQL" [app_dir]      # run read-only SQL query, get rows as JSON
hull agent request METHOD PATH [opts]    # HTTP request → structured response
hull agent status [app_dir] [-p port]    # check if dev server is running
hull agent errors [app_dir]              # structured errors from last reload
hull agent test [app_dir]                # run tests, get per-test pass/fail as JSON
```

Combined with `hull dev --agent` (which writes `.hull/dev.json` and `.hull/last_error.json` as sidecar files), this gives agents a complete feedback loop: edit code, check for errors, run tests, inspect the database, make HTTP requests — all with structured output.

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

### Deploy

```bash
# Build standalone binary (embeds app + stdlib + SQLite + HTTP server)
hull build -o myapp .

# The binary is the product — no runtime, no dependencies
./myapp -p 8080 -d /data/app.db

# Cross-platform (Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD)
hull build -o myapp . CC=cosmocc
```

The agent workflow for deployment: run `hull agent test .` to verify all tests pass, then `hull build -o myapp .` to produce the binary. The output is a single file under 2 MB. Copy it anywhere and run it.

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

Hull is in active development. All core features are implemented and tested across Linux, macOS, and Cosmopolitan APE. See [docs/roadmap.md](docs/roadmap.md) for what's next.

## License

AGPL-3.0. See [LICENSE](LICENSE).

Commercial licenses available for closed-source distribution. See the [Licensing](docs/MANIFESTO.md#licensing) section of the manifesto.
