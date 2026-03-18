<!-- minimal -->
## Middleware

Middleware functions intercept requests before handlers. Return `0` to continue, `1` to short-circuit.

```lua
-- Lua: pre-body middleware (runs before body is read)
app.use("*", "/*", function(req, res)
    req.ctx.start_time = time.now_ms()
    return 0  -- continue
end)

-- Post-body middleware (runs after body is read)
app.use_post("POST", "/api/*", function(req, res)
    if not req.body then return res.status(400).json({ error = "body required" }) end
    return 0
end)
```

```javascript
// JS
app.use("*", "/*", (req, res) => {
    req.ctx.startTime = time.nowMs();
    return 0;
});
app.usePost("POST", "/api/*", (req, res) => {
    if (!req.body) return res.status(400).json({ error: "body required" });
    return 0;
});
```

<!-- compact -->
## Registration

- **`app.use(method, pattern, fn)`** — pre-body middleware (before request body is read)
- **`app.use_post(method, pattern, fn)`** / `app.usePost(...)` — post-body middleware (after body is read)

**Method matching:** `"*"` matches any method. `"GET"`, `"POST"`, etc. match specific methods.

**Pattern matching:** `"/*"` matches all paths. `"/api/*"` matches paths under `/api/`. Exact paths like `"/health"` match only that path.

## Data Passing via `req.ctx`

Middleware stores data in `req.ctx` for downstream handlers:

```lua
-- Auth middleware sets req.ctx.user
app.use("*", "/api/*", auth.jwt_middleware({ secret = SECRET }))

-- Handler reads it
app.get("/api/profile", function(req, res)
    local user_id = req.ctx.user.user_id
    -- ...
end)
```

## Factory Pattern

All built-in middleware modules follow the factory pattern:

```lua
local mod = require("hull.<module>")
local mw = mod.middleware(opts)   -- returns a function(req, res) -> 0 | 1
app.use("*", "/api/*", mw)
```

## Recommended Stack Order

```lua
-- Pre-body (app.use)
1. logger        -- assign request ID, log request
2. ratelimit     -- reject abusive traffic early
3. cors          -- handle preflight before auth
4. auth          -- session or JWT authentication

-- Post-body (app.use_post)
5. csrf          -- needs body for form token
6. transaction   -- wrap mutations in db transaction
7. idempotency   -- cache POST responses by key
```

Rate limit before auth (save work). CORS before auth (preflight must not require credentials). Scope middleware to specific paths -- don't rate-limit `/health` or static assets.

<!-- full -->
## Custom Middleware Example

```lua
-- Request timing middleware
app.use("*", "/*", function(req, res)
    req.ctx.start_time = time.now_ms()
    return 0
end)

-- API key middleware
app.use("*", "/api/*", function(req, res)
    local key = req.headers["x-api-key"]
    if not key then
        res.status(401).json({ error = "API key required" })
        return 1  -- short-circuit, don't call handler
    end
    local rows = db.query("SELECT * FROM api_keys WHERE key = ?", key)
    if #rows == 0 then
        res.status(403).json({ error = "Invalid API key" })
        return 1
    end
    req.ctx.api_client = rows[1]
    return 0  -- continue to next middleware or handler
end)
```

```javascript
// JS equivalent
app.use("*", "/api/*", (req, res) => {
    const key = req.headers["x-api-key"];
    if (!key) {
        res.status(401).json({ error: "API key required" });
        return 1;
    }
    const rows = db.query("SELECT * FROM api_keys WHERE key = ?", key);
    if (rows.length === 0) {
        res.status(403).json({ error: "Invalid API key" });
        return 1;
    }
    req.ctx.apiClient = rows[0];
    return 0;
});
```

## Full Stack Example

```lua
local cors      = require("hull.middleware.cors")
local ratelimit = require("hull.middleware.ratelimit")
local auth      = require("hull.middleware.auth")
local logger    = require("hull.middleware.logger")
local session   = require("hull.middleware.session")

session.init()

app.use("*", "/*", logger.middleware({ skip = { "/health" } }))
app.use("*", "/api/*", ratelimit.middleware({
    limit = 60, window = 60,
    key = function(req) return req.headers["x-forwarded-for"] or "anon" end
}))
app.use("*", "/api/*", cors.middleware({
    origins = { "https://myapp.com" },
    credentials = true
}))
app.use("*", "/api/*", auth.session_middleware({}))
```

## Important Notes

- Middleware runs in registration order. The first middleware to return `1` stops the chain.
- Pre-body middleware cannot access `req.body`. Use `app.use_post` for body inspection.
- User routes are registered before static file middleware, so your routes always take priority.
- Each middleware call is per-request. Do not store mutable state in closures across requests.
