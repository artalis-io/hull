# Hull's HTMX hypermedia profile

**Status:** Shipped in v0.1.8.
**Audience:** developers writing server-rendered web apps in Hull who want partial-page updates without adopting a JS framework.

Hypermedia means HTML is the application protocol. Browsers fetch HTML, render it, follow links, submit forms. HTMX (`htmx.org`, v2.x in Hull's profile) extends the HTML vocabulary so any element can issue any HTTP verb and swap the response into the page. The server's job is unchanged: return HTML. There is no JSON state machine, no client-side router, no build step.

Hull's job is to make that pattern productive: a one-shot scaffold, a small helper module for HTMX response headers, a CSP profile that's strict enough to be useful and lax enough that Pico's component styles still work, and worked examples in both Lua and JS.

This doc covers the patterns. For the modules themselves see the module-level doc strings.

---

## Quick start

Scaffold a new app:

```sh
hull init my-app --profile htmx
cd my-app
make fetch-vendor    # one-time: SHA-pinned curl of htmx + pico
make dev             # serves :8080
make test            # in-process tests for plain + htmx paths
```

The scaffold writes a runnable HTMX + Pico app: per-request CSP nonce, anonymous session, CSRF on htmx requests, a sample `/todos` endpoint demonstrating fragment vs full-page rendering, and tests that cover both paths. See `examples/hypermedia_todo/` in the Hull repo for the same output checked in.

---

## The pattern

A hypermedia endpoint returns one of two shapes based on the inbound `HX-Request` header:

```lua
app.get("/", function(req, res)
    if htmx.is(req) then
        -- htmx-driven request: return only the changed fragment
        res:html(template.render("partials/todo_row.html", data))
    else
        -- plain browser navigation: return the full page
        res:html(template.render("pages/home.html", data))
    end
end)
```

`htmx.is(req)` is `req.headers["hx-request"] == "true"`. Set by every htmx-issued AJAX request; not set by normal links / form posts / non-htmx fetch.

The benefit: a plain browser without JS gets the full page. The same URL, hit from an `hx-get="/"` element, gets the fragment. Search bots, screen readers, accessibility tools, and `curl` see the full page. HTMX users see only what changed. One handler, one URL, both audiences.

---

## `hull.web.htmx` API

Pure functions. No state. Both runtimes (snake_case in Lua, camelCase in JS. Lua names below; JS sibling is `currentUrl`/`triggerName`/etc.).

### Request inspection

| Function | Returns |
|---|---|
| `htmx.is(req)` | `true` if `HX-Request: true`. |
| `htmx.boosted(req)` | `true` if the request came via `hx-boost`. |
| `htmx.current_url(req)` | The source page URL the request was made from, or nil. |
| `htmx.target(req)` | The id of the htmx target element, or nil. |
| `htmx.trigger_name(req)` | The name of the htmx trigger element, or nil. |

### Response mutation

| Function | Effect |
|---|---|
| `htmx.redirect(req, res, path)` | Dual-mode: `HX-Redirect` + 204 for htmx; normal 302 for plain. |
| `htmx.retarget(res, selector)` | Sets `HX-Retarget`. Overrides the swap target. |
| `htmx.reswap(res, mode)` | Sets `HX-Reswap`. Overrides the swap mode. |
| `htmx.trigger(res, event, payload?)` | Sets `HX-Trigger`. Fires a client-side event. |
| `htmx.trigger_after_swap(res, ...)` | Same, but after the DOM swap completes. |
| `htmx.trigger_after_settle(res, ...)` | Same, but after the swap settles. |
| `htmx.refresh(res)` | Sets `HX-Refresh: true`. Hard reload. |
| `htmx.push_url(res, url)` | Sets `HX-Push-Url`. Pushes to browser history. |
| `htmx.replace_url(res, url)` | Sets `HX-Replace-Url`. Replaces in browser history. |
| `htmx.location(res, path_or_opts)` | Sets `HX-Location`. Client-side nav without reload. |

---

## CSP integration

The scaffolded app wires `hull/web/middleware/csp@1` with the `htmx` profile:

```
Content-Security-Policy:
  default-src 'self';
  script-src 'self' 'nonce-{rand}';
  style-src 'self' 'nonce-{rand}';
  style-src-attr 'unsafe-inline';      # the Pico concession
  img-src 'self' data:;
  form-action 'self';
  frame-ancestors 'none';
  base-uri 'self'
```

A fresh 128-bit nonce is generated per request and exposed via `req.ctx.csp_nonce`. Templates emit it on `<script>` and `<style>` tags:

```html
<script nonce="{{ csp_nonce }}" src="/static/vendor/htmx.min.js"></script>
<link rel="stylesheet" nonce="{{ csp_nonce }}" href="/static/vendor/pico.classless.min.css">
```

Inline `<script>` and `<style>` blocks without the nonce are blocked. Inline `style="…"` attributes are allowed (this is what `style-src-attr 'unsafe-inline'` permits) because Pico v2 classless uses them on `<details>`, `<dialog>`, and a handful of form controls.

A strictly-no-inline-styles variant is available as `csp.strict()` for apps that don't use Pico.

**Why this combination is genuinely safe even with the `style-src-attr` concession:** see `docs/security.md` §2 "Signature Verification Chain" and the threat-model table in the Hull README. Short version: with this CSP, a compromised same-origin `htmx.min.js` cannot exfiltrate data, load additional code, or use `eval`; its attack surface is restricted to same-origin actions.

---

## CSRF on htmx requests

The scaffold's middleware chain:

```lua
app.use("*", "/*", csp.htmx())
app.use("*", "/*", session_bootstrap)    -- creates/loads an anonymous session
app.use_post("*", "/*", csrf.middleware({ secret = "..." }))
```

On safe methods (GET/HEAD/OPTIONS), CSRF generates a token and attaches it to `req.ctx.csrf_token`. Templates embed it in forms:

```html
<form hx-post="/todos" hx-target="#todos" hx-swap="afterbegin">
  <input type="hidden" name="_csrf" value="{{ csrf_token }}">
  <input type="text" name="title">
  <button type="submit">Add</button>
</form>
```

HTMX automatically sends the form body on `hx-post`, so the CSRF field is included. The middleware verifies on POST/PUT/DELETE/PATCH.

If you'd rather pass the token via the `X-CSRF-Token` header (e.g., for `hx-vals`-driven JSON-body requests), HTMX has an `hx-headers` attribute:

```html
<button hx-post="/todos" hx-headers='{"X-CSRF-Token": "{{ csrf_token }}"}'>Add</button>
```

**Session bootstrap.** CSRF tokens are bound to a session id. The scaffold ships a `session_bootstrap` middleware that creates an anonymous session on first visit (sets a `hull_session` cookie) and loads it on subsequent requests. Apps that already have a login flow (e.g., via `auth.session_middleware`) should use that instead and remove the bootstrap.

**Cross-runtime parity:** as of v0.1.8 the JS `csrf` middleware accepts a `sessionKey` option (default `"session_id"`) and reads `req.ctx[sessionKey]` first, falling back to a cookie parse. This matches the Lua sibling and means session middleware writing `req.ctx.session_id` on the same request is visible to CSRF immediately.

---

## Returning fragments

A POST handler that creates a resource and returns it as an htmx swap fragment:

```lua
app.post("/todos", function(req, res)
    local fields = form.parse(req.body or "")
    local title = (fields.title or ""):gsub("^%s+", ""):gsub("%s+$", "")

    if title == "" then
        -- Validation error: retarget so the error lands in the form's
        -- own container, not the list. The browser SEES the fragment
        -- swap into the right spot.
        htmx.retarget(res, "#new-todo")
        res:html('<p id="new-todo" role="alert">Title cannot be empty.</p>')
        return
    end

    db.exec("INSERT INTO todos (title, done) VALUES (?, 0)", { title })
    local id = db.query("SELECT last_insert_rowid() AS id")[1].id

    if htmx.is(req) then
        -- Compose: the new row goes into the list (the form's
        -- hx-target="#todos" hx-swap="afterbegin"), and a fresh form
        -- replaces itself in place.
        res:html(template.render("partials/todo_row.html",
            { id = id, title = title, done = false })
            .. template.render("partials/todo_form.html",
                { csrf_token = req.ctx.csrf_token }))
    else
        -- Plain form post: full reload.
        res:redirect("/")
    end
end)
```

Things to notice:

- Validation errors are fragments too. `htmx.retarget` redirects the swap to a different container so the error appears in context (not at the top of the list).
- Composing multiple fragments into one response is just string concatenation. HTMX swaps the whole response into the target.
- Form refresh after submit: include a fresh `{% include "partials/todo_form.html" %}` in the response so the form's empty state is restored.

---

## Delete with confirm

```html
<button hx-delete="/todos/{{ id }}"
        hx-target="#todo-{{ id }}"
        hx-swap="delete"
        hx-confirm="Delete this todo?">×</button>
```

The handler returns an empty body; `hx-swap="delete"` removes the target element from the DOM.

```lua
app.delete("/todos/:id", function(req, res)
    local id = tonumber(req.params.id)
    db.exec("DELETE FROM todos WHERE id = ?", { id })
    if htmx.is(req) then
        res:html("")
    else
        res:redirect("/")
    end
end)
```

---

## Flash messages

Flash messages are fragments that appear once and dismiss. The simplest pattern:

```lua
res:html(template.render("partials/flash.html",
    { kind = "ok", message = "Saved." })
    .. template.render("partials/todo_row.html", data))
```

```html
<!-- partials/flash.html -->
<div role="status"
     hx-swap-oob="afterbegin:#flash-zone">
  <p class="{{ kind }}">{{ message }}</p>
</div>
```

`hx-swap-oob` ("out of band") tells HTMX: this element targets `#flash-zone` regardless of the request's main `hx-target`. Auto-dismiss via a small inline script with the page nonce, or via a CSS animation.

---

## Search + debounce

Type-as-you-search is one of HTMX's signature wins. Two attributes do
all the work:

```html
<input type="search" name="q"
       placeholder="Search todos…"
       autocomplete="off"
       hx-get="/search"
       hx-trigger="keyup changed delay:300ms, search"
       hx-target="#todos">
```

What each piece does:

- `hx-trigger="keyup changed delay:300ms, search"` — HTMX waits 300ms
  after the LAST keystroke before firing. The `changed` qualifier
  means the request only goes out if the value actually changed
  (typing then deleting the same chars is a no-op). The second trigger
  `search` catches the browser's native input-clear button.
- `hx-target="#todos"` — the response replaces the `<ul id="todos">`
  inner content. Server returns just the `<li>` rows; no need to
  re-render the wrapper.
- `autocomplete="off"` — the browser's history dropdown is noise here.

Server side: a tiny route that runs `LIKE` and returns the row
fragments. Handle the empty-result case explicitly so the user knows
the query reached the server:

```lua
app.get("/search", function(req, res)
    local q = ((req.query and req.query.q) or "")
                :gsub("^%s+", ""):gsub("%s+$", "")
    local rows = (q == "")
        and db.query("SELECT id, title, done FROM todos "
                  .. "ORDER BY id DESC LIMIT 20")
        or  db.query("SELECT id, title, done FROM todos "
                  .. "WHERE title LIKE ? ORDER BY id DESC LIMIT 20",
                     { "%" .. q .. "%" })
    if htmx.is(req) then
        if #rows == 0 then
            res:html('<li class="muted">No matches.</li>')
        else
            local parts = {}
            for _, row in ipairs(rows) do
                parts[#parts + 1] = template.render(
                    "partials/todo_row.html", { t = row })
            end
            res:html(table.concat(parts))
        end
    else
        res:redirect("/")  -- plain GET fallback
    end
end)
```

JS shape is the same; see `examples/hypermedia_todo/app.js`.

### Rate-limit the search endpoint

Debounce limits CLIENT-side traffic. To bound SERVER-side cost (e.g.
keep typing-bots from blowing up a full-text search) add the existing
ratelimit middleware scoped to `/search`:

```lua
local ratelimit = require("hull.web.middleware.ratelimit")

-- Up to 30 search queries per minute per session. Reject early.
app.use("GET", "/search", ratelimit.middleware({
    limit  = 30,
    window = 60,
    key    = function(req) return req.ctx.session_id or req.headers["x-forwarded-for"] end,
}))
```

The default 429 JSON response works fine for fetch-based clients; for
HTMX you can pair this with §1.5.d-3's planned `htmx_response` option
on the middleware (not yet shipped — until then, returning JSON is
acceptable because HTMX 429s land in `htmx:responseError` for app
handling).

---

## Empty states

Hull's template engine treats empty Lua tables as truthy. To branch on emptiness:

```html
{% if has_items %}
<ul id="todos">
  {% for t in todos %}{% include "partials/todo_row.html" %}{% end %}
</ul>
{% else %}
<p class="empty">No todos yet. Add one above.</p>
{% end %}
```

In the handler:

```lua
local todos = db.query("SELECT id, title, done FROM todos ORDER BY id DESC")
res:html(template.render("pages/home.html", {
    todos = todos,
    has_items = #todos > 0,
    csrf_token = req.ctx.csrf_token,
    csp_nonce = req.ctx.csp_nonce,
}))
```

---

## Testing

Both `test.get` and `test.post` accept an `opts` table as their second argument. Critical: pass `middleware = true` (Lua) / `middleware: true` (JS) so the full middleware chain runs. Without it the test framework dispatches the handler directly and skips CSP, session, CSRF, etc.

```lua
test("GET / returns full HTML page", function()
    local res = test.get("/", { middleware = true })
    test.eq(res.status, 200)
    test.ok(string.find(res.body, "<!doctype html>"))
end)

test("POST /todos with htmx returns fragment", function()
    -- 1. GET / to obtain the CSRF token + session cookie.
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    -- 2. Send the POST with the cookie + token.
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
    test.eq(res.status, 200)
    test.ok(string.find(res.body, "buy milk"))
    test.ok(not string.find(res.body, "<!doctype"))
end)
```

The same shape exists for JS (`await test.get / test.post`, `body:` instead of `body =`, `[k]:` JS keys). See `examples/hypermedia_todo/tests/test_app.{lua,js}` for the full set.

---

## Cross-runtime parity notes

| Lua | JS | Note |
|---|---|---|
| `htmx.is(req)` | `htmx.is(req)` | identical |
| `htmx.trigger_after_swap(...)` | `htmx.triggerAfterSwap(...)` | snake_case ↔ camelCase |
| `app.use_post(...)` | `app.usePost(...)` | same convention |
| `require("hull.X")` | `import { X } from "hull:X"` | `.` vs `:` separator |
| Template `{{ csp_nonce }}` | Template `{{ csp_nonce }}` | snake_case in both; handlers pass `csp_nonce = req.ctx.csp_nonce` (Lua) / `csp_nonce: req.ctx.csp_nonce` (JS) |
| `req.ctx.csp_nonce` | `req.ctx.csp_nonce` | snake_case in both (aligned in v0.1.8) |
| `req.ctx.csrf_token` | `req.ctx.csrf_token` | snake_case in both |

Same template HTML files work for both runtimes; only the handler module changes per-runtime.

---

## See also

- `examples/hypermedia_todo/`. the canonical scaffolded app, both runtimes.
- `stdlib/lua/hull/web/htmx.lua` / `stdlib/js/hull/web/htmx.js`. the helper module source.
- `stdlib/lua/hull/web/middleware/csp.lua` / `stdlib/js/hull/web/middleware/csp.js`. the CSP middleware.
- `stdlib/lua/hull/web/middleware/csrf.lua` / `stdlib/js/hull/web/middleware/csrf.js`. CSRF tokens, HMAC-SHA256.
- `stdlib/lua/hull/template.lua`. template engine, full syntax reference at the top of the file.
- HTMX docs: <https://htmx.org/docs/>.
- Pico v2 docs: <https://picocss.com/docs/>.
- `docs/roadmap_next.md` §1.5.b for the streaming-multipart / attachment work landing in v0.1.9.
