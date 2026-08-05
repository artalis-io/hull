# Hull Examples

Example applications demonstrating Hull's capabilities. Each example has both a Lua and JavaScript version with identical behavior.

## Building Hull

```bash
git clone https://github.com/artalis-io/hull.git
cd hull
make                    # build hull binary (both Lua + JS runtimes)
make RUNTIME=lua        # Lua runtime only
make RUNTIME=js         # JS runtime only
make CC=cosmocc         # Cosmopolitan C — cross-platform APE binary
```

The binary is at `build/hull`.

## Running Examples

```bash
# Pattern: hull -p <port> examples/<name>/app.lua
#          hull -p <port> examples/<name>/app.js

./build/hull -p 3000 examples/hello/app.lua
./build/hull -p 3000 examples/hello/app.js
```

Use `-d <path>` to specify a database file (default: `data.db` in the app directory):

```bash
./build/hull -p 3000 -d /tmp/myapp.db examples/rest_api/app.lua
```

## Examples

### hello

Basic routing, query strings, route parameters, request body echo.

```bash
./build/hull -p 3000 examples/hello/app.lua

curl http://localhost:3000/
curl http://localhost:3000/health
curl http://localhost:3000/greet/World
curl -X POST -d 'payload' http://localhost:3000/echo
```

### rest_api

CRUD API for managing tasks — create, read, update, delete with JSON bodies.

```bash
./build/hull -p 3000 examples/rest_api/app.lua

# Create
curl -X POST http://localhost:3000/tasks \
  -H 'Content-Type: application/json' \
  -d '{"title":"Buy milk"}'

# List
curl http://localhost:3000/tasks

# Get one
curl http://localhost:3000/tasks/1

# Update
curl -X PUT http://localhost:3000/tasks/1 \
  -H 'Content-Type: application/json' \
  -d '{"title":"Buy oat milk","done":true}'

# Delete
curl -X DELETE http://localhost:3000/tasks/1
```

### bench_db

SQLite performance benchmark endpoints — read-heavy, write-heavy, batch writes, mixed workloads. Seeds 1000 rows on startup.

```bash
./build/hull -p 3000 examples/bench_db/app.lua

curl http://localhost:3000/health        # baseline (no DB)
curl http://localhost:3000/read          # SELECT 20 rows
curl -X POST http://localhost:3000/write       # single INSERT
curl -X POST http://localhost:3000/write-batch # 10 INSERTs in transaction
curl http://localhost:3000/mixed         # 1 INSERT + 1 SELECT
```

Use with a benchmarking tool:

```bash
wrk -t4 -c100 -d10s http://localhost:3000/read
wrk -t4 -c100 -d10s -s wrk_post.lua http://localhost:3000/write
```

### async_http

Async I/O primitives — async HTTP fetch, async database queries, async sleep, and worker dispatch. Demonstrates how `http.async.get()`, `db.async.query()`, `hull.sleep()`, and `worker.dispatch()` yield the coroutine so the event loop serves other connections while waiting.

```bash
./build/hull -p 3000 --no-sandbox examples/async_http/app.lua

# Async sleep (yields, event loop stays responsive)
curl http://localhost:3000/sleep

# Async HTTP fetch (yields during outbound request)
curl http://localhost:3000/async-fetch

# Async DB query (offloads to thread pool)
curl http://localhost:3000/async-db

# Worker dispatch (runs Lua function on a separate worker thread)
curl http://localhost:3000/worker-dispatch
```

### bench_template

Template rendering performance benchmark — measures variable substitution, loops with conditionals, and full-featured templates (inheritance + includes + filters). Seeds 50 items at startup to isolate template overhead.

```bash
./build/hull -p 3000 examples/bench_template/app.lua

curl http://localhost:3000/health        # baseline (JSON, no template)
curl http://localhost:3000/simple        # variable substitution only
curl http://localhost:3000/loop          # 50-item loop + conditionals
curl http://localhost:3000/full          # inheritance + include + loop + filters
```

Use with a benchmarking tool:

```bash
wrk -t4 -c100 -d10s http://localhost:3000/simple
wrk -t4 -c100 -d10s http://localhost:3000/full
```

### auth

Session-based authentication — register, login, logout, protected routes. Uses `crypto.hash_password` (PBKDF2-SHA256) for password hashing and SQLite-backed sessions.

```bash
./build/hull -p 3000 examples/auth/app.lua

# Register
curl -X POST http://localhost:3000/register \
  -H 'Content-Type: application/json' \
  -d '{"email":"alice@example.com","password":"secret1234","name":"Alice"}'

# Login (save cookie)
curl -X POST http://localhost:3000/login -c cookies.txt \
  -H 'Content-Type: application/json' \
  -d '{"email":"alice@example.com","password":"secret1234"}'

# Protected route (with cookie)
curl http://localhost:3000/me -b cookies.txt

# Without cookie → 401
curl http://localhost:3000/me

# Logout
curl -X POST http://localhost:3000/logout -b cookies.txt
```

### jwt_api

JWT-based authentication — register, login (returns Bearer token), protected routes, token refresh. Stateless alternative to session-based auth.

```bash
./build/hull -p 3000 examples/jwt_api/app.lua

# Register
curl -X POST http://localhost:3000/register \
  -H 'Content-Type: application/json' \
  -d '{"email":"alice@example.com","password":"secret1234","name":"Alice"}'

# Login (get token)
curl -X POST http://localhost:3000/login \
  -H 'Content-Type: application/json' \
  -d '{"email":"alice@example.com","password":"secret1234"}'
# → {"token":"eyJ...","user":{...}}

# Protected route (with token)
curl http://localhost:3000/me \
  -H 'Authorization: Bearer eyJ...'

# Refresh token
curl -X POST http://localhost:3000/refresh \
  -H 'Authorization: Bearer eyJ...'
```

### crud_with_auth

Tasks CRUD API with session-based auth — each user only sees their own tasks. Demonstrates per-user data isolation with foreign key scoping.

```bash
./build/hull -p 3000 examples/crud_with_auth/app.lua

# Register + login
curl -X POST http://localhost:3000/register \
  -H 'Content-Type: application/json' \
  -d '{"email":"alice@example.com","password":"secret1234","name":"Alice"}'

curl -X POST http://localhost:3000/login -c cookies.txt \
  -H 'Content-Type: application/json' \
  -d '{"email":"alice@example.com","password":"secret1234"}'

# CRUD (scoped to logged-in user)
curl -X POST http://localhost:3000/tasks -b cookies.txt \
  -H 'Content-Type: application/json' \
  -d '{"title":"Buy milk"}'

curl http://localhost:3000/tasks -b cookies.txt
curl http://localhost:3000/tasks/1 -b cookies.txt

curl -X PUT http://localhost:3000/tasks/1 -b cookies.txt \
  -H 'Content-Type: application/json' \
  -d '{"title":"Buy oat milk","done":true}'

curl -X DELETE http://localhost:3000/tasks/1 -b cookies.txt
```

### email

Contact-form / email-sending API with SMTP delivery and SQLite email log. Validates input, sends via `smtp.send()`, and logs every attempt (sent or failed) to the database. Configurable via environment variables.

```bash
SMTP_HOST=localhost SMTP_PORT=587 \
./build/hull -p 3000 --no-sandbox examples/email/app.lua

# Send an email
curl -X POST http://localhost:3000/send \
  -H 'Content-Type: application/json' \
  -d '{"to":"bob@example.com","subject":"Hello","body":"Hi Bob!"}'

# List sent emails
curl http://localhost:3000/sent

# Get a single email log entry
curl http://localhost:3000/sent/1
```

### health_etag

Health check and ETag middleware — demonstrates `hull.web.middleware.health` for liveness/readiness endpoints and `hull.web.middleware.etag` for conditional responses with `304 Not Modified`. Custom health checks, automatic ETag generation for JSON/text/HTML responses.

```bash
./build/hull -p 3000 examples/health_etag/app.lua

# Liveness check
curl http://localhost:3000/health

# Readiness check (includes custom checks + DB ping)
curl http://localhost:3000/ready

# JSON with ETag
curl -v http://localhost:3000/api/items
# → ETag: W/"abc123..."

# Conditional request (returns 304 if unchanged)
curl -H 'If-None-Match: W/"abc123..."' http://localhost:3000/api/items

# Text and HTML with ETag
curl http://localhost:3000/api/greeting?name=Hull
curl http://localhost:3000/api/page
```

### middleware

Middleware chaining — request ID generation, request logging, rate limiting (60 req/min on `/api/*`), and CORS headers. Shows how middleware composes.

```bash
./build/hull -p 3000 examples/middleware/app.lua

# Public route (request ID assigned, logged)
curl -v http://localhost:3000/
# → X-Request-ID: 67a1b2c3-1

# API route (rate limited + CORS)
curl -v http://localhost:3000/api/items
# → X-RateLimit-Limit: 60
# → X-RateLimit-Remaining: 59

# CORS preflight
curl -X OPTIONS -H 'Origin: http://localhost:5173' \
  http://localhost:3000/api/items
# → Access-Control-Allow-Origin: http://localhost:5173

# Debug endpoint
curl http://localhost:3000/api/debug
```

### webhooks

Webhook delivery and receipt with HMAC-SHA256 signatures, transactional outbox, idempotency, and inbox deduplication. Register webhook URLs, fire events that deliver to them via the outbox, and receive/verify incoming webhooks.

```bash
./build/hull -p 3000 examples/webhooks/app.lua

# Register a webhook (points back to self for demo)
curl -X POST http://localhost:3000/webhooks \
  -H 'Content-Type: application/json' \
  -d '{"url":"http://127.0.0.1:3000/webhooks/receive","events":"user.created,order.placed"}'

# Fire an event (atomically inserts event + enqueues outbox deliveries)
curl -X POST http://localhost:3000/events \
  -H 'Content-Type: application/json' \
  -H 'Idempotency-Key: evt-123' \
  -d '{"event":"user.created","data":{"user_id":1}}'

# List webhooks, events, outbox stats
curl http://localhost:3000/webhooks
curl http://localhost:3000/events
curl http://localhost:3000/outbox/stats
curl http://localhost:3000/webhooks/1/deliveries
```

### todo

Full-featured todo app with user authentication, CSRF protection, rate limiting, server-side rendering with HTML templates, and English/Hungarian i18n support. Pure HTML forms, no client-side JS.

```bash
./build/hull dev examples/todo/app.lua -d /tmp/todo.db

# Open in browser
open http://localhost:3000
# Register → Login → Manage todos
# Switch language via /lang/hu or /lang/en
```

### chat

WebSocket chat server with SSE event streaming. Demonstrates `app.ws()` for WebSocket endpoints (broadcast, per-connection state), `app.sse()` for Server-Sent Events, and `ws.broadcast()` / `ws.connections()` for connection management.

```bash
./build/hull -p 3000 examples/chat/app.lua

# Health check
curl http://localhost:3000/health

# Check connection count
curl http://localhost:3000/ws/connections

# Connect via WebSocket client (e.g. websocat, wscat)
websocat ws://localhost:3000/ws/chat
# Type messages — they broadcast to all connected clients

# SSE event stream (3 ticks, then closes)
curl http://localhost:3000/sse/events
```

**Key features demonstrated:**
- `app.ws(path, { on_open, on_message, on_close })` — WebSocket endpoint
- `ws.broadcast(path, data)` — broadcast to all connections on a path
- `ws.connections(path)` — count active connections
- `conn:id()`, `conn:send()` — per-connection methods
- `app.sse(path, handler)` — SSE endpoint
- `stream:event(name, data, id)`, `stream:comment()`, `stream:close()`

### timers

Background timers with `app.every()` and `app.daily()`. A repeating timer inserts heartbeat rows into the database every 500ms, and a self-cancelling timer counts to 3 then stops. Demonstrates timer lifecycle, self-cancellation via `return false`, and timer access to the full capability layer (db, time).

```bash
./build/hull -p 3000 examples/timers/app.lua

# Health check
curl http://localhost:3000/health

# Wait a few seconds, then check heartbeats
curl http://localhost:3000/heartbeats

# Heartbeat counts by source (every-timer keeps going, cancel-timer stops at 3)
curl http://localhost:3000/counter
```

### templates

Template engine showcase — demonstrates inheritance, includes, filters, loops, conditionals, HTML auto-escaping, and compiled/cached rendering. Renders pages with a base layout, navigation partial, and user data.

```bash
./build/hull dev examples/templates/app.lua

# Home page (inheritance + includes + loops + filters + escaping)
curl http://localhost:3000/

# About page (different block content, same base layout)
curl http://localhost:3000/about

# JSON endpoint (no template)
curl http://localhost:3000/users
```

### compute

WASM compute plugins — offload CPU-intensive work to sandboxed WASM modules. Demonstrates sync `compute.call()` for fast operations and async `compute.async.call()` for expensive computations that yield to the event loop.

```bash
./build/hull -p 3000 examples/compute/app.lua

# Sync echo (blocks handler, good for fast calls)
curl http://localhost:3000/echo?text=hello

# Sync score (returns a 0-100 score byte)
curl http://localhost:3000/score?text=hello

# Async echo (yields to event loop, other requests served during WASM execution)
curl http://localhost:3000/async-echo?text=hello

curl http://localhost:3000/health
```

#### Sample Compute Modules

Six reference WASM modules ship with the compute example, each with C source, pre-compiled `.wasm`, and test fixtures:

| Module | File | What it does |
|--------|------|-------------|
| `vector_ops` | `compute/vector_ops/` | Cosine similarity between float32 vectors |
| `sort` | `compute/sort/` | In-place quicksort of int32 arrays |
| `hash` | `compute/hash/` | FNV-1a 64-bit hash |
| `json_extract` | `compute/json_extract/` | Extract value by key from flat JSON |
| `scoring` | `compute/scoring/` | Min-max normalization + weighted scoring |
| `text` | `compute/text/` | Levenshtein edit distance |

### gpu_search

GPU-accelerated vector similarity search using WGSL compute shaders. Index embedding vectors on the GPU, then query for nearest neighbors via cosine similarity. Demonstrates `gpu.compile()`, `gpu.buffer()`, `gpu.dispatch()` with uniforms, persistent buffers, and workgroup dispatch.

Requires GPU build: `make fetch-wgpu && make HL_ENABLE_GPU=1`

```bash
./build/hull -p 3000 --no-sandbox examples/gpu_search/app.lua

# Check GPU availability and devices
curl http://localhost:3000/health

# Index 3 vectors of dimension 4
curl -X POST http://localhost:3000/index \
  -d '{"dimensions":4,"vectors":[[1,0,0,0],[0,1,0,0],[0.7,0.7,0,0]]}'

# Search: find top-2 most similar to [0.8, 0.6, 0, 0]
curl -X POST http://localhost:3000/search \
  -d '{"query":[0.8,0.6,0,0],"k":2}'
```

**Key GPU features demonstrated:**
- `gpu.compile(name, wgsl)` — compile WGSL shader once at startup
- `gpu.buffer(name, data)` — persistent GPU buffer for indexed vectors
- `gpu.dispatch(name, opts)` — dispatch with uniforms, workgroups, readback
- `app.manifest({ gpu = true })` — capability declaration

### gpu_pipeline

Multi-stage GPU compute pipeline — chains normalize → weight → reduce into a single GPU command buffer submission. Demonstrates `gpu.pipeline()` with shared named buffers, per-stage uniforms, fire-and-forget mode, and the performance advantage over multiple `gpu.dispatch()` calls (2.4x speedup for 3-stage pipeline).

Requires GPU build: `make fetch-wgpu && make HL_ENABLE_GPU=1`

```bash
./build/hull -p 3000 --no-sandbox examples/gpu_pipeline/app.lua

# Score items through 3-stage GPU pipeline
curl -X POST http://localhost:3000/score \
  -d '{"items":[[80,150,3],[60,200,1],[95,50,5]],"weights":[0.5,0.3,0.2]}'

# Compare pipeline (1 submit) vs 3x dispatch (3 submits)
curl http://localhost:3000/compare
# Returns: { dispatch_3x_ms: 9.5, pipeline_ms: 3.9, speedup: 2.4 }
```

**Key GPU features demonstrated:**
- `gpu.load(name)` — load WGSL from `shaders/<name>.wgsl` (dev iteration)
- `gpu.pipeline(stages, opts)` — multi-stage in single submission
- `gpu.pipeline(stages, { output = false })` — fire-and-forget (in-place update)
- Named buffer sharing across pipeline stages
- `gpu.buffer_copy(src, dst)` — GPU-side buffer copy without CPU roundtrip
- `fs.mmap()` → `gpu.buffer()` — zero-copy disk→GPU data loading

### gpu_texture

GPU texture processing — load images as GPU textures, process with WGSL compute shaders, read back results. Demonstrates `gpu.texture()`, `gpu.texture_read()`, and dispatch with `textures` array.

Requires GPU build: `make fetch-wgpu && make HL_ENABLE_GPU=1`

```bash
./build/hull -p 3000 --no-sandbox examples/gpu_texture/app.lua

curl http://localhost:3000/health
curl http://localhost:3000/process
```

**Key features demonstrated:**
- `image.new(w, h, "rgba8", pixels)` — create image from raw pixels
- `gpu.texture(name, img)` — persistent GPU texture from HlImage
- `gpu.texture_read(name)` — read back texture as HlImage
- `gpu.dispatch()` with `textures` array — sampled + storage textures
- WGSL `texture_2d<f32>` + `texture_storage_2d<rgba8unorm, write>`

### compute_gpu_chain

WASM→GPU zero-copy data flow — WASM preprocesses data, outputs a WasmBuffer, which passes directly to GPU dispatch without copying through a Lua string. Demonstrates the unified buffer protocol for chaining compute backends.

Requires GPU + WASM build: `make fetch-wgpu && make HL_ENABLE_GPU=1`

```bash
./build/hull -p 3000 --no-sandbox examples/compute_gpu_chain/app.lua

# WASM preprocess → GPU double
curl -X POST http://localhost:3000/chain \
  -d '{"values":[1,2,3,4,5,6,7,8]}'
# Returns: [2,4,6,8,10,12,14,16]

# Index via WASM → persistent GPU buffer, then query
curl -X POST http://localhost:3000/index -d '{"values":[10,20,30]}'
curl http://localhost:3000/query
# Returns: [20,40,60] (doubled in-place via fire-and-forget)
```

**Key features demonstrated:**
- `compute.call(name, input, { buffer = true })` → WasmBuffer output
- WasmBuffer passed directly to `gpu.dispatch()` and `gpu.buffer()` (unified buffer protocol)
- `gpu.dispatch({ output = false })` — fire-and-forget in-place update
- WASM + GPU in the same app with `app.manifest({ gpu = true, compute = true })`

### irc_chat

IRC-like encrypted chat server with channels, E2E encryption, and WebSocket real-time messaging. Users register with password (PBKDF2) and receive a Curve25519 keypair. Channel messages are encrypted with XSalsa20-Poly1305 (secretbox) — the server stores and relays ciphertext only.

```bash
./build/hull -p 3000 examples/irc_chat/app.lua

# Register a user (returns keypair)
curl -X POST http://localhost:3000/register \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"secret1234"}'

# Login (save cookie)
curl -X POST http://localhost:3000/login -c cookies.txt \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"secret1234"}'

# List channels
curl http://localhost:3000/channels -b cookies.txt

# Connect via WebSocket (ws://localhost:3000/ws with session cookie)
# Send: {"type":"join","channel":"#general"}
# Send: {"type":"msg","channel":"#general","encrypted":"...","nonce":"..."}
```

**Key features demonstrated:**
- `app.ws("/ws", handlers)` — authenticated WebSocket with JSON protocol
- `crypto.box_keypair()` — Curve25519 key generation per user
- `crypto.secretbox(msg, nonce, key)` — message encryption (XSalsa20-Poly1305)
- `crypto.box(data, nonce, pk, sk)` — key distribution (per-member encryption)
- Session auth + middleware for HTTP and WebSocket
- Channel management (create, join, leave, topic, kick, who)
- Encrypted message history (DB stores ciphertext only)
- See `ROADMAP.md` for planned: federation, file transfer, direct messages

### image_processing

Image decode/encode — create images from raw pixels, encode to PNG/JPEG, decode from encoded bytes. Demonstrates the `image` module's codec vtable backed by stb_image.

```bash
./build/hull -p 3000 examples/image_processing/app.lua

curl http://localhost:3000/health
curl http://localhost:3000/create
curl http://localhost:3000/info
```

**Key features demonstrated:**
- `image.new(w, h, "rgba8", pixels)` — create from raw pixel data
- `image.encode(img, "png")` — encode to PNG bytes
- `image.decode(bytes, "png")` — decode from encoded bytes
- `img:width()`, `img:height()`, `img:format()`, `img:size()` — properties

### cors_manifest

CORS via `app.manifest()` configuration — no middleware code needed. Keel registers CORS headers automatically. Also demonstrates `server.stats()` for live connection counts.

```bash
./build/hull -p 3000 examples/cors_manifest/app.lua

# Health check
curl http://localhost:3000/health

# API with CORS (try from allowed origin)
curl -H 'Origin: http://localhost:5173' http://localhost:3000/api/data

# CORS preflight
curl -X OPTIONS -H 'Origin: http://localhost:5173' \
  -H 'Access-Control-Request-Method: POST' \
  http://localhost:3000/api/data

# Create data
curl -X POST http://localhost:3000/api/data \
  -H 'Content-Type: application/json' \
  -d '{"name":"test"}'
```

### udf

User-defined SQL functions — register Lua/JS callbacks as SQL functions callable from queries. Demonstrates scalar UDFs, aggregate UDFs with GROUP BY.

```bash
./build/hull -p 3000 examples/udf/app.lua

# Products with uppercased names (hull_upper scalar UDF)
curl http://localhost:3000/products

# Average price per category (hull_avg_price aggregate UDF)
curl http://localhost:3000/avg-prices
```

**Key features demonstrated:**
- `db.udf.register(name, function, opts)` — Lua/JS scalar UDF
- `db.udf.register(name, {step, finalize}, opts)` — Lua/JS aggregate UDF
- `{ deterministic = true }` — enables SQLite query optimization
- Aggregate UDFs with GROUP BY (per-group state)

### jobs

Durable, DB-backed background job queue (`hull/jobs@1`). Enqueue work from a
request, process it out-of-band with retries, exponential backoff, and a
dead-letter path. Shows both execution models — the in-process `app.every`
poller (`app.lua`/`app.js`) and the dedicated `jobs.run_worker` process
(`worker.lua`/`worker.js`) — plus the ops surface (`jobs.stats`/`dead`/`retry`/
`cleanup`).

```bash
./build/hull -p 3000 examples/jobs/app.lua -d ./jobs.db

curl -X POST localhost:3000/jobs -d '{"type":"send_email","data":{"to":"a@b.c"}}'
curl localhost:3000/jobs/stats          # {"pending":..,"done":..,"dead":..}

# scale out: a dedicated worker against the same DB
hull jobs worker examples/jobs/worker.lua -d ./jobs.db
```

**Key features demonstrated:**
- `jobs.enqueue` / `jobs.handler` / `jobs.default` — enqueue + dispatch
- In-process poller (`app.every`) vs. dedicated worker (`jobs.run_worker`)
- Retry-with-backoff, dead-letter, and the visibility-timeout reaper
- Ops: `jobs.stats` / `jobs.dead` / `jobs.retry` / `jobs.cleanup`
- See [`docs/jobs.md`](../docs/jobs.md) for the full guide

## WASM Compute Developer Tooling

Hull provides `hull compute` commands for creating, building, and testing WASM modules:

```bash
# Scaffold a new module
hull compute new mymodule --lang=c
# Creates: compute/mymodule/mymodule.c, hull_compute.h, test_fixtures.json

# Compile to .wasm
hull compute build mymodule
# Creates: compute/mymodule.wasm

# Run test fixtures
hull compute test mymodule

# Validate module loads in WAMR
hull compute check mymodule
```

#### Streaming I/O

Process data larger than memory through WASM modules in chunks:

```lua
-- Buffer → buffer
local result = compute.stream("compress", large_data, nil, { chunk_size = 65536 })

-- File → file (never fully in memory)
compute.stream("transform", { file = "input.csv" }, { file = "output.json" }, { chunk_size = 65536 })

-- Buffer → callback
compute.stream("compress", data, function(chunk, index, is_last)
    -- handle each output chunk
end)
```

Modules can query chunk metadata via `hull_stream_is_first()`, `hull_stream_is_last()`, `hull_stream_chunk_index()` from the ABI header.

## Testing Examples

### Unit tests (`hull test`)

Each example has both `tests/test_app.lua` and `tests/test_app.js` that run in-process via Hull's built-in test framework — no TCP, no server startup, in-memory SQLite for isolation. Running `hull test` on an example directory discovers and runs tests for both runtimes:

```bash
hull test examples/hello/
hull test examples/rest_api/
hull test examples/bench_db/
hull test examples/auth/
hull test examples/jwt_api/
hull test examples/crud_with_auth/
hull test examples/middleware/
hull test examples/webhooks/
hull test examples/todo/
hull test examples/timers/
hull test examples/cors_manifest/
hull test examples/udf/
hull test examples/chat/
hull test examples/compute/
hull test examples/gpu_texture/
hull test examples/image_processing/
```

The test API:

```lua
-- Register a test
test("description", function()
    -- Dispatch an in-process request (no TCP)
    local res = test.get("/path")
    local res = test.post("/path", { body = '{"key":"value"}', headers = { ... } })

    -- Assertions
    test.eq(res.status, 200)        -- equality
    test.ok(res.json.field)         -- truthiness
    test.err(fn, "pattern")         -- expected error
end)
```

```javascript
// JavaScript test API (test_app.js)
test("description", () => {
    const res = test.get("/path");
    const res = test.post("/path", { body: '{"key":"value"}', headers: { ... } });

    test.eq(res.status, 200);        // equality
    test.ok(res.json.field);         // truthiness
    test.err(() => { throw ... }, "pattern");  // expected error
});
```

`test.get/post/put/delete/patch` return `{ status, body, json }` where `json` is auto-decoded.

**Note:** By default, middleware does not run during `hull test` dispatch — only the matched route handler executes. This means session loading, JWT extraction, rate limiting, and CORS are not active in unit tests. Pass `{ middleware = true }` to test with the full middleware chain:

```lua
local res = test.get("/api/items", { middleware = true })
```

For full middleware coverage without per-request flags, use the e2e tests below.

### E2E tests (shell)

Full integration tests that start real servers and exercise routes via curl, including middleware and cookie flows:

```bash
# Run e2e tests for all examples (both runtimes)
sh tests/e2e_examples.sh

# Single runtime
RUNTIME=lua sh tests/e2e_examples.sh
RUNTIME=js  sh tests/e2e_examples.sh
```

## Lua vs JavaScript

Every example has both `app.lua` and `app.js`. The APIs are identical except for naming conventions:

| | Lua | JavaScript |
|---|---|---|
| Globals | `app`, `db`, `time`, `log`, `json`, `crypto` are auto-injected | Must `import { app } from "hull:app"` etc. |
| Method calls | `res:json(data)` (colon syntax) | `res.json(data)` (dot syntax) |
| Tables/Objects | `{ key = "value" }` | `{ key: "value" }` |
| Stdlib imports | `require("hull.web.middleware.session")` | `import { session } from "hull:web:middleware:session"` |
| Naming | `snake_case` — `hash_password` | `camelCase` — `hashPassword` |
| Arrays | 1-indexed — `rows[1]` | 0-indexed — `rows[0]` |
