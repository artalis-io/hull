# HTMX widgets — usage guide (§1.5.g tier)

> **Status: shipped.** All 8 widgets are first-party stdlib modules
> available in any Hull binary that ships the HTTP server.
> Cross-references: [`docs/htmx.md`](htmx.md) for the broader htmx
> hypermedia profile; [`examples/htmx_widgets_register/`](../examples/htmx_widgets_register/)
> for a worked CRUD app exercising every widget; the
> [§1.5.g roadmap entry](roadmap_next.md) for design history.

The htmx widget tier is a set of server-rendered, structurally-CSS'd
helpers for the recurring UI patterns of every data-list-heavy
hypermedia app: search, sort, paginate, inline-edit, confirm before
destructive action, toast on completion, surface validation errors
with the right a11y wiring. Zero client framework. ~395 LOC of plain
JS total across all 8 widgets, all loaded once per page.

## At a glance

| Widget | Server helpers | Client JS | Client CSS |
|---|---|---|---|
| [`toast`](#toast) | `show / info / success / warning / error` | ~150 LOC | structural |
| [`confirm`](#confirm) | `attrs(question, opts?)` | ~140 LOC | structural |
| [`form`](#form) | `errors / field_error / field_attrs` | ~75 LOC | structural |
| [`search`](#search) | `input_attrs / results_attrs` | — | — |
| [`inline-edit`](#inline-edit) | `cell(opts) / editor(opts)` | ~30 LOC | structural |
| [`sort`](#sort) | `parse(req) / header_attrs(col, current, opts)` | — | structural |
| [`pagination`](#pagination) | `nav(total, opts)` | — | — |
| [`table`](#table) | `render(rows, schema, opts)` | — | — |

## The handler-pre-render pattern

Hull's template engine supports nil-safe dot paths but **does not
support function calls** in `{{ }}` expressions. Widget helpers run
**server-side in the handler** and the resulting strings flow into
the template data table. Templates splice via `{{ name | raw }}`.

This is the **only** correct usage shape — examples on this page
all follow it. If you see `{{ widget.func(...) | raw }}` in
older docs or AI-generated code, it won't work.

```lua
-- HANDLER: pre-render once, pass through.
local confirm = require("hull.web.htmx.confirm")
local DELETE_ATTRS = confirm.attrs("Delete?", { danger = true })

app.get("/items", function(req, res)
    res:html(template.render("items.html", {
        delete_attrs = DELETE_ATTRS,
    }))
end)
```

```html
<!-- TEMPLATE -->
<button hx-delete="/items/{{ id }}" {{ delete_attrs | raw }}>×</button>
```

For static-config widgets (same `confirm.attrs` call on every render)
build the string **once at module load** as a constant. For
per-request widgets (`form.field_error(errors, name)` where `errors`
varies), build per-request inside the handler.

## Setup checklist

Every app using the widget tier needs:

1. **Manifest declarations.** Declare each widget module:
   ```lua
   app.manifest({
       modules = {
           "hull/web/htmx@1",            -- core (always)
           "hull/web/htmx/toast@1",      -- pick the widgets you use
           "hull/web/htmx/confirm@1",
           -- ... etc
       },
       csp = "htmx",                     -- preset for the SSR + stdlib-JS pattern
   })
   ```
2. **Template assets** in your base template:
   ```html
   <!-- One <link> per widget that ships CSS -->
   <link rel="stylesheet" href="/static/hull/htmx/toast/toast.css">
   <link rel="stylesheet" href="/static/hull/htmx/confirm/confirm.css">
   <link rel="stylesheet" href="/static/hull/htmx/form/form.css">
   <link rel="stylesheet" href="/static/hull/htmx/inline-edit/inline-edit.css">
   <link rel="stylesheet" href="/static/hull/htmx/sort/sort.css">

   <!-- htmx core + the widget client JS -->
   <script src="/static/vendor/htmx.min.js"></script>
   <script src="/static/hull/htmx/toast/toast.js" defer></script>
   <script src="/static/hull/htmx/confirm/confirm.js" defer></script>
   <script src="/static/hull/htmx/form/form.js" defer></script>
   <script src="/static/hull/htmx/inline-edit/inline-edit.js" defer></script>
   ```
3. **Vendored htmx** at `static/vendor/htmx.min.js`. Hull doesn't
   ship htmx itself; vendor it (see `examples/hypermedia_photos/static/vendor/`).

The CSP preset `csp = "htmx"` expands to a known-good policy
covering `script-src 'self'`, `connect-src 'self'`, etc. — what the
shipped widget JS needs. See [`docs/security.md`](security.md) §3.A
for the full expansion.

---

## toast

Transient flash messages fired via the `HX-Trigger` response header.
The shipped client JS listens once at page load and renders each
event as a styled `<li>` in an `<ol id="hull-toast" aria-live="polite">`
stack (auto-created if missing). Multiple toasts stack; click `×`
or wait `duration` ms to dismiss. Per-id dedup: firing the same id
twice updates in place rather than stacking duplicates.

```lua
local toast = require("hull.web.htmx.toast")

app.post("/save", function(req, res)
    -- ... save ...
    toast.success(res, "Saved")               -- level=success
    -- or: toast.error / warning / info, or:
    toast.show(res, "Custom", { level = "info", duration = 8000, id = "unique" })
    res:html(...)
end)
```

| Option | Default | Notes |
|---|---|---|
| `level` | `"info"` | `info | success | warning | error`; unknown values collapse to `info` |
| `duration` | `4000` (ms) | Auto-dismiss timer; pass `0` for sticky |
| `id` | nil | Stable id for client-side dedup |

Apps style per-level appearance via `[data-level="..."]` selectors
on `.hull-toast-item`.

---

## confirm

Replaces htmx's default `window.confirm()` browser popup with a
styled native `<dialog>`. Intercepts `htmx:confirm`, shows the
dialog, calls `evt.detail.issueRequest()` only on the explicit
Confirm-button path; Esc / backdrop / Cancel all silently drop the
request.

```lua
local confirm = require("hull.web.htmx.confirm")

-- Build once at module load (config is static across requests):
local DELETE_ATTRS = confirm.attrs("Delete this asset?",
    { danger = true, yes = "Delete" })
```

```html
<button hx-delete="/assets/{{ id }}" {{ delete_attrs | raw }}>×</button>
```

| Option | Default | Notes |
|---|---|---|
| `yes` | `"Confirm"` | Confirm-button label |
| `no` | `"Cancel"` | Cancel-button label (focused by default) |
| `danger` | `false` | Adds `[data-danger="true"]` to the yes button for red-styling |
| `title` | nil | Optional heading above the question |

Single-instance: a second `htmx:confirm` while the dialog is open
overwrites the first (the first's request is silently dropped).

---

## form

Renders accessible validation errors next to inputs and adds an
auto loading state to the submit button.

### Validation errors

```lua
local form_w = require("hull.web.htmx.form")

app.post("/items", function(req, res)
    local body = form_lib.parse(req.body or "")
    local ok, errors = validate.check(body, schema)
    res:html(template.render("partials/item_form.html", {
        values     = body,
        name_attrs = form_w.field_attrs(errors, "name"),
        name_error = form_w.field_error(errors, "name"),
    }))
end)
```

```html
<input type="text" name="name" value="{{ values.name }}"
       {{ name_attrs | raw }}>
{{ name_error | raw }}
```

`field_attrs` returns `aria-invalid="true" aria-describedby="hull-form-error-name"`
when the field has an error; empty string otherwise. `field_error`
returns the matched-id `<span class="hull-form-error">` or empty
string. Same id on both → screen reader landing on the input is
pointed straight at the message.

`form.errors(errors)` renders an additional whole-form summary
block as `<ul role="alert">` with each `<li>` linking to its inline
span via fragment id.

### Loading state on submit button

Add `data-loading-label="..."` to the submit button:

```html
<button type="submit" data-loading-label="Saving…">Save</button>
```

The shipped JS toggles `aria-busy="true"` + `disabled` during the
request, swaps the button text to the label, and restores both on
completion (including `htmx:responseError` — failed validations
don't leave the button stuck).

---

## search

Debounced search input that posts to a server-rendered results
partial. Pure htmx — no client JS shipped.

```lua
local search = require("hull.web.htmx.search")

-- Build once:
local INPUT_ATTRS = search.input_attrs({
    url    = "/items/search",
    target = "#results",
    -- defaults: name="q", delay_ms=300, method="get", push_url=false
})
local RESULTS_ATTRS = search.results_attrs()  -- id="hull-search-results"
```

```html
<input placeholder="Search…" {{ search_input_attrs | raw }}>
<div {{ search_results_attrs | raw }}></div>

<!-- or with a custom id matching your target -->
<div id="results"></div>
```

| Option | Default | Notes |
|---|---|---|
| `url` | required | GET (or POST) endpoint returning the results fragment |
| `target` | `"#hull-search-results"` | hx-target CSS selector |
| `name` | `"q"` | Query-string param name |
| `delay_ms` | `300` | Debounce window |
| `method` | `"get"` | Or `"post"` for sensitive / long queries |
| `push_url` | `false` | Set true for bookmarkable search-results pages |
| `indicator` | nil | CSS selector for `<span class="htmx-indicator">` spinner |

Server-side: handler reads `req.query.q`, returns rows or empty
fragment for short queries.

---

## inline-edit

Click-to-edit single field. Canonical htmx round-trip: GET fetches
edit form, PATCH saves and returns display fragment. The cell is
keyboard-activatable (Enter/Space/click); the editor input auto-
focuses + selects after swap; Esc cancels.

Three endpoints per editable field — display cell GET, edit form GET,
save PATCH:

```lua
local ie = require("hull.web.htmx.inline-edit")

-- Initial render (pre-built per-row in your handler):
local cell_html = ie.cell({
    value    = asset.name,
    edit_url = "/assets/" .. asset.id .. "/name/edit",
    label    = "Edit name",          -- aria-label + title
})

-- GET /assets/:id/name/edit returns the editor:
app.get("/assets/:id/name/edit", function(req, res)
    local a = db.query("SELECT name FROM assets WHERE id=?", { req.params.id })[1]
    res:html(ie.editor({
        value      = a.name,
        save_url   = "/assets/" .. req.params.id .. "/name",
        cancel_url = "/assets/" .. req.params.id .. "/name/view",
        label      = "Asset name",
    }))
end)

-- PATCH /assets/:id/name returns the new display cell:
app.patch("/assets/:id/name", function(req, res)
    local body = form_lib.parse(req.body or "")
    db.exec("UPDATE assets SET name=? WHERE id=?", { body.value, req.params.id })
    res:html(ie.cell({
        value    = body.value,
        edit_url = "/assets/" .. req.params.id .. "/name/edit",
    }))
end)

-- GET /assets/:id/name/view — for Esc / Cancel:
app.get("/assets/:id/name/view", ...)  -- returns ie.cell same as PATCH success
```

`label` defaults to `"Edit"` for the display cell and `"Edit value"`
for the editor.

---

## sort

Sortable column headers driven by `?sort=col[:asc|desc]`. Two
helpers: `parse(req, opts)` reads + allowlist-validates the query
param; `header_attrs(col, current, opts)` returns the htmx attribute
string for an app-owned `<th>` element.

```lua
local sort = require("hull.web.htmx.sort")

app.get("/assets", function(req, res)
    local current = sort.parse(req, {
        allowed = { "id", "name", "category", "status" },   -- whitelist
        default = "id",                                     -- fallback
    })
    -- current = { column = "name", direction = "asc" } or nil
    local rows = db.query(
        "SELECT * FROM assets ORDER BY " .. current.column
        .. " " .. current.direction
        -- column allowlist-checked + sanitized → SQL-injection safe;
        -- direction constrained to "asc"|"desc" by sort.parse.
    )
    res:html(template.render("pages/assets.html", {
        rows = rows,
        name_sort = sort.header_attrs("name", current, {
            url    = "/assets",
            target = "#asset-table",
        }),
        -- ... per column
    }))
end)
```

```html
<th {{ name_sort | raw }}>Name</th>
```

The emitted attrs include `role="button"` + `tabindex="0"` (keyboard
activation), `aria-sort="ascending|descending|none"`, `data-sort-direction`
+ `class="hull-sort-{none|asc|desc}"` for styling, `hx-get="<url>?sort=col:dir"`
with the toggled direction.

Toggle semantics: asc → desc → asc (clicking a sorted column flips;
clicking a different column resets to asc).

Column-name safety: anything outside `[A-Za-z0-9_-]` is silently
collapsed to `_` AND the column is rejected if not in the allowlist.

Structural CSS ships direction-indicator content (▲ ▼ ↕) via
`::after` — apps override for custom icons.

---

## pagination

Htmx-attributed nav rendered over the existing `hull/web/pagination@1`
page-math (page count, prev/next, ellipsis insertion). Replaces
the ~30-line pagination partial every hypermedia app writes by hand.

```lua
local hxpag = require("hull.web.htmx.pagination")

app.get("/items", function(req, res)
    local total = db.query("SELECT COUNT(*) AS n FROM items")[1].n
    local nav_html = hxpag.nav(total, {
        page     = tonumber(req.query.page) or 1,
        per_page = 20,
        base_url = "/items",
        target   = "#item-list",
        push_url = true,
    })
    res:html(template.render("pages/items.html", {
        items          = ...,
        pagination_nav = nav_html,
    }))
end)
```

```html
<div id="item-list">...rows...</div>
{{ pagination_nav | raw }}
```

Returns `""` when there's only one page. Each link carries both
`hx-get` AND `href` for progressive enhancement (search-engine
crawlers + no-JS users still get working pagination).

---

## table

Schema-driven `<table>` renderer that composes sort + inline-edit
per column. The grid pattern of every admin / data UI in one helper
call.

```lua
local hxtable = require("hull.web.htmx.table")

local SCHEMA = {
    { name = "id",       label = "ID" },
    { name = "name",     label = "Name",     sortable = true, editable = true },
    { name = "category", label = "Category", sortable = true, editable = true },
    { name = "status",   label = "Status",   sortable = true,
        render = function(value, _row)  -- custom cell rendering
            return '<span class="badge badge-' .. value .. '">' .. value .. '</span>'
        end,
    },
}

app.get("/assets", function(req, res)
    local current = sort.parse(req, { allowed = ..., default = "id" })
    local rows = db.query("SELECT ... ORDER BY " .. current.column
                          .. " " .. current.direction)
    local html = hxtable.render(rows, SCHEMA, {
        base_url     = "/assets",
        target       = "#asset-table",
        current_sort = current,
        edit_url_for = function(row, col)
            return "/assets/" .. row.id .. "/" .. col .. "/edit"
        end,
    })
    res:html(template.render("pages/assets.html", { asset_table = html }))
end)
```

```html
<div id="asset-table">{{ asset_table | raw }}</div>
```

Schema column fields:
- `name` (required) — key in row + URL param
- `label` — header text (default: same as `name`)
- `sortable` — when true, header gets sort widget attrs
- `editable` — when true, cell wraps value in `inline_edit.cell`
- `render(value, row)` — custom cell HTML (spliced raw; app owns safety)

`opts.edit_url_for(row, col_name)` is required when any column has
`editable=true`.

Apps wire search input + pagination footer around the table
separately using the other widgets — `table.render` owns the grid,
not the page layout.

---

## Composition

The widgets compose naturally. The canonical "register-style admin
page" pulls together 6-8 of them; see
[`examples/htmx_widgets_register/`](../examples/htmx_widgets_register/)
for a complete worked example (~210 LOC for the app + ~50 for
templates), plus a UX-test checklist for verifying everything by
hand in a browser.

## Forks worth knowing

- **`csp = "htmx"` preset.** Always pair with the widget tier;
  shipped JS won't load otherwise under a stricter default CSP.
  See [`docs/security.md`](security.md) §3.A.
- **No client framework.** All widget client code is plain JS
  loaded via `<script>` from `/static/hull/htmx/...`. No bundler,
  no node_modules, no build step.
- **Structural CSS only.** Widgets ship layout + a11y CSS, not
  appearance. Apps theme by targeting the documented classes /
  data attributes (`.hull-toast-item[data-level="success"]`,
  `dialog#hull-confirm[data-danger="true"]`, etc.).
- **Templates can't call functions.** Always pre-render widget
  helpers in the handler. The widget docstrings show the right
  pattern.

## Related

- [`docs/htmx.md`](htmx.md) — broader htmx hypermedia profile
  (Pico, CSP nonces, scaffold structure)
- [`docs/security.md`](security.md) §3.A — CSP preset table +
  per-widget XSS notes
- [`examples/htmx_widgets_register/`](../examples/htmx_widgets_register/)
  — worked CRUD app, UX-test checklist
- [`examples/hypermedia_photos/`](../examples/hypermedia_photos/) —
  larger app with auth + multipart uploads, partially migrated to
  the widget tier
