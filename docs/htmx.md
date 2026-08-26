# Hull's HTMX hypermedia profile

**Status:** Shipped in v0.1.8.
**Audience:** developers writing server-rendered web apps in Hull who want partial-page updates without adopting a JS framework.

> **For the widget tier** (toast, confirm, form, search, sort,
> pagination, inline-edit, table) see the separate
> [HTMX widgets guide](htmx_widgets.md). The page below covers the
> broader profile - scaffolding, Pico, CSP nonces, htmx core
> helpers - that the widgets sit on top of.
>
> **Positioning:** the base `hull/web/htmx` module (htmx request
> detection + `HX-*` response headers) is frontend-agnostic core, a peer
> of `csv`/`jwt`/`validate`. The 8 **widgets** are an *optional,
> opinionated component pack* (they emit HTML/CSS/JS and are inert
> without htmx on the page) - opt into them only if you're building
> htmx-first. See the widgets guide's "Positioning" section.

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

The scaffold writes a runnable HTMX + Pico app: per-request CSP nonce, anonymous session, CSRF on htmx requests, a sample `/entries` endpoint demonstrating fragment vs full-page rendering, and tests that cover both paths. See `examples/hypermedia_photos/` in the Hull repo for the same output checked in.

---

## The pattern

A hypermedia endpoint returns one of two shapes based on the inbound `HX-Request` header:

```lua
app.get("/", function(req, res)
    if htmx.is(req) then
        -- htmx-driven request: return only the changed fragment
        res:html(template.render("partials/entry_row.html", data))
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
| `htmx.trigger(res, event, payload?, opts?)` | Sets `HX-Trigger`. Fires a client-side event. `opts.timing = "swap"` → `HX-Trigger-After-Swap`; `opts.timing = "settle"` → `HX-Trigger-After-Settle`. Multi-event form: `htmx.trigger(res, { e1 = p1, e2 = p2 }, opts?)`. |
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
<form hx-post="/entries" hx-target="#entries" hx-swap="afterbegin">
  <input type="hidden" name="_csrf" value="{{ csrf_token }}">
  <input type="text" name="title">
  <button type="submit">Add</button>
</form>
```

HTMX automatically sends the form body on `hx-post`, so the CSRF field is included. The middleware verifies on POST/PUT/DELETE/PATCH.

If you'd rather pass the token via the `X-CSRF-Token` header (e.g., for `hx-vals`-driven JSON-body requests), HTMX has an `hx-headers` attribute:

```html
<button hx-post="/entries" hx-headers='{"X-CSRF-Token": "{{ csrf_token }}"}'>Add</button>
```

**Session bootstrap.** CSRF tokens are bound to a session id. The scaffold ships a `session_bootstrap` middleware that creates an anonymous session on first visit (sets a `hull_session` cookie) and loads it on subsequent requests. Apps that already have a login flow (e.g., via `auth.session_middleware`) should use that instead and remove the bootstrap.

**Cross-runtime parity:** as of v0.1.8 the JS `csrf` middleware accepts a `sessionKey` option (default `"session_id"`) and reads `req.ctx[sessionKey]` first, falling back to a cookie parse. This matches the Lua sibling and means session middleware writing `req.ctx.session_id` on the same request is visible to CSRF immediately.

---

## Returning fragments

A POST handler that creates a resource and returns it as an htmx swap fragment:

```lua
app.post("/entries", function(req, res)
    local fields = form.parse(req.body or "")
    local title = (fields.title or ""):gsub("^%s+", ""):gsub("%s+$", "")

    if title == "" then
        -- Validation error: retarget so the error lands in the form's
        -- own container, not the list. The browser SEES the fragment
        -- swap into the right spot.
        htmx.retarget(res, "#new-entry")
        res:html('<p id="new-entry" role="alert">Title cannot be empty.</p>')
        return
    end

    db.exec("INSERT INTO entries (title, done) VALUES (?, 0)", { title })
    local id = db.query("SELECT last_insert_rowid() AS id")[1].id

    if htmx.is(req) then
        -- Compose: the new row goes into the list (the form's
        -- hx-target="#entries" hx-swap="afterbegin"), and a fresh form
        -- replaces itself in place.
        res:html(template.render("partials/entry_row.html",
            { id = id, title = title, done = false })
            .. template.render("partials/entry_form.html",
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
- Form refresh after submit: include a fresh `{% include "partials/entry_form.html" %}` in the response so the form's empty state is restored.

---

## Delete with confirm

```html
<button hx-delete="/entries/{{ id }}"
        hx-target="#entry-{{ id }}"
        hx-swap="delete"
        hx-confirm="Delete this entry?">×</button>
```

The handler returns an empty body; `hx-swap="delete"` removes the target element from the DOM.

```lua
app.delete("/entries/:id", function(req, res)
    local id = tonumber(req.params.id)
    db.exec("DELETE FROM entries WHERE id = ?", { id })
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
    .. template.render("partials/entry_row.html", data))
```

```html
<!-- partials/flash.html -->
<div role="status"
     hx-swap-oob="afterbegin:#flash-zone">
  <p class="{{ kind }}">{{ message }}</p>
</div>
```

`hx-swap-oob` ("out of band") tells HTMX: this element targets `#flash-zone` regardless of the request's main `hx-target`. Auto-dismiss via a small inline script with the page nonce, or via a CSS animation.

### Composing `flash.trigger` with other `HX-Trigger` events

`flash.trigger(res, text, kind?)` sets `HX-Trigger` directly. A handler that ALSO calls `htmx.trigger(res, "saved")` would overwrite the flash event (last write wins). To fire multiple events in one response, skip `flash.trigger` and call `htmx.trigger` once with a table:

```lua
htmx.trigger(res, {
    saved = { id = 42 },
    flash = { text = "Saved.", kind = "success" },
})
```

The client-side `htmx:on:flash` listener fires the same way; the multi-event payload is HTMX-native.

---

## Search + debounce

Type-as-you-search is one of HTMX's signature wins. Two attributes do
all the work:

```html
<input type="search" name="q"
       placeholder="Search entries…"
       autocomplete="off"
       hx-get="/search"
       hx-trigger="keyup changed delay:300ms, search"
       hx-target="#entries">
```

What each piece does:

- `hx-trigger="keyup changed delay:300ms, search"` - HTMX waits 300ms
  after the LAST keystroke before firing. The `changed` qualifier
  means the request only goes out if the value actually changed
  (typing then deleting the same chars is a no-op). The second trigger
  `search` catches the browser's native input-clear button.
- `hx-target="#entries"` - the response replaces the `<ul id="entries">`
  inner content. Server returns just the `<li>` rows; no need to
  re-render the wrapper.
- `autocomplete="off"` - the browser's history dropdown is noise here.

Server side: a tiny route that runs `LIKE` and returns the row
fragments. Handle the empty-result case explicitly so the user knows
the query reached the server:

```lua
app.get("/search", function(req, res)
    local q = ((req.query and req.query.q) or "")
                :gsub("^%s+", ""):gsub("%s+$", "")
    local rows = (q == "")
        and db.query("SELECT id, title, done FROM entries "
                  .. "ORDER BY id DESC LIMIT 20")
        or  db.query("SELECT id, title, done FROM entries "
                  .. "WHERE title LIKE ? ORDER BY id DESC LIMIT 20",
                     { "%" .. q .. "%" })
    if htmx.is(req) then
        if #rows == 0 then
            res:html('<li class="muted">No matches.</li>')
        else
            local parts = {}
            for _, row in ipairs(rows) do
                parts[#parts + 1] = template.render(
                    "partials/entry_row.html", { t = row })
            end
            res:html(table.concat(parts))
        end
    else
        res:redirect("/")  -- plain GET fallback
    end
end)
```

JS shape is the same; see `examples/hypermedia_photos/app.js`.

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
on the middleware (not yet shipped - until then, returning JSON is
acceptable because HTMX 429s land in `htmx:responseError` for app
handling).

---

## Inline edit (click row → form → save → row)

The canonical CRUD-row UX: click an edit affordance on a row, the row
becomes an inline form, submit replaces the form with the updated
row, cancel restores the original row. Three routes do all the work:

| Verb  | Path                | Returns                          |
|-------|---------------------|----------------------------------|
| GET   | `/entries/:id/edit`   | inline edit form (replaces row) |
| GET   | `/entries/:id`        | the row (used by Cancel)        |
| PATCH | `/entries/:id`        | updated row, or re-rendered edit form on validation error |

**Route registration order matters.** Register the more specific
path (`/edit`) BEFORE the bare `/:id` so the router doesn't greedily
capture `123/edit` as the `:id` param. The example does this; if you
add inline-edit to your own list view, mirror the order.

The Edit button on the row swaps the whole `<li>` with the edit form:

```html
<button hx-get="/entries/{{ t.id }}/edit"
        hx-target="#entry-{{ t.id }}"
        hx-swap="outerHTML"
        aria-label="Edit">✎</button>
```

The edit form posts via `hx-patch` (HTMX wraps the PATCH verb on a
form automatically - the browser still sends POST, but HTMX adds the
right `X-HTTP-Method-Override` semantics):

```html
<li id="entry-{{ t.id }}" class="entry-edit">
  <form hx-patch="/entries/{{ t.id }}"
        hx-target="#entry-{{ t.id }}"
        hx-swap="outerHTML">
    <input type="hidden" name="_csrf" value="{{ csrf_token }}">
    <input type="text" name="title" value="{{ t.title }}" required autofocus>
    <button type="submit">Save</button>
    <button type="button"
            hx-get="/entries/{{ t.id }}"
            hx-target="#entry-{{ t.id }}"
            hx-swap="outerHTML">Cancel</button>
  </form>
</li>
```

Server PATCH handler returns the row on success, OR re-renders the
same edit form with an inline error on validation failure:

```lua
app.patch("/entries/:id", function(req, res)
    local fields = form.parse(req.body or "")
    local title = (fields.title or ""):gsub("^%s+", ""):gsub("%s+$", "")
    if title == "" then
        local existing = db.query(...)[1]
        existing.title = ""  -- keep the (empty) value the user typed
        res:html(template.render("partials/_entry_edit_form.html", {
            t = existing,
            csrf_token = req.ctx.csrf_token,
            error = "Title cannot be empty.",
        }))
        return
    end
    db.exec("UPDATE entries SET title = ? WHERE id = ?", { title, id })
    res:html(template.render("partials/entry_row.html", { t = updated_row }))
end)
```

### Plain-form fallback (Rails-style `_method=PATCH`)

HTMX-only is fine for most internal tools, but if you need to degrade
gracefully to non-HTMX clients (curl, accessibility tools that don't
run JS), expose a POST alias that reads a `_method` override field:

```html
<form method="POST" action="/entries/{{ t.id }}">
  <input type="hidden" name="_method" value="PATCH">
  <input type="hidden" name="_csrf" value="{{ csrf_token }}">
  <input type="text" name="title" value="{{ t.title }}">
  <button>Save</button>
</form>
```

```lua
app.post("/entries/:id", function(req, res)
    local fields = form.parse(req.body or "")
    if fields._method == "PATCH" then
        return patch_todo(req, res, fields)  -- shared handler
    end
    res:status(405)
end)
```

The example is HTMX-only; this is documented for apps that want
progressive enhancement.

### CSRF token freshness

The CSRF middleware's default `max_age = 3600` (1hr) is usually fine
for single-form submits. Inline edit changes the math: a user might
open `/entries/123/edit`, leave it open all afternoon, then submit -
the token in the form has expired.

Two ways to handle:

1. **Bump `max_age` for HTMX apps.** `csrf.middleware({ secret = ...,
   max_age = 4 * 3600 })` (4 hours). Sessions almost certainly out-live
   the token at that point; the binding is still session-scoped.
2. **Re-fetch the edit form before submit.** HTMX `hx-trigger="focus"`
   on the input can re-issue `GET /entries/:id/edit` so the form
   always carries a fresh token. Heavier; usually overkill.

The example ships with the default 1-hour `max_age` (good for the
demo); production deployments with long inline-edit cycles should
pick option 1.

---

## Loading indicator

Every htmx request is a network round-trip. Slow responses without
visual feedback feel broken. HTMX has a built-in mechanism:

1. Mark an element as the indicator with `class="htmx-indicator"`.
2. Reference it via `hx-indicator="#id"` on whichever element fires
   the request (or on a parent - the attribute inherits to children).
3. While a request is in-flight, HTMX adds `class="htmx-request"` to
   the indicator. CSS reveals it.

### Global indicator (the scaffold ships this)

A single spinner in the top-right corner, used for every request:

```html
<body hx-indicator="#spinner">
  <div id="spinner" class="htmx-indicator" aria-hidden="true"></div>
  ...
</body>
```

```css
#spinner {
  position: fixed;
  top: 1rem;
  right: 1rem;
  width: 1.5rem;
  height: 1.5rem;
  border: 0.2rem solid var(--pico-muted-border-color, #ccc);
  border-top-color: var(--pico-primary, #1095c1);
  border-radius: 50%;
  animation: spin 0.8s linear infinite;
  pointer-events: none;
  z-index: 1000;
}
.htmx-indicator {
  opacity: 0;
  transition: opacity 200ms ease-in;
}
.htmx-indicator.htmx-request,
.htmx-request .htmx-indicator {
  opacity: 1;
}
@keyframes spin {
  to { transform: rotate(360deg); }
}
```

The 200ms transition delay means fast (<200ms) responses don't flash
a spinner, which would feel jittery. Slow ones fade in smoothly.

### Per-element indicator

Override the inherited body-level indicator on specific requests
(e.g., a "Save" button that should show inline feedback inside its
own form):

```html
<form hx-patch="/entries/{{ t.id }}"
      hx-target="#entry-{{ t.id }}"
      hx-indicator="#row-saving-{{ t.id }}">
  ...
  <button type="submit">Save</button>
  <span id="row-saving-{{ t.id }}" class="htmx-indicator">saving…</span>
</form>
```

The CSS `.htmx-request .htmx-indicator { opacity: 1 }` rule also
covers the case where the indicator is a child of the triggering
form - htmx adds `htmx-request` to the form during the request.

### Pico's `aria-busy="true"` alternative

Pico v2 has built-in styling for `aria-busy="true"` (renders a small
spinner inline). Apps that prefer Pico's native idiom can wire htmx
events to toggle the attribute via tiny JS:

```html
<button id="save-btn" hx-patch="/entries/42"
        onclick="this.setAttribute('aria-busy','true')">Save</button>
<script nonce="{{ csp_nonce }}">
  document.body.addEventListener("htmx:afterRequest", (e) => {
    if (e.target.id === "save-btn") e.target.removeAttribute("aria-busy");
  });
</script>
```

The `.htmx-indicator` mechanism is simpler when you don't need
button-state changes. Pick whichever fits the UX better.

---

## Form re-population on validation error

The canonical HTMX form pattern: submit → server validates → server
returns the same form fragment with the user's input pre-filled and
per-field error messages inline. The user fixes the error without
losing what they typed.

### The template-driven shape

Form template uses `{{ values.X }}` for value preservation and
`{% if errors.X %}<small role="alert">{{ errors.X }}</small>{% end %}`
for inline errors:

```html
<form id="new-entry" hx-post="/entries" hx-target="#entries" hx-swap="afterbegin">
  <input type="hidden" name="_csrf" value="{{ csrf_token }}">
  <input type="text" name="title" placeholder="New entry"
         value="{{ values.title }}"
         aria-invalid="{% if errors.title %}true{% else %}false{% end %}"
         required autofocus>
  {% if errors.title %}
  <small role="alert" id="new-entry-error">{{ errors.title }}</small>
  {% end %}
  <button type="submit">Add</button>
</form>
```

Handler uses `hull.validate.check` (one declarative call) and on
failure re-renders the same partial with `values` + `errors` in
the data context:

```lua
local validate = require("hull.validate")

app.post("/entries", function(req, res)
    local fields = form.parse(req.body or "")
    local ok, errors = validate.check(fields, {
        title = {
            required = true, trim = true, min = 1, max = 200,
            message = "Title cannot be empty.",
        },
    })
    if not ok then
        htmx.retarget(res, "#new-entry")
        htmx.reswap(res, "outerHTML")
        res:html(template.render("partials/entry_form.html", {
            csrf_token = req.ctx.csrf_token,
            values     = fields,
            errors     = errors,
        }))
        return
    end
    -- success: fields.title is already trimmed by validate
    db.exec("INSERT INTO entries (title, done) VALUES (?, 0)", { fields.title })
    ...
end)
```

`trim = true` on the rule strips whitespace into the SAME `fields`
table in-place, so the post-validate `fields.title` is the cleaned
value - no separate variable needed.

`htmx.retarget(res, "#new-entry") + htmx.reswap(res, "outerHTML")`
overrides the form's own `hx-target="#entries"` so the validation
response replaces the form element itself, not the list.

### Why not just check `if fields.title == ""`?

Could you. But `hull.validate` already handles trim, length bounds,
type coercion, regex, allowlist, email, custom function - all
declaratively. Mixing one schema for one field with hand-rolled
checks for another gets messy fast. Use the validator for everything
and the error block in the template handles each case uniformly.

### Multi-field forms: the `partials/_form_field.html` partial

For forms with more than 1-2 fields, repeating the
`value="{{ values.X }}"` + `aria-invalid` + `<small role="alert">`
block per field gets noisy. The scaffold ships a
`partials/_form_field.html` that bundles all three. Hull templates
can't pass per-call args to `{% include %}`, so use it inside a
for-loop where the loop variable provides each iteration's `field`
context:

```lua
local fields_spec = {
    { name = "email", label = "Email", type = "email",
      required = true, autofocus = true,
      value = req_values.email, error = errors and errors.email },
    { name = "password", label = "Password", type = "password",
      required = true,
      value = req_values.password, error = errors and errors.password },
}
res:html(template.render("pages/signup.html", { fields = fields_spec, ... }))
```

```html
<form hx-post="/signup" hx-target="#signup-form" hx-swap="outerHTML">
  <input type="hidden" name="_csrf" value="{{ csrf_token }}">
  {% for field in fields %}
    {% include "partials/_form_field.html" %}
  {% end %}
  <button type="submit">Sign up</button>
</form>
```

For single-field forms (the entry demo), inline is simpler. For 3+
fields, the for-loop + partial pattern saves a lot of HTML.

---

## Idempotency (double-submit protection)

HTMX form submits are vulnerable to double-clicks: a user mashes the
Save button, two POSTs go out, two rows get inserted. `hx-disabled-elt`
helps at the UI level, but the server-side fix is HTTP-standard
`Idempotency-Key`: client mints a UUID per logical action, server
caches the response keyed by `(principal, key)` and replays it on
retry.

### The scaffold wires it in

`hull init --profile htmx` ships a manifest that declares
`hull/web/middleware/idempotency@1` and calls
`idempotency.init({ ttl = 86400 })` at startup. The middleware is
registered for `POST` and `PATCH`:

```lua
app.use_post("POST",  "/*", idempotency.middleware({
    get_principal = function(req) return req.ctx.session_id or "__anon" end,
}))
app.use_post("PATCH", "/*", idempotency.middleware({
    get_principal = function(req) return req.ctx.session_id or "__anon" end,
}))
```

Without an `Idempotency-Key` header the middleware is a no-op: every
request runs the handler normally. With the header, the middleware
caches the response on first execution and replays it (with
`X-Idempotency-Replay: true`) on retry.

### Handler side: `idempotency.respond_html`

For the cache → replay path to actually work for HTML fragments, the
handler must use `idempotency.respond_html` (or `respond` for JSON)
instead of `res:html(...)` directly:

```lua
app.post("/entries", function(req, res)
    ...
    local html = template.render("partials/entry_row.html", { t = row })
    -- caches HTML + content-type when an Idempotency-Key is in flight,
    -- otherwise just sends the response normally.
    idempotency.respond_html(req, res, 200, html)
end)
```

Why the helper? The middleware needs to know the response status,
body, and content-type to cache. `res:html(...)` alone doesn't reach
the middleware. Without `respond_html` the cache row stays `inflight`
and a retry returns `409 already in progress`.

### Client side: opt in via `hx-headers`

HTMX doesn't send `Idempotency-Key` automatically. Forms that need
double-submit protection opt in:

```html
<form hx-post="/entries"
      hx-headers='{"Idempotency-Key": "{{ random_uuid }}"}'>
  ...
</form>
```

Generate the UUID once per render (server-side, in the template
context). The client sends the same key on every retry of the same
logical action. The scaffold ships the middleware but does NOT add
`hx-headers` by default - apps add it on the forms that need it.

### Semantics

- **Same key + same body** → cached response replayed; handler skipped.
- **Same key + DIFFERENT body** → `409 Conflict` (the key was used
  before with a different request).
- **Same key, still inflight** → `409 already in progress`.
- **No key** → handler runs normally; nothing is cached.

The body fingerprint is `SHA-256(method || path || body)`. Reuse of
a key with a different body is treated as a bug and rejected loudly.

---

## Empty states

Hull's template engine treats empty Lua tables as truthy. To branch on emptiness:

```html
{% if has_items %}
<ul id="entries">
  {% for t in entries %}{% include "partials/entry_row.html" %}{% end %}
</ul>
{% else %}
<p class="empty">No entries yet. Add one above.</p>
{% end %}
```

In the handler:

```lua
local entries = db.query("SELECT id, title, done FROM entries ORDER BY id DESC")
res:html(template.render("pages/home.html", {
    entries = entries,
    has_items = #entries > 0,
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

test("POST /entries with htmx returns fragment", function()
    -- 1. GET / to obtain the CSRF token + session cookie.
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    -- 2. Send the POST with the cookie + token.
    local res = test.post("/entries", {
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

The same shape exists for JS (`await test.get / test.post`, `body:` instead of `body =`, `[k]:` JS keys). See `examples/hypermedia_photos/tests/test_app.{lua,js}` for the full set.

---

## Cross-runtime parity notes

| Lua | JS | Note |
|---|---|---|
| `htmx.is(req)` | `htmx.is(req)` | identical |
| `htmx.trigger(res, e, p, opts)` | `htmx.trigger(res, e, p, opts)` | `opts.timing = "swap" | "settle"` for after-swap / after-settle (replaces the prior `trigger_after_swap` / `triggerAfterSwap` variants) |
| `app.use_post(...)` | `app.usePost(...)` | same convention |
| `require("hull.X")` | `import { X } from "hull:X"` | `.` vs `:` separator |
| Template `{{ csp_nonce }}` | Template `{{ csp_nonce }}` | snake_case in both; handlers pass `csp_nonce = req.ctx.csp_nonce` (Lua) / `csp_nonce: req.ctx.csp_nonce` (JS) |
| `req.ctx.csp_nonce` | `req.ctx.csp_nonce` | snake_case in both (aligned in v0.1.8) |
| `req.ctx.csrf_token` | `req.ctx.csrf_token` | snake_case in both |

Same template HTML files work for both runtimes; only the handler module changes per-runtime.

---

## Photo uploads

Multipart file uploads are the one place where the usual form-encoded
HTMX pattern needs an extra knob: the form has to send
`multipart/form-data` (not `application/x-www-form-urlencoded`), and
the request body is streamed in chunks. Hull's
[`hull/attachment@1`](attachments.md) module + the
`hull/web/attachment-serve@1` HTTP helper handle the server side; the
client side is plain HTMX with one extra attribute.

### Markup

```html
<form hx-post="/entries/{{ t.id }}/photos"
      hx-encoding="multipart/form-data"
      hx-target="#attachments-{{ t.id }}"
      hx-swap="outerHTML">
  <input type="hidden" name="_csrf" value="{{ csrf_token }}">
  <label>
    Attach photo
    <input type="file"
           name="photo"
           accept="image/png,image/jpeg,image/gif,image/webp"
           hx-trigger="change"
           required>
  </label>
</form>
```

Three knobs vs a normal HTMX form:

| Attribute | Why |
|---|---|
| `hx-encoding="multipart/form-data"` | Tells HTMX to send `multipart/form-data` instead of the default `application/x-www-form-urlencoded`. Without it, the file's filename + bytes don't make it through. |
| `hx-trigger="change"` on the `<input type="file">` | Submits as soon as the user picks a file (no separate Submit button needed). Drop this to require an explicit submit. |
| `accept="image/png,..."` | Native browser file-picker filter. **Not authoritative** - server-side `mime_allowlist` is the real gate (clients can spoof Content-Type). The `accept` is just UX so a phone gallery doesn't show 50 PDFs. |

CSRF still has to be wired through. The middleware reads the token
from the `X-CSRF-Token` header OR the `_csrf` form field; for
multipart, the header path is cleaner. HTMX 2.x sets the header
automatically when `meta name="htmx-config"` configures it; or just
include the hidden input as above (csrf middleware checks both).

### Server side

Inside the handler, read the multipart iterator and pass each file
part to `attachment.store`:

```lua
app.post("/entries/:id/photos", function(req, res)
    local entry_id = tonumber(req.params.id)
    local new_ids = {}
    local ok, err = pcall(function()
        for part in req:multipart() do
            if part.filename then
                local id = attachment.store(part, {
                    uploaded_by = req.ctx.session_id,
                })
                db.exec(
                    "INSERT INTO entry_attachments (entry_id, attachment_id, created_at) VALUES (?, ?, ?)",
                    { entry_id, id, time.now() })
                new_ids[#new_ids + 1] = id
            end
        end
    end)
    if not ok then
        res:status(413)
        res:html('<small role="alert">Upload failed: ' .. tostring(err) .. '</small>')
        return
    end
    -- Re-render the photo strip for this entry as the response.
    res:html(template.render("partials/_attachment_strip.html", {
        t = { id = entry_id, attachments = list_attachments_for_todo(entry_id) }
    }))
end, { multipart = { max_part_size = 8 * 1024 * 1024 } })
```

```javascript
app.post("/entries/:id/photos", async (req, res) => {
    const entryId = Number.parseInt(req.params.id, 10);
    try {
        for await (const part of req.multipart()) {
            if (part.filename) {
                const id = await attachment.store(part, {
                    uploadedBy: req.ctx.session_id,
                });
                db.exec(
                    "INSERT INTO entry_attachments (entry_id, attachment_id, created_at) VALUES (?, ?, ?)",
                    [entryId, id, time.now()]);
            }
        }
    } catch (e) {
        res.status(413);
        res.html('<small role="alert">Upload failed: ' +
                 String(e.message || e) + '</small>');
        return;
    }
    res.html(template.render("partials/_attachment_strip.html", {
        t: { id: entryId, attachments: listAttachmentsForTodo(entryId) }
    }));
}, { multipart: { maxPartSize: 8 * 1024 * 1024 } });
```

The route-level `multipart = { max_part_size = ... }` is the
multipart parser's cap (set generously). The `attachment.init({
max_size = ... })` cap fires inside `attachment.store` and is the
one that gates "is this upload too big for our app." Put the
multipart cap above the attachment cap; the attachment cap is what
produces the user-facing error.

### Validation feedback on rejected MIMEs

If `attachment.init` was called with a `mime_allowlist`, an upload of
a disallowed file (e.g. PDF when only images are allowed) raises
inside `attachment.store`. The `pcall` / `try-catch` above catches
it and emits a structured 413 with a small `role="alert"` fragment.
HTMX swaps it into the form's `hx-target` so the user sees the error
inline without a page reload.

For more elaborate UX (per-field validation messages, multi-error
display), use the same `_form_field.html` partial pattern documented
in the [Form re-population](#form-re-population-on-validation-error)
section above - pass `errors = { photo = "MIME not allowed" }` to
the form template and let `_form_field.html` render the error block.

### Progress events

HTMX 2.x fires `htmx:xhr:progress` events for in-flight uploads. The
`upload` event detail carries `loaded` / `total`:

```html
<script nonce="{{ csp_nonce }}">
  document.body.addEventListener('htmx:xhr:progress', (e) => {
    const pct = (e.detail.loaded / e.detail.total) * 100;
    const bar = document.getElementById('upload-progress');
    if (bar) bar.style.width = pct.toFixed(1) + '%';
  });
</script>
```

For a single global progress bar, that's enough. For per-file progress
on multi-file uploads, switch to `htmx:beforeRequest` / `htmx:afterRequest`
and key the bar by the form element.

### Serving the uploaded photo

The `<img>` tag points at a route that calls `attachment-serve.serve`:

```html
<figure id="att-{{ a.id }}">
  <img src="/entries/{{ t.id }}/photos/{{ a.id }}"
       alt="{{ a.original_name }}"
       loading="lazy">
  <button hx-delete="/entries/{{ t.id }}/photos/{{ a.id }}"
          hx-target="#att-{{ a.id }}"
          hx-swap="delete"
          hx-confirm="Delete this photo?">×</button>
</figure>
```

```lua
app.get("/entries/:id/photos/:att_id", function(req, res)
    attachment_serve.serve(req, res, req.params.att_id, {
        auth_check = function(req, meta)
            -- Gate however your app wants. The demo gates on
            -- "is this attachment attached to this entry?" via a
            -- join table. A real multi-user app would compare
            -- meta.uploaded_by against req.ctx.user_id.
            return owns_attachment(tonumber(req.params.id), req.params.att_id)
        end,
    })
end)
```

`attachment-serve.serve` handles `If-None-Match` → 304 automatically
(the ETag is the blob's SHA-256, so unchanged images hit cache),
sets `Content-Disposition` with the original filename, and uses
`res:bytes` for binary-safe transfer (no gzip on already-compressed
images).

`loading="lazy"` on the `<img>` makes the browser only fetch
attachments as they scroll into view - useful for entry lists with
many attachments.

### Working example

See [`examples/hypermedia_photos`](../examples/hypermedia_photos) for the
end-to-end demo: per-entry photo strip, file-input upload with
type-filtering, delete-with-confirm, the auth_check pattern, and
the strip-swap response. The end-to-end test in
[`tests/e2e_hypermedia_photos_upload.sh`](../tests/e2e_hypermedia_photos_upload.sh)
exercises the full flow.

## See also

- `examples/hypermedia_photos/`. the canonical scaffolded app, both runtimes.
- `stdlib/lua/hull/web/htmx.lua` / `stdlib/js/hull/web/htmx.js`. the helper module source.
- `stdlib/lua/hull/web/middleware/csp.lua` / `stdlib/js/hull/web/middleware/csp.js`. the CSP middleware.
- `stdlib/lua/hull/web/middleware/csrf.lua` / `stdlib/js/hull/web/middleware/csrf.js`. CSRF tokens, HMAC-SHA256.
- `stdlib/lua/hull/template.lua`. template engine, full syntax reference at the top of the file.
- HTMX docs: <https://htmx.org/docs/>.
- Pico v2 docs: <https://picocss.com/docs/>.
- `docs/attachments.md`. the attachment storage module reference (`hull/attachment@1`).
- `docs/roadmap_next.md` §1.5.b for the streaming-multipart / attachment work.
