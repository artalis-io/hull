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

### gpu_search

GPU-accelerated vector similarity search using WGSL compute shaders. Index embedding vectors on the GPU, then query for nearest neighbors via cosine similarity. Demonstrates `gpu.compile()`, `gpu.buffer()`, and `gpu.dispatch()` with uniforms, persistent buffers, and workgroup dispatch.

Requires GPU build: `make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu`

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
# Returns: [0.7,0.7,0,0] (score ~0.98) and [1,0,0,0] (score ~0.8)
```

### gpu_pipeline

Multi-stage GPU compute pipeline — chains normalize → weight → reduce into a single GPU command buffer submission. Demonstrates `gpu.pipeline()` with shared named buffers, per-stage uniforms, and the performance advantage over multiple `gpu.dispatch()` calls (2.4x speedup for 3-stage pipeline).

Requires GPU build: `make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu`

```bash
./build/hull -p 3000 --no-sandbox examples/gpu_pipeline/app.lua

# Score items through 3-stage GPU pipeline
curl -X POST http://localhost:3000/score \
  -d '{"items":[[80,150,3],[60,200,1],[95,50,5]],"weights":[0.5,0.3,0.2]}'

# Compare pipeline (1 submit) vs 3x dispatch (3 submits)
curl http://localhost:3000/compare
# Returns: { dispatch_3x_ms: 9.5, pipeline_ms: 3.9, speedup: 2.4 }
```

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
hull test examples/compute/
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

**Note:** Middleware registered via `app.use()` does not run during `hull test` dispatch — only the matched route handler executes. This means session loading, JWT extraction, rate limiting, and CORS are not active in unit tests. For full middleware coverage, use the e2e tests below.

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
| Stdlib imports | `require("hull.middleware.session")` | `import { session } from "hull:middleware:session"` |
| Naming | `snake_case` — `hash_password` | `camelCase` — `hashPassword` |
| Arrays | 1-indexed — `rows[1]` | 0-indexed — `rows[0]` |
