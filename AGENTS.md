# Hull — Agent Development Guide

Hull is a capability-secure, local-first runtime for programmable tools and workflows. It provides structured JSON interfaces for AI coding agents, but also works for human developers and automated services. This document covers everything an agent needs to build, test, debug, and deploy Hull applications.

## Quick Start

```bash
# Create a new project
hull new myapp && cd myapp

# Start development server (hot-reload)
hull dev --agent app.lua -p 3000

# In another terminal — introspect the app
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

WASM compute plugins provide a sandboxed data-plane layer for CPU-intensive computation. Plugins are pure functions (no I/O) that run in isolated WASM linear memory with gas metering. Place `.wasm` files in `compute/`, call via `compute.call("name", input)` (sync) or `compute.async.call("name", input)` (async, yields to event loop) from Lua/JS. `compute.stream("name", input, output, opts)` processes data larger than memory in chunks via a persistent instance. `hull build` auto-compiles to AOT when `wamrc` is available (~1.2x native speed vs ~54x for fast interpreter). See `docs/wamr_architecture.md`.

WASM modules can also be registered as SQL UDFs via `db.udf.register("hull_name", "module_name", opts)` — the WASM function is called per row during query execution with gas metering.

GPU compute shaders (optional, `HL_ENABLE_GPU=1`) provide massively parallel data processing via wgpu-native (Metal/Vulkan/DX12). Compile shaders inline with `gpu.compile("name", wgsl)` or load from files with `gpu.load("name")` (reads `shaders/<name>.wgsl`). Dispatch with `gpu.dispatch("name", opts)` or chain multiple shaders with `gpu.pipeline(stages, opts)` for single-submission execution. Persistent buffers (`gpu.buffer()`) keep data on GPU across requests. Persistent textures (`gpu.texture(name, img)`) accept HlImage objects and can be read back via `gpu.texture_read(name)`. Dispatch supports `textures` array for sampled and storage texture bindings. Fire-and-forget mode (`output = false`) updates GPU buffers in-place without readback. `gpu.buffer_copy()` copies between GPU buffers without CPU roundtrip. GPU dispatches time out after 5 seconds (`HL_GPU_TIMEOUT_MS`).

Both WASM and GPU compute accept the same input types via the unified buffer protocol: strings, `MappedBuffer` (from `fs.mmap()`), and `WasmBuffer` (from `compute.call({buffer=true})`). This enables zero-copy data flow between disk, WASM, and GPU without Lua/JS string intermediaries. Declare `gpu: true` and/or `compute: true` in manifest.

Each app is a single file (`app.lua` or `app.js`) with optional:
- `migrations/*.sql` — database schema (auto-run on startup)
- `templates/*.html` — server-side templates
- `static/*` — served at `/static/*`
- `compute/*.wasm` — WASM compute plugins (auto-AOT compiled during `hull build`)
- `shaders/*.wgsl` — GPU compute shaders (embedded by `hull build`, loaded via `gpu.load()`)
- `tests/test_*.lua` or `tests/test_*.js` — test files

## Runtime Selection

Hull supports two runtimes — selected by file extension:

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

Analyze deployment readiness — manifest, directory structure, existing configs.

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
    "No signature — run hull build --sign for verified deployments"
  ]
}
```

## Development Workflow

### 1. Start Dev Server

```bash
hull dev --agent --audit app.lua -p 3000
```

The `--agent` flag enables:
- `.hull/dev.json` — written on start (port, PID, timestamp), removed on stop
- `.hull/last_error.json` — written on app load failure, cleared on success

The `--audit` flag enables:
- Structured JSON logging of every capability call (db, fs, http, env, tool, smtp) to stderr
- Zero overhead when disabled — single branch on a global flag

The `--max-instructions N` flag (or `HULL_MAX_INSTRUCTIONS` env var) overrides the per-request instruction limit (default: 100M). Both Lua and JS handlers are terminated if they exceed this budget.

### 2. Develop (Tight Loop)

```
Edit code → hull dev reloads automatically → agent checks status/errors → agent tests
```

After editing app code, `hull dev` detects the file change and restarts the server. The agent workflow:

1. Check `hull agent status` — is the server running?
2. If not running, check `hull agent errors` — what went wrong?
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

Tests run in-process with `:memory:` SQLite — no TCP, no file I/O, fast.

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
local validate = require("hull.validate")
app.manifest({})

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
local session = require("hull.middleware.session")
local auth    = require("hull.middleware.auth")

app.manifest({})

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
local cors        = require("hull.middleware.cors")
local ratelimit   = require("hull.middleware.ratelimit")
local logger      = require("hull.middleware.logger")
local transaction = require("hull.middleware.transaction")

app.manifest({})

-- Pre-body middleware (runs before body is read)
app.use("*", "/*", logger.middleware({ skip = {"/health"} }))
app.use("*", "/api/*", ratelimit.middleware({ limit = 60, window = 60 }))
app.use("*", "/api/*", cors.middleware({ origins = { "https://myapp.com" } }))

-- Post-body middleware (runs after body is read)
app.use_post("POST", "/api/*", transaction.middleware())
```

## Manifest & Capabilities

Every app must call `app.manifest({})`. Capabilities are declared here:

```lua
app.manifest({
    fs_read  = { "config/" },              -- read access to config/ directory
    fs_write = { "uploads/" },             -- write access to uploads/ directory
    env      = { "DATABASE_URL", "SECRET" }, -- environment variable access
    hosts    = { "api.example.com" },       -- outbound HTTP allowlist
})
```

Undeclared capabilities are blocked. An empty manifest `{}` means no filesystem, no env, no outbound HTTP.

## Available Standard Library

| Module | Lua Import | JS Import | Purpose |
|--------|-----------|-----------|---------|
| `json` | `json` (global) | built-in `JSON` | Encode/decode |
| `db` | `db` (global) | `import { db } from "hull:db"` | SQLite queries |
| `crypto` | `crypto` (global) | `import { crypto } from "hull:crypto"` | Hashing, signing |
| `time` | `time` (global) | `import { time } from "hull:time"` | Timestamps |
| `log` | `log` (global) | `import { log } from "hull:log"` | Logging |
| `env` | `env` (global) | `import { env } from "hull:env"` | Environment vars |
| `fs` | `fs` (global) | `import { fs } from "hull:fs"` | Sandboxed filesystem |
| `gpu` | `gpu` (global) | `import { gpu } from "hull:gpu"` | GPU compute (wgpu-native, requires `HL_ENABLE_GPU=1`) |
| `compute` | `compute` (global) | `import { compute } from "hull:compute"` | WASM compute plugins |
| `image` | `image` (global) | `import { image } from "hull:image"` | Image decode/encode (stb_image) |
| `http` | `http` (global) | `import { http } from "hull:http"` | HTTP client |
| `validate` | `require("hull.validate")` | `import { validate } from "hull:validate"` | Input validation |
| `session` | `require("hull.middleware.session")` | `import { session } from "hull:middleware:session"` | Server sessions |
| `auth` | `require("hull.middleware.auth")` | `import { auth } from "hull:middleware:auth"` | Authentication |
| `jwt` | `require("hull.jwt")` | `import { jwt } from "hull:jwt"` | JWT sign/verify |
| `csrf` | `require("hull.middleware.csrf")` | `import { csrf } from "hull:middleware:csrf"` | CSRF protection |
| `cors` | `require("hull.middleware.cors")` | `import { cors } from "hull:middleware:cors"` | CORS headers |
| `ratelimit` | `require("hull.middleware.ratelimit")` | `import { ratelimit } from "hull:middleware:ratelimit"` | Rate limiting |
| `logger` | `require("hull.middleware.logger")` | `import { logger } from "hull:middleware:logger"` | Request logging |
| `transaction` | `require("hull.middleware.transaction")` | `import { transaction } from "hull:middleware:transaction"` | DB transactions |
| `idempotency` | `require("hull.middleware.idempotency")` | `import { idempotency } from "hull:middleware:idempotency"` | Idempotency keys |
| `outbox` | `require("hull.middleware.outbox")` | `import { outbox } from "hull:middleware:outbox"` | Transactional outbox |
| `inbox` | `require("hull.middleware.inbox")` | `import { inbox } from "hull:middleware:inbox"` | Inbox dedup |
| `template` | `require("hull.template")` | `import { template } from "hull:template"` | HTML templates |
| `form` | `require("hull.form")` | `import { form } from "hull:form"` | Form parsing |
| `cookie` | `require("hull.cookie")` | `import { cookie } from "hull:cookie"` | Cookie helpers |
| `csv` | `require("hull.csv")` | `import { csv } from "hull:csv"` | CSV parse/encode (RFC 4180) |
| `search` | `require("hull.search")` | `import { search } from "hull:search"` | Full-text search (SQLite FTS5) |
| `rbac` | `require("hull.middleware.rbac")` | `import { rbac } from "hull:middleware:rbac"` | Role-based access control |
| `i18n` | `require("hull.i18n")` | `import { i18n } from "hull:i18n"` | Translations |
| `health` | `require("hull.middleware.health")` | `import { health } from "hull:middleware:health"` | Health check + readiness endpoints |
| `etag` | `require("hull.middleware.etag")` | `import { etag } from "hull:middleware:etag"` | ETag response helpers |

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

All queries are parameterized (no SQL injection possible). SQLite is in WAL mode with prepared statement caching. Tables prefixed with `_hull_` are reserved for internal middleware and cannot be accessed via `db.exec()`/`db.query()` — attempts will raise an error.

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

1. **Always call `app.manifest({})`** — even with no capabilities. Without it, the app won't start.

2. **Lua indexing starts at 1** — `db.query()` returns Lua tables indexed from 1. Empty results = `{}` (empty table, truthy in Lua).

3. **Check empty results with `#rows == 0`** — not `if not rows` (empty tables are truthy in Lua).

4. **`req.body` is a string** — always `json.decode(req.body)` for JSON APIs. Always validate before using.

5. **Route parameters are strings** — `req.params.id` is `"42"` not `42`. Use `tonumber()` in Lua or `parseInt()` in JS.

6. **Middleware order matters** — rate limiting before auth, CORS before auth, CSRF after session.

7. **Post-body middleware** — use `app.use_post()` for middleware that needs `req.body` (CSRF, idempotency, transaction).

8. **Migrations auto-run** — on `hull dev` startup and `hull test`. Skip with `--no-migrate`.

9. **Template auto-escaping** — `{{ var }}` HTML-escapes. Use `{{{ var }}}` for raw HTML only when safe.

10. **Static files at `/static/*`** — put files in `static/` directory. They're auto-detected and served.

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
{"ts":"2026-03-06T14:23:01Z","cap":"db.query","sql":"SELECT * FROM tasks WHERE id = ?","nparams":1,"result":0}
{"ts":"2026-03-06T14:23:01Z","cap":"fs.read","path":"uploads/file.txt","bytes":4096}
{"ts":"2026-03-06T14:23:02Z","cap":"http.request","method":"POST","url":"https://api.example.com","status":200,"result":0}
{"ts":"2026-03-06T14:23:02Z","cap":"env.get","name":"DATABASE_URL","result":"ok"}
{"ts":"2026-03-06T14:23:03Z","cap":"smtp.send","host":"smtp.example.com","from":"noreply@app.com","to":"user@example.com","subject":"Welcome","result":0}
{"ts":"2026-03-06T14:23:04Z","cap":"tool.spawn","cmd":"cc","exit_code":0}
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
hull version              # hull 0.1.0
hull version --json       # {"version":"0.1.0","runtime":"lua+js","platform":"darwin-arm64","build":"release"}

# Build standalone binary (includes app + stdlib + SQLite)
hull build myapp/

# The binary is self-contained — deploy anywhere
./myapp -p 8080 -d /data/app.db

# Verify signatures
hull verify myapp/
hull inspect myapp/
```

### Deployment Config Generator

`hull deploy` generates deployment configs from the app's manifest. It writes files — it never makes network calls or executes the configs.

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
- **systemd hardening** — 17 security directives layered on top of hull's sandbox
- **Signature verification** — optional `--sign` adds `hull verify` step in Dockerfile

Common flags: `--port N`, `--name NAME`, `-o DIR`, `--force` (overwrite existing).

**Agent workflow:**
```bash
hull agent deploy myapp/           # check deployment readiness (JSON)
hull deploy dockerfile myapp/      # generate Dockerfile
hull deploy systemd myapp/         # generate systemd unit
```

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
