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

Webhook delivery and receipt with HMAC-SHA256 signatures. Register webhook URLs, fire events that deliver to them, and receive/verify incoming webhooks.

```bash
./build/hull -p 3000 examples/webhooks/app.lua

# Register a webhook (points back to self for demo)
curl -X POST http://localhost:3000/webhooks \
  -H 'Content-Type: application/json' \
  -d '{"url":"http://127.0.0.1:3000/webhooks/receive","events":"user.created,order.placed"}'

# Fire an event (delivers to matching webhooks with HMAC signature)
curl -X POST http://localhost:3000/events \
  -H 'Content-Type: application/json' \
  -d '{"event":"user.created","data":{"user_id":1}}'

# List webhooks, events, deliveries
curl http://localhost:3000/webhooks
curl http://localhost:3000/events
curl http://localhost:3000/webhooks/1/deliveries
```

## Testing Examples

### Unit tests (`hull test`)

Each example has a `tests/test_app.lua` that runs in-process via Hull's built-in test framework — no TCP, no server startup, in-memory SQLite for isolation:

```bash
hull test examples/hello/
hull test examples/rest_api/
hull test examples/bench_db/
hull test examples/auth/
hull test examples/jwt_api/
hull test examples/crud_with_auth/
hull test examples/middleware/
hull test examples/webhooks/
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
| Stdlib imports | `require("hull.session")` | `import { session } from "hull:session"` |
| Naming | `snake_case` — `hash_password` | `camelCase` — `hashPassword` |
| Arrays | 1-indexed — `rows[1]` | 0-indexed — `rows[0]` |
