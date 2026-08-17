# Hull. Agent Development Guide

Hull is a hardened, capability-secure runtime for AI-native systems. It provides structured JSON interfaces for AI coding agents, but also works for human developers and automated services. This document covers everything an agent needs to build, test, debug, and deploy Hull applications.

> **Comprehensive companion:** [`docs/agent_guide.md`](docs/agent_guide.md). Full SDLC reference (install → dev → test → build → sign → deploy → release), every CLI command, every module's API surface, security model, performance reference, common patterns/anti-patterns, and a gap analysis of where `hull agent` could grow next. Start there if you need depth; this AGENTS.md is the quick reference.

## Quick Start

```bash
# Create a new project
hull new myapp && cd myapp

# Start development server (hot-reload)
hull dev --agent app.lua -p 3000

# In another terminal. Introspect the app
hull agent routes .                    # list all routes as JSON
hull agent db schema .                 # show database tables and columns
hull agent request GET /health         # HTTP request to the running server
hull agent status .                    # check if dev server is running
hull agent test .                      # run tests with JSON output
```

## Architecture Overview

```
Application Code (Lua or JS)
        ↓
    app.manifest({...})       # declare capabilities
    app.get("/path", fn)      # register routes
    app.use("*", "/*", mw)    # register middleware
        ↓
Capability Layer (C)          # enforces security boundaries
        ↓
    db.query() / db.exec()    # SQLite (WAL mode, parameterized, _hull_* tables blocked)
    fs.read() / fs.mmap()     # sandboxed filesystem (mmap → GPU zero-copy)
    crypto.sha256() / etc.    # cryptographic primitives
    http.get() / http.post()  # outbound HTTP (host allowlist)
    gpu.dispatch() / pipeline # GPU compute (wgpu-native, optional)
        ↓
Keel HTTP Server (C)          # epoll/kqueue event loop + async + thread pool
        ↓
Kernel Sandbox                # pledge+unveil (Linux), C-level (macOS)
```

WASM compute plugins provide a sandboxed data-plane layer for CPU-intensive computation. Plugins are pure functions (no I/O) that run in isolated WASM linear memory with gas metering. Place `.wasm` files in `compute/`, call via `compute.call("name", input)` (sync) or `compute.async.call("name", input)` (async, yields to event loop) from Lua/JS. `compute.stream("name", input, output, opts)` processes data larger than memory in chunks via a persistent instance. `hull build` auto-compiles `.c` sources to `.wasm` and to AOT when `wamrc` is available (~1.2x native speed vs ~54x for fast interpreter). When a module loads via the fast interpreter at runtime (no AOT artifact), Hull emits a one-time warning per module. Install `wamrc` (`make wamrc`) and rebuild to silence it. Scaffolding lifecycle: `hull compute new|build|test|check|refresh-header`. See `docs/wamr_architecture.md` and `hull agent context --task=compute --level=compact`.

WASM modules can also be registered as SQL UDFs via `db.udf.register("hull_name", "module_name", opts)`. The WASM function is called per row during query execution with gas metering.

GPU compute shaders (optional, `HL_ENABLE_GPU=1`) provide massively parallel data processing via wgpu-native (Metal/Vulkan/DX12). Compile shaders inline with `gpu.compile("name", wgsl)` or load from files with `gpu.load("name")` (reads `shaders/<name>.wgsl`). Dispatch with `gpu.dispatch("name", opts)` or chain multiple shaders with `gpu.pipeline(stages, opts)` for single-submission execution. Persistent buffers (`gpu.buffer()`) keep data on GPU across requests. Persistent textures (`gpu.texture(name, img)`) accept HlImage objects and can be read back via `gpu.texture_read(name)`. Dispatch supports `textures` array for sampled and storage texture bindings. Fire-and-forget mode (`output = false`) updates GPU buffers in-place without readback. `gpu.buffer_copy()` copies between GPU buffers without CPU roundtrip. GPU dispatches time out after 5 seconds (`HL_GPU_TIMEOUT_MS`).

Both WASM and GPU compute accept the same input types via the unified buffer protocol: strings, `MappedBuffer` (from `fs.mmap()`), and `WasmBuffer` (from `compute.call({buffer=true})`). This enables zero-copy data flow between disk, WASM, and GPU without Lua/JS string intermediaries. Declare `gpu: true` and/or `compute: true` in manifest.

**Mapped spans (zero-copy large files → WASM).** To scan/parse a very large file (OSM PBF, Parquet, raster, model blob) inside a WASM plugin without copying it into linear memory, map a window with `fs.mmap(path, { offset, length })` (whole-file if no opts; page alignment handled internally) and attach it read-only for one call: `compute.call("plugin", input, { spans = { { name = "source", buffer = w } } })`, then `w:close()`. The plugin reads it in place through the shipped SDK header `hull/wasm/span.h` — `hull_span_setup()` at start (SPAN_INFO host calls happen during setup only — a count query plus one per span; reads make none), then bounds-checked typed accessors `hull_span_read_u32le(w, len, off, &v)` etc. (u/i/f × 8/16/32/64 × LE/BE) that under AOT lower to a range check + a native load with **no per-access host call**. Kernel demand paging is retained (mapping a 50 GB file does not allocate 50 GB); files larger than WASM's 4 GiB address space are handled by moving a window (≤ 1 GiB) via the span's 64-bit `foffset`. Spans are read-only, strictly isolated per instance, and their lifetime is refcount-safe (a span never outlives its mapping). Reads are zero-copy with no per-access host call as **proven** properties; per-scan throughput is workload-dependent (see `docs/mapped_span_benchmark_design.md`), not a blanket "near-native" guarantee. Store accessors / writable spans are a deferred follow-up. Example: `examples/mapped_spans/`.

**The produced binary is tailored to the app.** Hull is completely modular and composable: the distributed hull's app-build base drops **every** reducible subsystem (both interpreters, the HTTP layer, the Keel event loop, mbedTLS, SQLite, WASM, image codecs), and `hull build` composes back only what the app uses, statically at build time (not dynamically loaded, no `dlopen`). You get exactly one interpreter (Lua or JS, inferred from the entry file); the HTTP layer + the Keel event loop + server only when the manifest declares an HTTP module (`hull/http-server`, `hull/http-client`, ws/sse/smtp/email); mbedTLS only when the app needs TLS (HTTP or a `--with=postgres`/`mysql` net-DB); SQLite only when the app uses `db`; WASM only when it uses `compute`; image codecs only when it declares `hull/image`. So an `app.main` CLI or compute tool with no HTTP/TLS/DB links **zero** Keel, mbedTLS, SQLite, or WASM (~2.1 MB, vs ~6.5 MB full). This is automatic, nothing to install or flag (all of it is embedded in `hull` and auto-composed); only large optional subsystems like gpu/duckdb/tui are `hull build --with=<name>`. `--flavor=pure-compute` is now just a preset that also *validates* the app declares no HTTP/TLS. Cosmo (the fat APE) keeps everything in-base.

Each app is a single file (`app.lua` or `app.js`) with optional:
- `migrations/*.sql`. Database schema (auto-run on startup)
- `templates/*.html`. Server-side templates
- `static/*`. Served at `/static/*`
- `compute/*.wasm`. WASM compute plugins (auto-AOT compiled during `hull build`)
- `shaders/*.wgsl`. GPU compute shaders (embedded by `hull build`, loaded via `gpu.load()`)
- `tests/test_*.lua` or `tests/test_*.js`. Test files

## App Lifecycle

Apps run in one of two modes, selected by what they register (not by a
build flag). Knowing which mode an app is in tells you which subcommands
apply (`hull dev` is server-only; running the entry directly, `hull app.lua`,
works for both) and what the process lifecycle looks like.

**Server mode**. App registers `app.get/post/use/ws/sse/every/daily`.

```
process start → init runtime → sandbox phase 1 → load app
  → manifest extracted → modules resolved → sandbox phase 2
  → migrations (HL_ENABLE_DB + ./migrations/) → start Keel
  → event loop: dispatch requests until SIGINT/SIGTERM
  → graceful shutdown → exit
```

**CLI mode** (see [docs/cli_mode.md](docs/cli_mode.md)). App
registers `app.main(fn)`.

```
process start → init runtime → sandbox phase 1 → load app
  → manifest extracted → modules resolved → sandbox phase 2
  → migrations (same gate as server mode)
  → call app.main(ctx) where ctx = { args, env, stdin, stdout, stderr }
  → main returns (sync or via coroutine/Promise)
  → cleanup (drain caches, scrub keys, close DB) → exit with main's rc
```

Apply this when working on app code:
- If you see `app.get/post/use` in the app, it's server mode. `hull dev`
  + `hull agent request` are the right tools to iterate.
- If you see `app.main`, it's CLI mode. Run the entry directly
  (`hull app.lua ...`); `hull dev` doesn't apply. Test files use
  `test.run_main({args=..., stdin=...})` instead of `test.get/post`.
- Registering both `app.main` and routes is an error. Hull fails at
  startup. If you're refactoring, pick one mode for the whole app.
- `hull agent manifest` reports `"mode": "server" | "cli"` so you can
  introspect without reading the source.

## Runtime Selection

Hull supports two runtimes. Selected by file extension:

| Runtime | Extension | Naming | Module Import |
|---------|-----------|--------|---------------|
| Lua 5.4 | `.lua` | `snake_case` | `require("hull.module")` |
| QuickJS (ES2023) | `.js` | `camelCase` | `import { mod } from "hull:module"` |

Both runtimes have identical capabilities. Choose based on preference.

## Agent CLI Commands

All `hull agent` commands output JSON to stdout. Errors go to stderr. Exit code 0 = success, non-zero = error.

### `hull agent routes [app_dir]`

List all registered routes and middleware.

```json
{
  "runtime": "lua",
  "routes": [
    {"method": "GET", "pattern": "/health"},
    {"method": "POST", "pattern": "/tasks"},
    {"method": "GET", "pattern": "/tasks/:id"},
    {"method": "PUT", "pattern": "/tasks/:id"},
    {"method": "DELETE", "pattern": "/tasks/:id"}
  ],
  "middleware": [
    {"method": "*", "pattern": "/*", "phase": "pre"},
    {"method": "POST", "pattern": "/api/*", "phase": "post"}
  ]
}
```

### `hull agent db schema [app_dir] [-d path]`

Introspect the database schema. Uses `data.db` in app dir, or `:memory:` with migrations.

```json
{
  "tables": [
    {
      "name": "tasks",
      "columns": [
        {"name": "id", "type": "INTEGER", "pk": true},
        {"name": "title", "type": "TEXT", "notnull": true},
        {"name": "done", "type": "INTEGER", "default": "0"}
      ]
    }
  ]
}
```

### `hull agent db query "SQL" [app_dir] [-d path]`

Run a read-only SQL query against the app database.

```json
{
  "columns": ["id", "title", "done"],
  "rows": [[1, "Buy groceries", 0], [2, "Write tests", 1]],
  "count": 2
}
```

### `hull agent request METHOD PATH [-p port] [-d body] [-H header]`

Send an HTTP request to the running dev server.

```bash
hull agent request GET /health
hull agent request POST /tasks -d '{"title":"New task"}' -H 'Content-Type: application/json'
hull agent request GET /tasks -p 8080
```

```json
{
  "status": 200,
  "elapsed_ms": 3,
  "headers": {"Content-Type": "application/json", "Content-Length": "42"},
  "body": "{\"id\":1,\"title\":\"New task\",\"done\":0}"
}
```

### `hull agent status [app_dir] [-p port]`

Check if the dev server is running.

```json
{"running": true, "port": 3000}
```

When using `hull dev --agent`, reads port and PID from `.hull/dev.json`.

### `hull agent errors [app_dir]`

Show structured errors from the last reload failure.

```json
{"error": "failed to load app.lua", "timestamp": 1709000000}
```

Returns `{"errors": []}` when no errors exist.

### `hull agent test [app_dir]`

Run tests with structured JSON output (per-file, per-test results).

```json
{
  "runtime": "lua",
  "files": [
    {
      "name": "test_app.lua",
      "tests": [
        {"name": "GET /health returns ok", "status": "pass"},
        {"name": "POST /tasks creates task", "status": "pass"},
        {"name": "GET /missing returns 404", "status": "fail", "error": "expected 404 got 200"}
      ]
    }
  ],
  "total": 3,
  "passed": 2,
  "failed": 1
}
```

### `hull agent deploy [app_dir]`

Analyze deployment readiness. Manifest, directory structure, existing configs.

```json
{
  "app_dir": "examples/todo",
  "runtime": "lua",
  "entry_point": "app.lua",
  "manifest": {
    "present": true,
    "env": ["SECRET_KEY"],
    "hosts": ["api.stripe.com"],
    "fs_write": ["uploads/"],
    "database": true,
    "gpu": false,
    "compute": false
  },
  "files": {
    "migrations": 3,
    "templates": 5,
    "static": 2,
    "compute": 0,
    "shaders": 0
  },
  "signature": { "package_sig": false },
  "configs": {
    "dockerfile": false,
    "systemd": false,
    "fly_toml": false
  },
  "recommendations": [
    "No signature. Run hull build --sign for verified deployments"
  ]
}
```

### Extended introspection (Phase 6, 2026-05-15; auth-stack additions v0.3.0)

Eighteen additional subcommands close the iterative-edit loop. All JSON to stdout.

#### `hull agent overview [app_dir]`

One-shot snapshot. Composes manifest + modules + capabilities + routes + compute/gpu inventory + cache state into a single JSON document. Cheaper than calling each subcommand individually when an agent just wants "what's this app's overall shape".

#### `hull agent auth-status [app_dir]` (v0.3.0)

Auth-stack health probe. Same shape as `hull.web.auth_health.check({include_counts = true})`: session table presence + count, audit-log cleanup status (`"scheduled" | "external" | "missing"`), pwned reachability (`not_yet_checked | reachable | fail_open`), TOTP enrollment count, RBAC table presence. Used by ops dashboards to surface "did we wire the auth stack correctly?" from the CLI.

```json
{"sessions":{"ok":true,"sessions":42},
 "audit_log":{"ok":true,"cleanup":"scheduled","events":1023},
 "pwned":{"ok":true,"status":"reachable","last_check_at":1717920000},
 "totp":{"ok":true,"enrolled_users":7},
 "rbac":{"ok":true,"roles":3,"permissions":12},
 "all_ok":true}
```

#### `hull agent tools`

Tool-registry × install-state. Lists every registered side-loadable tool (today: `wamrc`, the `zig` toolchain-free linker bundle, the `libc-musl-<arch>` static-link floors, and the `cosmocc` bundle for self-contained builds on stock Windows), whether each is available for this platform, whether it's installed, the install path, the expected asset name, and a copy-paste install hint. Independent of `HL_ENABLE_HTTP_CLIENT` - CLI flavors without the installer still list what's registered.

#### `hull agent sbom`

Same data as `hull sbom --format=json`. No `app_dir` argument; reports the running hull binary's SBOM (binary SHA-256, all vendored components with versions, CycloneDX/SPDX co-formats available via `hull sbom`).

#### `hull agent manifest [app_dir]`

Effective manifest as JSON (post-extraction). Different from `hull manifest` which prints a hash.

```json
{"declared":true,"runtime":"lua","fs":{"read":[],"write":[]},"env":[],"hosts":[]}
```

#### `hull agent endpoint METHOD PATH [app_dir]`

Preview which handler + middleware stack would fire for a request, without running it.

```json
{"method":"GET","path":"/users/42","middleware":[{"method":"*","pattern":"/api/*","phase":"pre","kind":"middleware"}],"middleware_count":1,"routes":[{"method":"GET","pattern":"/users/:id","kind":"route"}],"route_count":1,"would_match":true}
```

#### `hull agent middleware METHOD PATH [app_dir]`

Just the middleware stack. Focused subset of the above.

#### `hull agent capabilities [app_dir]`

Source-walk vs manifest diff. Surfaces declared-but-unused (tighten manifest) and used-but-undeclared (sandbox will deny).

```json
{"runtime":"lua","manifest_declared":true,"capabilities":[...],"used_but_undeclared_count":0}
```

#### `hull agent modules [app_dir]`

The app's declared first-party module surface plus the build's capability matrix. Use this to know what an app imports without parsing source.

```json
{"declared":["hull/crypto@1","hull/db@1","hull/web/middleware/session@1"],
 "intrinsic":["hull/app","hull/json","hull/log"],
 "build_caps":{"db":true,"wasm":true,"gpu":false},
 "registry_count":39}
```

`hull agent manifest` also includes the modules block under `manifest.modules` for free. Use this `modules` subcommand when you want the resolved closure (declared + intrinsic) and the build's compile-time subsystem flags side by side.

#### `hull agent validate <file>`

Parse one Lua/JS file in isolation. Faster than `hull dev` + `hull agent errors` for iterative editing.

```json
{"file":"app.lua","runtime":"lua","ok":true,"findings":[{"severity":"high","pattern":"loadstring(","message":"loadstring is removed in the Lua sandbox","line":42}]}
```

#### `hull agent vfs [app_dir]`

Every embedded file (app + stdlib) with name, size, and bucket (template/static/migration/shader/compute/stdlib-lua/stdlib-js/app-module/module).

#### `hull agent compute [app_dir]`

WASM modules: name, size, AOT presence, AOT architecture.

#### `hull agent gpu [app_dir]`

WGSL shaders + GPU availability.

#### `hull agent perf [app_dir]`

Compile-time feature flags + default limits snapshot. Points at `/ready` for live stats (requires `health.middleware()`).

#### `hull agent logs [app_dir] [--tail N]`

Last N lines from `.hull/dev.log`. Default 100, max 10000.

#### `hull agent eval <code> [app_dir]`

Run a one-shot Lua/JS snippet against the loaded app. Return value JSON-encoded by the runtime's own JSON encoder.

```bash
hull agent eval "1+1" myapp
# → {"ok":true,"result":2}

hull agent eval "db.query('SELECT count(*) AS n FROM users')[1].n" myapp
# → {"ok":true,"result":42}
```

#### `hull agent template <name> [data.json] [app_dir]`

Render a template with sample data. Validates the template path independently of the request pipeline.

#### `hull agent compute-call <module> <input-file> [app_dir]`

Invoke a WASM compute module against file input.

#### `hull agent schema-diff [app_dir] [-d path]`

DB schema drift: tokenises migration SQL, diffs `CREATE TABLE/INDEX` against `sqlite_master`. Reports `expected_tables`, `actual_tables`, `drift_tables`, `drift_indexes`, `missing_tables`, `in_sync`.

#### `hull agent sql named <qname> [--params JSON] [app_dir]`

Run a pre-defined query from `app_dir/queries.json`. Safer than `db query "..."` because the agent can only invoke published queries.

`queries.json` format:
```json
{
  "list_active_users": "SELECT id, name FROM users WHERE active = 1",
  "user_by_id":        "SELECT * FROM users WHERE id = :id",
  "recent_orders":     "SELECT * FROM orders ORDER BY created_at DESC LIMIT :limit"
}
```

Invocation:
```bash
hull agent sql named user_by_id --params '{"id":42}'
```

## Development Workflow

### 1. Start Dev Server

```bash
hull dev --agent --audit app.lua -p 3000
```

The `--agent` flag enables:
- `.hull/dev.json`. Written on start (port, PID, timestamp), removed on stop
- `.hull/last_error.json`. Written on app load failure, cleared on success

The `--audit` flag enables:
- Structured JSON logging of every capability call (db, fs, http, env, tool, smtp) to stderr
- Zero overhead when disabled. Single branch on a global flag

The `--max-instructions N` flag (or `HULL_MAX_INSTRUCTIONS` env var) overrides the per-request instruction limit (default: 100M). Both Lua and JS handlers are terminated if they exceed this budget.

### 2. Develop (Tight Loop)

```
Edit code → hull dev reloads automatically → agent checks status/errors → agent tests
```

After editing app code, `hull dev` detects the file change and restarts the server. The agent workflow:

1. Check `hull agent status`. Is the server running?
2. If not running, check `hull agent errors`. What went wrong?
3. If running, verify with `hull agent request GET /health`
4. Run `hull agent test` for structured test results

### 3. Database Workflow

```bash
# Create a migration
hull migrate new add_users

# Check current schema
hull agent db schema .

# Query data
hull agent db query "SELECT * FROM users LIMIT 5"
```

Migrations are SQL files in `migrations/` directory, numbered `001_`, `002_`, etc. They run automatically on startup.

### 4. Testing

Write test files as `tests/test_*.lua` or `tests/test_*.js`:

```lua
-- tests/test_app.lua
test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.eq(res.json.status, "ok")
end)

test("POST /tasks creates task", function()
    local res = test.post("/tasks", {
        body = '{"title":"Test task"}',
        headers = { ["Content-Type"] = "application/json" }
    })
    test.eq(res.status, 201)
    test.ok(res.json.id, "should have an id")
end)
```

```javascript
// tests/test_app.js
test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});
```

Tests run in-process with `:memory:` SQLite. No TCP, no file I/O, fast.

## App Patterns

### Minimal API

```lua
-- app.lua
app.manifest({})

app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

app.get("/greet/:name", function(req, res)
    res:json({ greeting = "Hello, " .. req.params.name .. "!" })
end)
```

### CRUD with Database

```lua
-- app.lua
local db       = require("hull.db")
local validate = require("hull.validate")

app.manifest({
    modules = { "hull/db@1", "hull/validate@1" },
})

-- migrations/001_init.sql creates the tasks table

app.get("/tasks", function(_req, res)
    local rows = db.query("SELECT * FROM tasks ORDER BY id")
    res:json(rows)
end)

app.get("/tasks/:id", function(req, res)
    local rows = db.query("SELECT * FROM tasks WHERE id = ?", { req.params.id })
    if #rows == 0 then return res:status(404):json({ error = "not found" }) end
    res:json(rows[1])
end)

app.post("/tasks", function(req, res)
    local body = json.decode(req.body)
    local ok, errors = validate.check(body, {
        title = { required = true, type = "string", min = 1, max = 200 }
    })
    if not ok then return res:status(400):json({ errors = errors }) end

    db.exec("INSERT INTO tasks (title, done) VALUES (?, 0)", { body.title })
    local id = db.last_id()
    res:status(201):json({ id = id, title = body.title, done = 0 })
end)

app.put("/tasks/:id", function(req, res)
    local body = json.decode(req.body)
    local changes = db.exec("UPDATE tasks SET title = ?, done = ? WHERE id = ?",
                            { body.title, body.done, req.params.id })
    if changes == 0 then return res:status(404):json({ error = "not found" }) end
    res:json({ id = tonumber(req.params.id), title = body.title, done = body.done })
end)

app.del("/tasks/:id", function(req, res)
    local changes = db.exec("DELETE FROM tasks WHERE id = ?", { req.params.id })
    if changes == 0 then return res:status(404):json({ error = "not found" }) end
    res:json({ ok = true })
end)
```

### With Authentication

```lua
local db      = require("hull.db")
local crypto  = require("hull.crypto")
local session = require("hull.web.middleware.session")
local auth    = require("hull.web.middleware.auth")

app.manifest({
    modules = {
        "hull/db@1",
        "hull/crypto@1",
        "hull/web/middleware/session@1",
        "hull/web/middleware/auth@1",
    },
})

session.init({ ttl = 3600 })

-- Load session on every request
app.use("*", "/*", auth.session_middleware({ optional = true }))

app.post("/register", function(req, res)
    local body = json.decode(req.body)
    -- hash password, insert user, create session
    local hash = crypto.password_hash(body.password)
    db.exec("INSERT INTO users (email, password_hash) VALUES (?, ?)",
            { body.email, hash })
    local user_id = db.last_id()
    local sid = auth.login(req, res, { user_id = user_id, email = body.email })
    res:status(201):json({ user_id = user_id, session_id = sid })
end)

app.post("/login", function(req, res)
    local body = json.decode(req.body)
    local rows = db.query("SELECT * FROM users WHERE email = ?", { body.email })
    if #rows == 0 then return res:status(401):json({ error = "invalid credentials" }) end
    if not crypto.password_verify(body.password, rows[1].password_hash) then
        return res:status(401):json({ error = "invalid credentials" })
    end
    local sid = auth.login(req, res, { user_id = rows[1].id, email = rows[1].email })
    res:json({ session_id = sid })
end)
```

### With Middleware Stack

```lua
local db          = require("hull.db")
local cors        = require("hull.web.middleware.cors")
local ratelimit   = require("hull.web.middleware.ratelimit")
local logger      = require("hull.web.middleware.logger")
local transaction = require("hull.web.middleware.transaction")

app.manifest({
    modules = {
        "hull/db@1",
        "hull/web/middleware/cors@1",
        "hull/web/middleware/ratelimit@1",
        "hull/web/middleware/logger@1",
        "hull/web/middleware/transaction@1",
    },
})

-- Pre-body middleware (runs before body is read)
app.use("*", "/*", logger.middleware({ skip = {"/health"} }))
app.use("*", "/api/*", ratelimit.middleware({ limit = 60, window = 60 }))
app.use("*", "/api/*", cors.middleware({ origins = { "https://myapp.com" } }))

-- Post-body middleware (runs after body is read)
app.use_post("POST", "/api/*", transaction.middleware())
```

## Manifest & Capabilities

Every app must call `app.manifest({})`. Capabilities AND module imports are declared here:

```lua
app.manifest({
    modules  = {                              -- first-party modules the app uses
        "hull/crypto@1",
        "hull/db@1",
        "hull/web/middleware/session@1",
    },
    fs_read  = { "config/" },                 -- read access to config/ directory
    fs_write = { "uploads/" },                -- write access to uploads/ directory
    env      = { "DATABASE_URL", "SECRET" },  -- environment variable access
    hosts    = { "api.example.com" },         -- outbound HTTP allowlist
})
```

Undeclared capabilities are blocked. An empty manifest `{}` means no filesystem, no env, no outbound HTTP, and no side-effect modules. The intrinsic core (always available without declaration) is **`hull/app`** alone (the registration API: `app.manifest`, `app.get/post/use`, `app.router`, `app.ws/sse`, `app.every/daily`, `app.main`). `hull/log` and `hull/json` are declared modules. Apps that call `log.X` or `json.X` must put `"hull/log@1"` / `"hull/json@1"` in `manifest.modules`.

**Module rules (important for AI agents):**
- Format is `"namespace/name@major"` (e.g. `"hull/crypto@1"`). Major version is required.
- Only first-party modules from the registry are accepted. List them with `hull modules available`.
- Module imports are checked at `require`/`import` time. Undeclared imports fail with an error like `module 'hull/crypto' not declared in manifest. Add it to modules = { ... }`.
- The transitive closure is enforced: if you import `hull/web/middleware/session`, you must also declare `hull/db` and `hull/crypto`. The error message lists exactly which deps to add.
- Capability bits piggy-back: declaring `hull/fs` doesn't grant filesystem access. You still need `fs_read`/`fs_write`. The module declaration only opens the import; the capability declaration grants the data path.
- `hull modules analyze [app_dir]` static-scans your code and reports imports you didn't declare. Run it before `hull build`.
- `hull check` includes the same analysis as a pre-test gate.

## Available Standard Library

Every module except the intrinsic core must be listed in `manifest.modules`. The intrinsic core is just `hull/app` (registration API; bootstrap requirement because the manifest itself uses `app.manifest`). `hull/log` and `hull/json` are declared modules. List them if you call `log.X` or `json.X` directly. The "Declare" column is the literal manifest entry to add.

| Module | Lua Import | JS Import | Declare | Purpose |
|--------|-----------|-----------|---------|---------|
| `app` | `app` (intrinsic, gets methods via declarations) | `import { app } from "hull:app"` | _intrinsic_ | Bootstrap: `app.manifest` + `app.main` only. The rest is decorated by declared modules. |
| `log` | `local log = require("hull.log")` | `import { log } from "hull:log"` | `"hull/log@1"` | Logging |
| `json` | `local json = require("hull.json")` | `import { json } from "hull:json"` (or built-in `JSON`) | `"hull/json@1"` | Encode/decode |
| `db` | `local db = require("hull.db")` | `import { db } from "hull:db"` | `"hull/db@1"` | SQLite queries (in-base default). A `postgres://` / `mysql://` / `duckdb://` DSN needs the matching **composable feature** (`hull feature install postgres\|mysql\|duckdb` → `hull build --with=postgres\|mysql\|duckdb`, native-only) |
| `crypto` | `local crypto = require("hull.crypto")` | `import { crypto } from "hull:crypto"` | `"hull/crypto@1"` | Hashing, signing, random |
| `time` | `local time = require("hull.time")` | `import { time } from "hull:time"` | `"hull/time@1"` | Timestamps |
| `env` | `local env = require("hull.env")` | `import { env } from "hull:env"` | `"hull/env@1"` | Environment vars (also needs `env` allowlist) |
| `fs` | `local fs = require("hull.fs")` | `import { fs } from "hull:fs"` | `"hull/fs@1"` | Sandboxed FS (also needs `fs_read`/`fs_write`) |
| `http_client` | `local http_client = require("hull.http-client")` | `import { httpClient } from "hull:http-client"` | `"hull/http-client@1"` | Outbound HTTP/HTTPS (`http.fetch` etc.); needs `hosts` allowlist |
| `http_server` | `local http_server = require("hull.http-server")` | `import { httpServer } from "hull:http-server"` | `"hull/http-server@1"` | Decorates `app` with `get/post/use/router`; also exposes `server.stats()` |
| `ws_server` | `local ws_server = require("hull.web.ws-server")` | `import { wsServer } from "hull:web:ws-server"` | `"hull/web/ws-server@1"` | Decorates `app.ws`; also exposes `wsServer.broadcast`, `wsServer.connections` |
| `ws_client` | `local ws_client = require("hull.web.ws-client")` | `import { wsClient } from "hull:web:ws-client"` | `"hull/web/ws-client@1"` | Outbound WebSocket (`wsClient.connect`); needs `hosts` allowlist |
| `sse` | (no module. `app.sse` decoration only) | (same) | `"hull/web/sse@1"` | Decorates `app.sse` for Server-Sent Events |
| `timers` | (no module. `app.every` / `app.daily` decoration) | (same) | `"hull/timers@1"` | Decorates `app.every` and `app.daily` |
| `gpu` | `require("hull.gpu")` | `import { gpu } from "hull:gpu"` | `"hull/gpu@1"` (or `"hull/gpu@1?"` optional) | GPU compute. Manifest `gpu = true`. Ships as the **`gpu` composable feature** (`hull feature install gpu` → `hull build --with=gpu`, native-only) or `HL_ENABLE_GPU=1`. Use the `?` suffix + a `gpu.available()` check to fall back to CPU when absent |
| `compute` | `require("hull.compute")` | `import { compute } from "hull:compute"` | `"hull/compute@1"` | WASM compute plugins |
| `image` | `require("hull.image")` | `import { image } from "hull:image"` | `"hull/image@1"` | Image decode/encode |
| `template` | `require("hull.template")` | `import { template } from "hull:template"` | `"hull/template@1"` | HTML templates |
| `validate` | `require("hull.validate")` | `import { validate } from "hull:validate"` | `"hull/validate@1"` | Input validation |
| `form` | `require("hull.web.form")` | `import { form } from "hull:web:form"` | `"hull/web/form@1"` | Form parsing |
| `cookie` | `require("hull.web.cookie")` | `import { cookie } from "hull:web:cookie"` | `"hull/web/cookie@1"` | Cookie helpers |
| `jwt` | `require("hull.jwt")` | `import { jwt } from "hull:jwt"` | `"hull/jwt@1"` | JWT sign/verify (deps: `crypto`) |
| `csv` | `require("hull.csv")` | `import { csv } from "hull:csv"` | `"hull/csv@1"` | CSV parse/encode |
| `search` | `require("hull.search")` | `import { search } from "hull:search"` | `"hull/search@1"` | FTS5 search (deps: `db`) |
| `rbac` | `require("hull.web.middleware.rbac")` | `import { rbac } from "hull:web:middleware:rbac"` | `"hull/web/middleware/rbac@1"` | RBAC (deps: `db`) |
| `i18n` | `require("hull.i18n")` | `import { i18n } from "hull:i18n"` | `"hull/i18n@1"` | Translations |
| `session` | `require("hull.web.middleware.session")` | `import { session } from "hull:web:middleware:session"` | `"hull/web/middleware/session@1"` | Sessions (deps: `db`, `crypto`) |
| `auth` | `require("hull.web.middleware.auth")` | `import { auth } from "hull:web:middleware:auth"` | `"hull/web/middleware/auth@1"` | Auth middleware |
| `csrf` | `require("hull.web.middleware.csrf")` | `import { csrf } from "hull:web:middleware:csrf"` | `"hull/web/middleware/csrf@1"` | CSRF (deps: `crypto`) |
| `cors` | `require("hull.web.middleware.cors")` | `import { cors } from "hull:web:middleware:cors"` | `"hull/web/middleware/cors@1"` | CORS headers |
| `ratelimit` | `require("hull.web.middleware.ratelimit")` | `import { ratelimit } from "hull:web:middleware:ratelimit"` | `"hull/web/middleware/ratelimit@1"` | Rate limiting |
| `logger` | `require("hull.web.middleware.logger")` | `import { logger } from "hull:web:middleware:logger"` | `"hull/web/middleware/logger@1"` | Request logging |
| `transaction` | `require("hull.web.middleware.transaction")` | `import { transaction } from "hull:web:middleware:transaction"` | `"hull/web/middleware/transaction@1"` | DB transactions (deps: `db`) |
| `idempotency` | `require("hull.web.middleware.idempotency")` | `import { idempotency } from "hull:web:middleware:idempotency"` | `"hull/web/middleware/idempotency@1"` | Idempotency keys (deps: `db`, `crypto`) |
| `outbox` | `require("hull.web.middleware.outbox")` | `import { outbox } from "hull:web:middleware:outbox"` | `"hull/web/middleware/outbox@1"` | Transactional outbox (deps: `db`, `http`) |
| `inbox` | `require("hull.web.middleware.inbox")` | `import { inbox } from "hull:web:middleware:inbox"` | `"hull/web/middleware/inbox@1"` | Inbox dedup (deps: `db`) |
| `health` | `require("hull.web.middleware.health")` | `import { health } from "hull:web:middleware:health"` | `"hull/web/middleware/health@1"` | Health/readiness endpoints |
| `etag` | `require("hull.web.middleware.etag")` | `import { etag } from "hull:web:middleware:etag"` | `"hull/web/middleware/etag@1"` | ETag helpers (deps: `crypto`) |

To get the authoritative list for your hull build (including deps and capability requirements), run `hull modules available --json`. For a single module's spec, `hull modules explain hull/web/middleware/session`.

**Modules that require an HTTP-enabled build** (i.e. `HL_ENABLE_HTTP_SERVER=1` /
`HL_ENABLE_HTTP_CLIENT=1`, both default on): `hull/http-server`,
`hull/http-client`, `hull/web/ws-server`, `hull/web/ws-client`, `hull/web/sse`,
`hull/smtp`, `hull/email`, and every entry under `hull/web/middleware/*` (auth,
cors, csrf, etag, health, idempotency, inbox, logger, outbox, ratelimit, rbac,
session, transaction). On a CLI-only build (`HL_ENABLE_HTTP=0`, the back-compat
alias that pins both halves off, see [docs/cli_mode.md](docs/cli_mode.md))
declaring an inbound module fails at resolve time with
`requires HL_ENABLE_HTTP_SERVER (build-time)` (or `_CLIENT` for outbound ones).
`hull doctor` reports which subsystems your binary supports. On the distributed
SLIM base these subsystems are dropped and auto-composed back at `hull build`
only when the app declares an HTTP module (nothing to install or flag).

## Database API

```lua
-- Query (returns array of row objects)
local rows = db.query("SELECT * FROM tasks WHERE done = ?", { 0 })
-- rows = { { id = 1, title = "Buy milk", done = 0 }, ... }

-- Execute (returns number of changed rows)
local changes = db.exec("UPDATE tasks SET done = 1 WHERE id = ?", { 42 })

-- Last inserted row ID
local id = db.last_id()

-- Transaction (atomic batch)
db.batch(function()
    db.exec("INSERT INTO events (type) VALUES (?)", { "created" })
    db.exec("INSERT INTO outbox (destination) VALUES (?)", { "https://hook.example.com" })
end)
-- Both inserts succeed or both roll back
```

All queries are parameterized (no SQL injection possible). SQLite is in WAL mode with prepared statement caching. Tables prefixed with `_hull_` are reserved for internal middleware and cannot be accessed via `db.exec()`/`db.query()`. Attempts will raise an error.

### User-Defined Functions (UDFs)

Register Lua/JS callbacks or WASM modules as SQL functions:

```lua
-- Scalar UDF (called per row)
db.udf.register("hull_upper", function(text)
    if not text then return nil end
    return string.upper(text)
end, { deterministic = true })

-- Aggregate UDF (called across rows with GROUP BY)
db.udf.register("hull_avg_price", {
    step = function(ctx, val)
        ctx.sum = (ctx.sum or 0) + val
        ctx.count = (ctx.count or 0) + 1
    end,
    finalize = function(ctx)
        if not ctx.count or ctx.count == 0 then return nil end
        return ctx.sum / ctx.count
    end,
}, { aggregate = true })

-- WASM-backed UDF (string arg = module name in compute/)
db.udf.register("hull_score", "scoring", { deterministic = true, gas = 100000 })

-- Use in queries like any SQL function
db.query("SELECT hull_upper(name) FROM products")
db.query("SELECT category, hull_avg_price(price) FROM products GROUP BY category")

-- Remove a UDF
db.udf.unregister("hull_upper")
```

Names must start with `hull_` to prevent shadowing SQLite built-ins.

## Response API

```lua
-- JSON response
res:json({ key = "value" })

-- Status + JSON
res:status(201):json({ id = 42 })

-- Text response
res:text("Hello")

-- HTML response
res:html("<h1>Hello</h1>")

-- Set headers
res:header("X-Custom", "value")

-- Redirect
res:status(302):header("Location", "/new-path"):text("")

-- Template rendering
local template = require("hull.template")
res:html(template.render("pages/home.html", { title = "Home" }))
```

## Request Object

```lua
req.method          -- "GET", "POST", etc.
req.path            -- "/tasks/42"
req.query           -- "page=2&limit=10" (raw query string)
req.body            -- request body (string, available after body is read)
req.params.id       -- route parameter from "/tasks/:id"
req.headers["content-type"]  -- request header (lowercase keys)
req.header("Content-Type")   -- case-insensitive header lookup (Lua function)
req.ctx             -- middleware context table (session, user, csrf_token, etc.)
```

## Common Gotchas

1. **Always call `app.manifest({})`**. Even with no capabilities. Without it, the app won't start.

2. **Lua indexing starts at 1**. `db.query()` returns Lua tables indexed from 1. Empty results = `{}` (empty table, truthy in Lua).

3. **Check empty results with `#rows == 0`**. Not `if not rows` (empty tables are truthy in Lua).

4. **`req.body` is a string**. Always `json.decode(req.body)` for JSON APIs. Always validate before using.

5. **Route parameters are strings**. `req.params.id` is `"42"` not `42`. Use `tonumber()` in Lua or `parseInt()` in JS.

6. **Middleware order matters**. Rate limiting before auth, CORS before auth, CSRF after session.

7. **Post-body middleware**. Use `app.use_post()` for middleware that needs `req.body` (CSRF, idempotency, transaction).

8. **Migrations auto-run**. On `hull dev` startup and `hull test`. Skip with `--no-migrate`.

9. **Template auto-escaping**. `{{ var }}` HTML-escapes. Use `{{{ var }}}` for raw HTML only when safe.

10. **Static files at `/static/*`**. Put files in `static/` directory. They're auto-detected and served.

11. **Module imports must be declared**. `require("hull.crypto")` / `import { crypto } from "hull:crypto"` fails unless `"hull/crypto@1"` is in `manifest.modules = { ... }`. The error message names the missing module. Run `hull modules analyze` before building. The intrinsic core is just `hull/app` (registration API, must be intrinsic because the manifest itself is expressed via `app.manifest`). Everything else, including `hull/log` and `hull/json`, must be declared.

12. **Module deps are transitive**. Declaring `"hull/web/middleware/session@1"` is not enough; you must also declare its dependencies (`hull/db@1`, `hull/crypto@1`). The error tells you exactly which to add. Use `hull modules explain hull/web/middleware/session` to see required deps.

## Audit Logging

Hull logs every capability call as structured JSON to stderr when `--audit` is passed or `HULL_AUDIT=1` is set. This gives agents full visibility into what the app actually does at runtime.

```bash
# Start dev server with audit logging
hull dev --agent --audit app.lua -p 3000

# Or enable via environment variable
HULL_AUDIT=1 hull dev --agent app.lua -p 3000
```

Example audit output (one JSON object per line on stderr):

```
{"ts":"2026-08-05T14:23:01Z","cap":"db.query","sql":"SELECT * FROM tasks WHERE id = ?","nparams":1,"result":0}
{"ts":"2026-08-05T14:23:01Z","cap":"fs.read","path":"uploads/file.txt","bytes":4096}
{"ts":"2026-08-05T14:23:02Z","cap":"http.request","method":"POST","url":"https://api.example.com","status":200,"result":0}
{"ts":"2026-08-05T14:23:02Z","cap":"env.get","name":"DATABASE_URL","result":"ok"}
{"ts":"2026-08-05T14:23:03Z","cap":"smtp.send","host":"smtp.example.com","from":"noreply@app.com","to":"user@example.com","subject":"Welcome","result":0}
{"ts":"2026-08-05T14:23:04Z","cap":"tool.spawn","cmd":"cc","exit_code":0}
```

Every capability module is instrumented: `db.query`, `db.exec`, `fs.read`, `fs.write`, `fs.delete`, `http.request`, `env.get`, `tool.spawn`, `smtp.send`. SQL queries are truncated to 512 bytes. Passwords and secrets are never logged.

**Use cases for agents:**
- **Debugging failed requests:** See exactly which DB queries ran, what files were accessed, which HTTP calls were made
- **Verifying security:** Confirm that capability enforcement is working (denied access shows `"result":"denied"`)
- **Performance investigation:** Identify which capability calls a slow endpoint makes
- **Understanding app behavior:** Trace the full sequence of operations for any request

## Build & Deploy

```bash
# Print hull version
hull version              # hull 0.10.0
hull version --json       # {"version":"0.10.0","runtime":"lua+js","platform":"darwin-arm64","build":"release"}

# Check environment readiness (linker/compiler available, platform embedded)
hull doctor               # human-readable report; exits 0 if hull build is ready
hull doctor --json        # {"version":"0.10.0","platform_embedded":"multi-arch","hull_build":"ready",...}

# Self-update (verifies SHA-256, atomic replace via rename)
hull update               # download + verify + install latest stable release
hull update --check       # check only; print "update available" / "up to date"
hull update --channel=beta  # include pre-releases (reserved; same as stable today)

# Initialize a hull project in-place (idempotent, like git init)
hull init                 # initialize in current directory (Lua)
hull init myapp           # initialize in ./myapp/ (creates dir if needed)
hull init . --runtime js  # initialize with JS runtime

# Build standalone binary (composes app + the exactly-needed subsystems)
hull build myapp/                    # compiler-FREE by default: emits app_registry.o
                                     #   via the object emitter and links. No C compiler needed.
hull build myapp/ --compiler=system  # opt into a system cc (gcc/clang from PATH)
hull build myapp/ --compiler=/path/to/cc  # explicit compiler path

# hull build needs only a LINKER by default. Pick one (or a toolchain-free bundle):
hull build myapp/ --linker=lld       # system/PATH lld
hull build myapp/ --linker=zig       # self-contained zig bundle (hull tools install zig)
hull build myapp/ --linker=lld-static # fully static link (musl floor, hull tools install libc-musl-<arch>)
hull build myapp/ --linker=zig --target=x86_64-linux-musl  # cross-compile via the zig bundle

# The binary is self-contained. Deploy anywhere
./myapp -p 8080 -d /data/app.db

# Verify signatures
hull verify myapp/
hull inspect myapp/
```

### Build flavors

Your app binary is *already* minimal without a flavor: the distributed hull's
base composes back only the subsystems the app uses (see "The produced binary is
tailored to the app" above), so a compute app links zero HTTP/Keel/mbedTLS/SQLite
on its own. `--flavor` is now a thin validation layer on that composable base:

```bash
hull build myapp/ --flavor=full          # the default embedded base (no-op)
hull build myapp/ --flavor=pure-compute  # validate: reject any HTTP/TLS module
hull build myapp/ --flavor=auto          # infer the minimal flavor from declared modules
```

`--flavor=pure-compute` is a preset: it builds on the default composable base and
rejects an app that declares an HTTP/TLS module at build time (a build failure
rather than a runtime surprise). The size win comes from the composable base, not
the flavor, so there is nothing to install: `hull flavor install pure-compute`
reports "preset flavor, nothing to install", and `hull flavor list` shows it as
`preset (default base)`. (Pre-built per-flavor platform libs were removed in Phase
4.3.) `--flavor=auto` reads the app's declared modules and picks the smallest
flavor that still satisfies them. Reference:
[docs/build_flavors.md](docs/build_flavors.md).

### Composable features (`--with=`)

Some large subsystems are **not** in the base binary - they're optional
**features** you bolt on at build time. Where a flavor *slims* the base
(subtractive), a feature *adds* a subsystem (additive), shipped as its own
signed archive `libhull_feature-<name>.a`. Five features today, all
**native-only (no cosmo)**:

- **`duckdb`** - embedded DuckDB OLAP backend, reached via a `duckdb://` DSN on
  `hull.db`.
- **`postgres`** - pure-C PostgreSQL wire backend, reached via a
  `postgres://` / `postgresql://` DSN on `hull.db`.
- **`mysql`** - pure-C MySQL / MariaDB wire backend, reached via a
  `mysql://` / `mariadb://` DSN on `hull.db`.
- **`gpu`** - wgpu-native GPU compute (`gpu.*`).
- **`tui`** - terminal UI subsystem (`hull/tui`; auto-inferred from the manifest).

```bash
hull feature install gpu          # fetch + verify + cache to ~/.hull/feature/
hull feature list                 # not installed / installed, per feature
hull build myapp/ --with=gpu      # compose the feature into the app binary
hull build myapp/ --with=duckdb --compiler=system   # duckdb is C++: needs system cc
```

`--flavor` and `--with` are orthogonal and compose. If an app declares
`"hull/gpu@1"` or uses a `duckdb://` DSN but you build **without** the feature,
the app is rejected (base build) - either compose the feature, or mark the
module optional (below). Same signed trust chain as `hull flavor install`;
`make feature-<name>` builds from source. Reference:
[docs/features_and_flavors.md](docs/features_and_flavors.md).

**Signed builds attest every composed archive.** A `hull build --sign` records
each archive it composes (the runtime, HTTP core, tui bridge, and any `--with`
feature) into `package.sig.gethull.composed`, so `--verify-sig` proves the app is
built from genuine gethull.dev artefacts - not just the base platform lib. This is
automatic; you don't manage it. (On a release-built hull with a real platform key,
a mismatched composed archive makes `--verify-sig` refuse to boot; today's builds
carry the placeholder pubkey, which skips the check.) Design:
[docs/composed_feature_signing.md](docs/composed_feature_signing.md).

### Optional modules - graceful capability fallback

Mark a build-cap module optional with a trailing `?`
(`modules = { "hull/gpu@1?" }`). On a build that lacks the capability
(compiled out, no `--with=` feature), the resolver skips it instead of failing
app load, and `require`/`import` returns nil/null so the app can degrade:

```lua
local gpu = require("hull.gpu")           -- nil on a base binary
if gpu and gpu.available() then use_gpu() else use_cpu() end
```
```javascript
import { gpu } from "hull:gpu";            // null on a base binary
if (gpu && gpu.available()) use_gpu(); else use_cpu();
```

A present optional module works exactly as a normal declaration. Use this when
you want one app binary that exploits a feature when available (a
`--with=gpu` build) but still runs on a base binary.

### Compute Modules (WASM)

Compute modules are pure WASM functions invoked via `compute.call()` /
`compute.async.call()` from Lua/JS. The full developer lifecycle is
exposed as `hull compute` subcommands; an agent typically chains them
like the database migration flow.

```bash
# Scaffold a new module. Writes compute/<name>/{<name>.c, hull_compute.h, test_fixtures.json}.
hull compute new score

# (edit compute/score/score.c. Implement hull_process)

# Compile the module to compute/score.wasm.
hull compute build score

# Run the JSON fixtures in compute/score/test_fixtures.json.
hull compute test score

# Smoke-test that the .wasm actually loads in WAMR.
hull compute check score

# Refresh the per-module hull_compute.h from the canonical version embedded in
# the hull binary (run after upgrading Hull).
hull compute refresh-header score

# Build the whole app. Hull build auto-rebuilds any stale .c -> .wasm,
# AOT-compiles every .wasm if wamrc is present, and embeds both into the
# final binary. --no-build-compute opts out for hermetic CI pipelines that
# ship pre-committed .wasm artifacts.
hull build .
```

Introspection (read-only, JSON):

```bash
hull agent compute .                  # list modules + sizes + AOT presence
hull agent compute-call <name> <file> # one-shot invocation against file input
hull agent deploy .                   # includes compute_modules[] with source_stale flags
hull agent context --task=compute --level=compact  # this doc surface as JSON
```

The runtime warns once per module when it loads as the fast interpreter
(no AOT artifact found). `hull doctor` reports the compute toolchain
status: `wasm_enabled`, `wamrc`, `clang`, `wasm_ld`, `aot_ready`.

### Deployment Config Generator

`hull deploy` generates deployment configs from the app's manifest. It writes files. It never makes network calls or executes the configs.

```bash
# Generate Dockerfile + .dockerignore
hull deploy dockerfile myapp/

# Generate systemd service unit + install script
hull deploy systemd myapp/ --name myapp --user webapp

# Generate fly.toml (+ Dockerfile if missing)
hull deploy fly myapp/ --region lax --memory 512
```

| Target | Output | Consumed by |
|--------|--------|-------------|
| `dockerfile` | `Dockerfile` + `.dockerignore` | `docker build` |
| `systemd` | `deploy/<name>.service` + `deploy/install.sh` | `systemctl` |
| `fly` | `fly.toml` + `Dockerfile` (if missing) | `flyctl deploy` |

Generated configs adapt to the app automatically:
- **FROM scratch** by default (zero attack surface) or `--distroless`
- **CA bundle** only when manifest declares outbound `hosts`
- **ENV declarations** from manifest `env` array
- **VOLUME/mounts** only for database apps (has `migrations/`)
- **systemd hardening**. 17 security directives layered on top of hull's sandbox
- **Signature verification**. Optional `--sign` adds `hull verify` step in Dockerfile

Common flags: `--port N`, `--name NAME`, `-o DIR`, `--force` (overwrite existing).

**Agent workflow:**
```bash
hull agent deploy myapp/           # check deployment readiness (JSON)
hull deploy dockerfile myapp/      # generate Dockerfile
hull deploy systemd myapp/         # generate systemd unit
```

## Release Process

> **Agents should not run this procedure unattended.** Cutting a Hull
> release involves generating / handling the Ed25519 release private
> key, setting GitHub repository secrets, and pushing version tags.
> All three actions are durable, hard to undo, and affect every Hull
> end-user. Treat as a human-driven workflow; agent involvement is
> limited to running diagnostics (`gh run watch`, `hull verify-release`,
> etc.) and drafting release notes.

The release pipeline (`.github/workflows/release.yml`) is fully wired:
it builds `hull-{linux-x86_64,linux-aarch64,darwin-arm64,cosmo}`, signs
`hull.sha256` with the offline release Ed25519 key (loaded from the
`HULL_RELEASE_KEY` repo secret), and publishes a GitHub release.
End-user `hull update` verifies the signature against the public key
embedded as `HL_RELEASE_PUBKEY_HEX` (in `include/hull/release.h`)
before atomically replacing itself via `rename(2)`.

### Maintainer setup (one time)

```bash
mkdir -p ~/.hull/keys && chmod 700 ~/.hull/keys
cd ~/.hull/keys && hull keygen release   # writes release.{pub,key}
```

- `release.key` (128 hex, mode 0600) stays on the maintainer's machine
  plus an offline backup. Never in the repo, never in any log.
- `release.pub` (64 hex) is pasted into
  `include/hull/release.h::HL_RELEASE_PUBKEY_HEX`, committed, pushed.
- `gh secret set HULL_RELEASE_KEY --body "$(cat ~/.hull/keys/release.key)"
  --repo artalis-io/hull` installs the signing key for the workflow.

### Per release

1. Confirm CI green on the commit to be tagged. The release workflow
   re-runs `make`, signs from the freshly built linux-native binary,
   and publishes. Red CI means a red release.
2. `git tag -a vX.Y.Z -m "Hull vX.Y.Z" && git push origin vX.Y.Z`
3. `.github/workflows/release.yml` produces the six release assets
   (four platform binaries + `hull.sha256` + `hull.sha256.sig`).
4. Smoke-test from a clean machine:
   `curl -fsSL https://gethull.dev/install.sh | sh && hull update --check`.

### Agent-callable diagnostics

| Command | Use |
|---------|-----|
| `gh run list --branch main --limit 3` | check CI state before suggesting a tag |
| `gh release view vX.Y.Z` | confirm the release landed with all six assets |
| `hull verify-release hull.sha256 hull.sha256.sig` | offline integrity check of a downloaded release |
| `hull update --check` | end-user view: is a newer release available? |

Threat model, rationale for signing the manifest rather than each
binary, and rotation plan: [docs/release_signing.md](docs/release_signing.md).
The reference procedure (with rationale per step) is the **Release
Process** section in [CLAUDE.md](CLAUDE.md).

## Project Layout Convention

```
myapp/
  app.lua (or app.js)         # entry point
  migrations/
    001_init.sql              # schema migrations
    002_add_users.sql
  templates/
    base.html                 # template inheritance
    pages/home.html
    partials/nav.html
  static/
    style.css                 # served at /static/style.css
    app.js
  tests/
    test_app.lua              # test files
    test_auth.lua
```
