<!-- minimal -->
## Quickstart: web app (server-side, Lua)

Hull serves web apps from a single `app.lua` file with route
registration via `app.get/post/put/delete/use`. Sandboxed runtime,
in-process SQLite, hot reload, signed binary build - one command each.

**For server-rendered apps with partial-page updates (HTMX), prefer
`hull init --profile htmx`.** It ships a complete starter app with
HTMX + Pico classless + per-request CSP nonce + session + CSRF wired
together. Full details: `hull agent context --task=htmx`.

For a minimal hello-world without the HTMX wiring, omit `--profile`:

```bash
hull init myapp --runtime=lua    # scaffold (creates app.lua + tests/ + .gitignore)
cd myapp
hull dev                         # hot-reload dev server on :3000
hull test                        # in-process HTTP harness, no real port
hull build .                     # signed standalone binary in build/
```

### Minimal app

```lua
-- app.lua
app.manifest({
    modules = { "hull/http-server@1", "hull/log@1" },
})

app.get("/hello", function(req, res)
    return res:json({ msg = "hi", method = req.method })
end)
```

Run `hull dev` and `curl http://localhost:3000/hello`. Done.

### Add a route handler

```lua
app.post("/users", function(req, res)
    local body = req:json()             -- auto-parse JSON body
    if not body or not body.name then
        return res:status(400):json({ error = "name required" })
    end
    return res:json({ id = 1, name = body.name })
end)
```

`req.method`, `req.path`, `req.headers`, `req.query`, `req:json()`,
`req:form()`, `req:body()`. `res:json(t)`, `res:status(n)`,
`res:header(k,v)`, `res:redirect(url)`, `res:html(s)`.

<!-- compact -->
## With a database

```lua
-- app.lua
app.manifest({
    modules = { "hull/http-server@1", "hull/db@1", "hull/log@1" },
})

app.get("/users", function(req, res)
    local rows = db.query("SELECT id, name FROM users LIMIT 100")
    return res:json({ users = rows })
end)

app.post("/users", function(req, res)
    local body = req:json()
    local id = db.exec("INSERT INTO users (name) VALUES (?)", { body.name })
    return res:json({ id = id })
end)
```

Create `migrations/001_init.sql`:
```sql
CREATE TABLE users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    created_at INTEGER DEFAULT (unixepoch())
);
```

`hull dev` auto-runs migrations on startup. `db.query()` returns
arrays of row objects. ALWAYS use `?` placeholders - string
concatenation in SQL is a banned anti-pattern; use parameterized
queries.

## Middleware stack (recommended order)

```lua
local logger     = require("hull.web.middleware.logger")
local cors       = require("hull.web.middleware.cors")
local ratelimit  = require("hull.web.middleware.ratelimit")
local auth       = require("hull.web.middleware.auth")

app.use("*", "/*",       logger.middleware({ skip = {"/health"} }))
app.use("*", "/api/*",   ratelimit.middleware({ limit = 60, window = 60 }))
app.use("*", "/api/*",   cors.middleware({ origins = {"https://myapp.com"} }))
app.use("*", "/api/*",   auth.session_middleware({}))
```

Order matters: rate limit BEFORE auth (reject early), CORS BEFORE
auth (preflight has no credentials). See
`hull agent context --task=middleware` for the full stack including
CSRF, transactions, idempotency, outbox.

## Testing

```lua
-- tests/test_users.lua
local t = require("hull.test")

t.case("GET /users returns array", function()
    local res = t.request("GET", "/users")
    t.assert_eq(res.status, 200)
    local body = t.json(res)
    t.assert_type(body.users, "table")
end)
```

`hull test` runs the suite in-process - no real TCP, no real DB
(uses `:memory:`). Migrations apply automatically. Tests run in
isolation per case.

## Build for production

```bash
hull build .                    # → build/<appname>
./build/<appname> -p 8080       # run on :8080
```

The build embeds: app code, templates, static files, migrations,
compute modules (`compute/*.wasm`), GPU shaders (`shaders/*.wgsl`),
signature. Single binary, no external runtime.

For HTTPS, pass `--tls-cert` / `--tls-key`. For more,
`hull agent context --task=build`.

<!-- full -->
## Static files

Drop files in `app_dir/static/`. They serve at `/static/*` with
proper MIME, ETag, 304-handling, and `Cache-Control` set to
`no-cache` in dev and `public, max-age=86400` in built binaries.
No config.

```
myapp/
  app.lua
  static/
    style.css          → /static/style.css
    img/logo.png       → /static/img/logo.png
```

## Templates

Drop HTML files in `app_dir/templates/`. Render with `template.render`:

```lua
app.get("/", function(req, res)
    return res:html(template.render("home.html", {
        title = "Welcome", user = req.ctx.user,
    }))
end)
```

Templates support `{{ var }}` (HTML-escaped), `{{{ var }}}` (raw),
`{% if %}` / `{% for %}` / `{% block %}` / `{% extends %}` /
`{% include %}` / `{# comment #}`. Compiled once per template, cached
in memory. Full reference: `hull agent context --task=templates`.

## WebSockets and SSE

```lua
app.ws("/ws/chat", {
    on_open    = function(conn) log.info("open: " .. conn:id()) end,
    on_message = function(conn, msg) ws.broadcast("/ws/chat", msg) end,
    on_close   = function(conn) log.info("close") end,
})

app.sse("/sse/events", function(req, stream)
    for i = 1, 10 do
        stream:event("tick", tostring(i))
        hull.sleep(1000)
    end
    stream:close()
end)
```

Both run on the event loop. `ws.broadcast(path, msg)` fans out;
`ws.connect(url, handlers)` is the outbound WebSocket client.

## Background timers

```lua
app.every(5000, function() session.cleanup() end)
app.daily("02:00", function() outbox.cleanup(86400 * 30) end)
```

Min interval 100ms. Return `false` to self-cancel. Errors are
logged, the timer keeps running.

## Deploy

```bash
hull deploy dockerfile > Dockerfile
hull deploy systemd > myapp.service
hull deploy fly > fly.toml
```

Each renders from the manifest. For full deploy patterns see
`hull agent context --task=deploy`.

## What you cannot do (by design)

- `require("fs")`, `require("http")`, `io.open`, `os.execute` - the
  sandbox rejects these. Use the capability layer: `fs.read`,
  `http.fetch`, etc., after declaring them in `app.manifest`.
- Direct SQL string concatenation - use `?` parameters always.
- Module imports outside the first-party stdlib - `manifest.modules`
  selects from the canonical registry (`hull modules available`).
- `eval()` / `loadstring()` - disabled at the C level for both
  runtimes.

## Next steps

- `hull agent context --task=auth` - session + JWT auth stack
- `hull agent context --task=middleware` - full middleware reference
- `hull agent context --task=templates` - template engine
- `hull agent context --task=testing` - test patterns + fixtures
- `hull agent context --task=deploy` - production deployment
