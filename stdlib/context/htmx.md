<!-- minimal -->
## Task: HTMX hypermedia web app

**Use this profile when:** building a server-rendered web app that needs
partial-page updates (forms, lists, optimistic UI) without adopting a
JS framework. Internal tools, admin consoles, CRUD apps, anything where
the server is the source of truth.

**Scaffold + run in 30 seconds:**

```bash
hull init my-app --profile htmx
cd my-app
make fetch-vendor    # one-time SHA-pinned download of htmx + pico
make dev             # serves :8080
make test            # in-process tests for plain + htmx paths
```

What you get out of the box: per-request CSP nonce, anonymous session,
CSRF on htmx requests, a sample `/todos` endpoint demonstrating
fragment-vs-full-page rendering, both Lua and JS apps, working tests.

**The pattern:** every handler branches on `htmx.is(req)`:

```lua
app.get("/", function(req, res)
    if htmx.is(req) then
        res:html(template.render("partials/foo.html", data))   -- fragment
    else
        res:html(template.render("pages/foo.html", data))      -- full page
    end
end)
```

Plain browsers (and `curl`, search bots, accessibility tools) get the
full page. HTMX-driven elements (via `hx-get`, `hx-post`, etc.) get
only the fragment that changed. Same URL, same handler, both audiences.

<!-- compact -->

## Stack the scaffold wires up

- **HTMX 2.0.9** (vendored at `static/vendor/htmx.min.js`, fetched via
  `make fetch-vendor` with SHA-256 verification, served from
  `/static/vendor/htmx.min.js`).
- **Pico v2.1.1 classless** (`static/vendor/pico.classless.min.css`).
  Semantic-HTML-first; no class names needed. Customize via
  `static/app.css`.
- **`hull/web/htmx@1`** helper module. Request inspection (`is`, `boosted`,
  `current_url`, `target`, `trigger_name`) + response mutation
  (`redirect`, `retarget`, `reswap`, `trigger`, `refresh`,
  `push_url`, `replace_url`, `location`). `trigger` accepts an
  optional `{ timing = "swap" | "settle" }` for the HX-Trigger-
  After-Swap / After-Settle header variants.
- **`hull/web/middleware/csp@1`** with the `htmx` profile. Generates a
  fresh 128-bit nonce per request, exposes as `req.ctx.csp_nonce`,
  sets `script-src 'self' 'nonce-{rand}'; style-src 'self'
  'nonce-{rand}'; style-src-attr 'unsafe-inline'` (the last is the
  Pico concession, allows inline `style="..."` attrs but not
  `<style>` blocks).
- **`hull/web/middleware/csrf@1`**. HMAC-SHA256 tokens bound to session id.
  Reads from `req.ctx.session_id` first (matching the Lua sibling).
- **Anonymous session bootstrap.** A tiny inline middleware creates a
  session on first visit and loads it on subsequent requests. Production
  apps would replace this with `auth.session_middleware` + a real
  login flow.

## API quick reference

```lua
-- Request inspection
htmx.is(req)             -- boolean: HX-Request: true
htmx.boosted(req)        -- boolean: HX-Boosted: true
htmx.current_url(req)    -- the source page URL or nil
htmx.target(req)         -- the htmx target element id or nil
htmx.trigger_name(req)   -- the trigger element name or nil

-- Response mutation
htmx.redirect(req, res, "/path")        -- dual-mode: HX-Redirect or 302
htmx.retarget(res, "#errors")           -- HX-Retarget
htmx.reswap(res, "outerHTML")           -- HX-Reswap
htmx.trigger(res, "saved", { id = 1 }) -- HX-Trigger (event + payload)
htmx.trigger(res, "settled", nil, { timing = "swap" })   -- HX-Trigger-After-Swap
htmx.trigger(res, "done",    nil, { timing = "settle" }) -- HX-Trigger-After-Settle
htmx.refresh(res)                       -- HX-Refresh: true
htmx.push_url(res, "/items/42")         -- HX-Push-Url
htmx.location(res, "/dashboard")        -- HX-Location
```

JS naming: snake_case becomes camelCase (`htmx.pushUrl`,
`htmx.currentUrl`). All other semantics identical.

## Patterns

### Validation errors as fragments

Send a fragment with `htmx.retarget` so the error lands in the form's
own container, not the swap target:

```lua
if title == "" then
    htmx.retarget(res, "#new-todo")
    res:html('<p id="new-todo" role="alert">Title cannot be empty.</p>')
    return
end
```

### Compose multiple fragments

The response body is just HTML; concatenate fragments:

```lua
res:html(template.render("partials/todo_row.html", data)
    .. template.render("partials/todo_form.html",
        { csrf_token = req.ctx.csrf_token }))
```

### Delete with confirm

```html
<button hx-delete="/todos/{{ id }}"
        hx-target="#todo-{{ id }}"
        hx-swap="delete"
        hx-confirm="Delete this todo?">×</button>
```

Handler returns empty body; `hx-swap="delete"` removes the element.

<!-- full -->

## Cross-runtime parity

| Lua | JS | Note |
|---|---|---|
| `htmx.is(req)` | `htmx.is(req)` | identical |
| `htmx.trigger(res, e, p, opts)` | `htmx.trigger(res, e, p, opts)` | `opts.timing = "swap" | "settle"` for after-swap / after-settle |
| `app.use_post(...)` | `app.usePost(...)` | same convention |
| `require("hull.X")` | `import { X } from "hull:X"` | `.` vs `:` separator |
| `req.ctx.csp_nonce` | `req.ctx.csp_nonce` | snake_case in both (aligned v0.1.8) |
| `req.ctx.csrf_token` | `req.ctx.csrf_token` | snake_case in both |

Templates use snake_case literals (`{{ csp_nonce }}`, `{{ csrf_token }}`)
in BOTH runtimes — handlers pass `csp_nonce = req.ctx.csp_nonce` (Lua)
or `csp_nonce: req.ctx.csp_nonce` (JS). Same template HTML files work
for both apps.

## Module declarations (what to put in app.manifest.modules)

The scaffold pre-fills these; reference for custom apps:

```lua
modules = {
    "hull/http-server@1",            -- enables app.get / app.use / etc.
    "hull/web/htmx@1",                   -- the response-header helpers
    "hull/web/middleware/csp@1",         -- nonce + CSP header
    "hull/web/middleware/csrf@1",        -- HMAC token verification
    "hull/web/middleware/session@1",     -- session storage
    "hull/web/cookie@1",                 -- cookie parse / serialize
    "hull/template@1",               -- HTML template engine
    "hull/web/form@1",                   -- url-encoded body parsing
    "hull/db@1",                     -- SQLite for app state
    "hull/log@1",
}
```

## Testing

Critical: `test.get/post(path, opts)` takes opts as second arg with
`opts.body`, `opts.headers`, `opts.middleware`. **Always pass
`middleware = true`** so CSP/session/CSRF actually run:

```lua
local home = test.get("/", { middleware = true })
local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')

local res = test.post("/todos", {
    middleware = true,
    body = "title=buy+milk&_csrf=" .. token,
    headers = {
        ["hx-request"] = "true",
        ["content-type"] = "application/x-www-form-urlencoded",
        ["cookie"] = home.headers["set-cookie"] or "",
        ["x-csrf-token"] = token,
    },
})
```

JS: same shape with `await` and JS keys.

## Production hardening

The scaffold ships dev-friendly defaults. Before deploying:

1. **Cookie `Secure` flag.** The scaffold sets `{ secure = false }`
   on the session cookie so `hull dev` (plain HTTP `:8080`) works
   in a real browser. For production over HTTPS, remove the opt
   (`cookie.serialize` defaults to `secure = true`).
2. **CSRF secret.** Replace the literal `CHANGE-ME-IN-PRODUCTION`
   in `CSRF_SECRET` with a real high-entropy value loaded from env.
   The scaffold logs a one-time startup warning if it's still the
   placeholder.
3. **Anonymous session bootstrap.** Real apps replace this with
   `auth.session_middleware({ optional = true })` + a login flow
   (see `auth` task context for the canonical pattern).

## See also

- `docs/htmx.md` — long-form pattern guide with flash messages,
  empty states, `hx-swap-oob`, the security model rationale.
- `examples/hypermedia_todo/` — the canonical scaffolded app, both
  runtimes, runs under `hull test`.
- `auth` task context — for replacing the anonymous session
  bootstrap with a real login flow.
- `templates` task context — template syntax, inheritance, filters.
- `testing` task context — full test framework reference.
- HTMX upstream: <https://htmx.org/docs/>.
- Pico v2 docs: <https://picocss.com/docs/>.
