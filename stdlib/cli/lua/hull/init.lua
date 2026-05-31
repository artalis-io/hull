--
-- hull.init — Initialize a Hull project in the current (or specified) directory
--
-- Usage: hull init [dir] [--runtime lua|js] [--cli] [--profile <name>]
--
-- Unlike `hull new`, init is idempotent: it creates missing files but never
-- overwrites existing ones. Works in-place like `git init`.
--
-- If an app file already exists, mode (CLI vs server) is detected from its
-- contents (presence of `app.main(`). Otherwise the --cli flag picks mode;
-- default is server.
--
-- --profile <name>: opt into a richer scaffold for a specific app shape.
--   Currently supported: "htmx" (HTMX + Pico classless + per-request CSP
--   nonce + session + CSRF). Default: minimal "hello world" + /health.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

-- ── File templates (same content as hull.new) ────────────────────────

local templates = {}

templates.lua_app = [[-- Declare every first-party module the app imports.
-- The runtime gate refuses undeclared imports — `hull modules available`
-- lists the full registry. Add capability sections (fs, hosts, env)
-- alongside `modules` when a module needs them.
local log = require("hull.log")
local time = require("hull.time")

app.manifest({
    modules = {
        -- hull/http-server installs app.get/post/use/etc on the
        -- `app` intrinsic. Without it, app.get below is nil at load.
        "hull/http-server@1",
        "hull/log@1",
        "hull/time@1",
    },
})

app.get("/", function(_req, res)
    res:json({ status = "ok" })
end)

app.get("/health", function(_req, res)
    res:json({ status = "ok", uptime = time.clock() })
end)

log.info("app loaded")
]]

templates.lua_test = [[test("GET / returns ok", function()
    local res = test.get("/")
    test.eq(res.status, 200)
end)

test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.ok(res.json.uptime)
end)
]]

templates.js_app = [[// Declare every first-party module the app imports.
// The runtime gate refuses undeclared imports — `hull modules available`
// lists the full registry. Add capability sections (fs, hosts, env)
// alongside `modules` when a module needs them.
import { app }  from "hull:app";
import { log }  from "hull:log";
import { time } from "hull:time";

app.manifest({
    modules: [
        // hull/http-server installs app.get/post/use/etc on the
        // `app` intrinsic. Without it, app.get below is undefined.
        "hull/http-server@1",
        "hull/log@1",
        "hull/time@1",
    ],
});

app.get("/", (_req, res) => {
    res.json({ status: "ok" });
});

app.get("/health", (_req, res) => {
    res.json({ status: "ok", uptime: time.clock() });
});

log.info("app loaded");
]]

templates.js_test = [[test("GET / returns ok", async () => {
    const res = await test.get("/");
    test.eq(res.status, 200);
});

test("GET /health returns ok", async () => {
    const res = await test.get("/health");
    test.eq(res.status, 200);
    test.ok(res.json.uptime);
});
]]

templates.lua_cli_app = [[-- CLI app — `hull run app.lua [-- args...]` invokes app.main once and exits.
-- `app.main` is mutually exclusive with route registration: pick CLI mode
-- (app.main) or server mode (app.get/etc), not both.

app.manifest({
    modules = {},
})

app.main(function(ctx)
    local who = ctx.args[1] or "world"
    ctx.stdout:write("hello " .. who .. "\n")
    return 0
end)
]]

templates.lua_cli_test = [[test("placeholder", function()
    test.eq(1, 1)
end)
]]

templates.js_cli_app = [[// CLI app — `hull run app.js [-- args...]` invokes app.main once and exits.
import { app } from "hull:app";

app.manifest({
    modules: [],
});

app.main(async (ctx) => {
    const who = ctx.args[0] ?? "world";
    ctx.stdout.write(`hello ${who}\n`);
    return 0;
});
]]

templates.js_cli_test = [[test("placeholder", () => {
    test.eq(1, 1);
});
]]

templates.gitignore = [[data.db
data.db-*
*.key
hull.sig
build/
]]

templates.migration_init = [[-- Migration: 001_init
-- Add your initial schema here
]]

-- ── HTMX profile templates ───────────────────────────────────────────
-- The "htmx" profile scaffolds a fuller starter app: per-request CSP
-- nonce, vendored HTMX + Pico classless CSS (fetched via `make
-- fetch-vendor`), CSRF on htmx requests, a sample todos endpoint with
-- partial-fragment rendering. Both runtimes use the same template HTML
-- (the only differences are in app.{lua,js}).

templates.htmx_lua_app = [[-- HTMX + Pico hypermedia app scaffold.
-- Returns full pages for plain navigation; returns fragments when
-- HX-Request is set. CSRF + per-request CSP nonce wired in by default.
local htmx     = require("hull.htmx")
local csp      = require("hull.middleware.csp")
local csrf     = require("hull.middleware.csrf")
local session  = require("hull.middleware.session")
local cookie   = require("hull.cookie")
local template = require("hull.template")
local form     = require("hull.form")
local log      = require("hull.log")
local db       = require("hull.db")

app.manifest({
    modules = {
        "hull/http-server@1",
        "hull/htmx@1",
        "hull/middleware/csp@1",
        "hull/middleware/csrf@1",
        "hull/middleware/session@1",
        "hull/cookie@1",
        "hull/template@1",
        "hull/form@1",
        "hull/db@1",
        "hull/log@1",
    },
})

-- Sessions table (creates _hull_sessions on first run).
session.init({ ttl = 86400 })

-- Bootstrap an anonymous session on every request. CSRF binds to
-- this session id; without one, csrf.generate would fail. Creates a
-- new session on first visit (sets cookie); loads existing on
-- subsequent requests. Production apps would replace this with a
-- real login flow (auth.session_middleware).
local function session_bootstrap(req, res)
    req.ctx = req.ctx or {}
    local cookies = cookie.parse(req.headers["cookie"] or "")
    local sid = cookies.hull_session
    if sid and session.load(sid) then
        req.ctx.session_id = sid
        return 0
    end
    sid = session.create({})
    req.ctx.session_id = sid
    res:header("Set-Cookie", cookie.serialize("hull_session", sid))
    return 0
end

-- Pre-body middleware (runs before req.body is read):
--   1. CSP nonce. Populates req.ctx.csp_nonce + sets the header.
--   2. Session bootstrap. Populates req.ctx.session_id.
app.use("*", "/*", csp.htmx())
app.use("*", "/*", session_bootstrap)

-- Post-body middleware (runs after req.body is parsed):
--   3. CSRF. Verifies on unsafe methods; injects token on safe.
app.use_post("*", "/*", csrf.middleware({ secret = "CHANGE-ME-IN-PRODUCTION" }))

local function todos_data()
    return db.query("SELECT id, title, done FROM todos ORDER BY id DESC")
end

app.get("/", function(req, res)
    local data = {
        csp_nonce  = req.ctx.csp_nonce,
        csrf_token = req.ctx.csrf_token,
        todos      = todos_data(),
    }
    res:html(template.render("pages/home.html", data))
end)

app.post("/todos", function(req, res)
    local fields = form.parse(req.body or "")
    local title = (fields.title or ""):gsub("^%s+", ""):gsub("%s+$", "")
    if title == "" then
        -- Validation error fragment (replaces the input area).
        htmx.retarget(res, "#new-todo")
        res:html('<p id="new-todo" role="alert">Title cannot be empty.</p>')
        return
    end
    db.exec("INSERT INTO todos (title, done) VALUES (?, 0)", { title })
    local id = db.query("SELECT last_insert_rowid() AS id")[1].id

    if htmx.is(req) then
        -- HTMX: return the new row to insert + a fresh empty form.
        res:html(template.render("partials/todo_row.html",
            { id = id, title = title, done = false })
            .. template.render("partials/todo_form.html",
                { csrf_token = req.ctx.csrf_token }))
    else
        -- Plain form post: redirect back to /, browser reloads.
        res:redirect("/")
    end
end)

app.post("/todos/:id/toggle", function(req, res)
    local id = tonumber(req.params.id)
    db.exec("UPDATE todos SET done = NOT done WHERE id = ?", { id })
    local row = db.query("SELECT id, title, done FROM todos WHERE id = ?", { id })[1]
    if not row then res:status(404); return end
    if htmx.is(req) then
        res:html(template.render("partials/todo_row.html", row))
    else
        res:redirect("/")
    end
end)

app.delete("/todos/:id", function(req, res)
    local id = tonumber(req.params.id)
    db.exec("DELETE FROM todos WHERE id = ?", { id })
    if htmx.is(req) then
        res:html("")  -- htmx swap-mode=delete removes the row.
    else
        res:redirect("/")
    end
end)

log.info("hypermedia app loaded")
]]

templates.htmx_js_app = [[// HTMX + Pico hypermedia app scaffold.
// Returns full pages for plain navigation; returns fragments when
// HX-Request is set. CSRF + per-request CSP nonce wired in by default.
import { app }      from "hull:app";
import { htmx }     from "hull:htmx";
import { csp }      from "hull:middleware:csp";
import { csrf }     from "hull:middleware:csrf";
import { session }  from "hull:middleware:session";
import { cookie }   from "hull:cookie";
import { template } from "hull:template";
import { form }     from "hull:form";
import { log }      from "hull:log";
import { db }       from "hull:db";

app.manifest({
    modules: [
        "hull/http-server@1",
        "hull/htmx@1",
        "hull/middleware/csp@1",
        "hull/middleware/csrf@1",
        "hull/middleware/session@1",
        "hull/cookie@1",
        "hull/template@1",
        "hull/form@1",
        "hull/db@1",
        "hull/log@1",
    ],
});

session.init({ ttl: 86400 });

// Bootstrap anonymous session per request (same shape as Lua sibling).
//
// Cross-runtime note: the JS csrf middleware reads the session id from
// `req.headers.cookie`, not from `req.ctx`. When we create a new session
// here we also splice the cookie into req.headers so the downstream
// csrf middleware finds it on the SAME request (otherwise the first
// GET would render the form with an empty csrf token).
function sessionBootstrap(req, res) {
    req.ctx = req.ctx || {};
    const cookies = cookie.parse(req.headers["cookie"] || "");
    let sid = cookies.hull_session;
    if (sid && session.load(sid)) {
        req.ctx.session_id = sid;
        return 0;
    }
    sid = session.create({});
    req.ctx.session_id = sid;
    const setCookie = cookie.serialize("hull_session", sid);
    res.header("Set-Cookie", setCookie);
    // Make the new cookie visible to downstream middleware on this same
    // request. We only need the name=value part for csrf's parser.
    const prev = req.headers["cookie"] || "";
    req.headers["cookie"] = prev
        ? prev + "; hull_session=" + sid
        : "hull_session=" + sid;
    return 0;
}

app.use("*", "/*", csp.htmx());
app.use("*", "/*", sessionBootstrap);
// cookieName matches sessionBootstrap's cookie. JS csrf's default is
// `hull.sid` (stdlib-internal convention) while JS auth uses
// `hull_session`; we anchor to the session-cookie name we actually set.
app.usePost("*", "/*", csrf.middleware({
    secret: "CHANGE-ME-IN-PRODUCTION",
    cookieName: "hull_session",
}));

function todosData() {
    return db.query("SELECT id, title, done FROM todos ORDER BY id DESC");
}

app.get("/", (req, res) => {
    // Template literal keys are snake_case to match the Lua sibling +
    // the actual ctx keys (csp.js writes csp_nonce; csrf.js writes
    // csrf_token). Same template HTML works for both runtimes.
    res.html(template.render("pages/home.html", {
        csp_nonce:  req.ctx.csp_nonce,
        csrf_token: req.ctx.csrf_token,
        todos:      todosData(),
    }));
});

app.post("/todos", (req, res) => {
    const fields = form.parse(req.body || "");
    const title = (fields.title || "").trim();
    if (!title) {
        htmx.retarget(res, "#new-todo");
        res.html('<p id="new-todo" role="alert">Title cannot be empty.</p>');
        return;
    }
    db.exec("INSERT INTO todos (title, done) VALUES (?, 0)", [title]);
    const id = db.query("SELECT last_insert_rowid() AS id")[0].id;
    if (htmx.is(req)) {
        res.html(
            template.render("partials/todo_row.html",  { id, title, done: false })
            + template.render("partials/todo_form.html", { csrf_token: req.ctx.csrf_token })
        );
    } else {
        res.redirect("/");
    }
});

app.post("/todos/:id/toggle", (req, res) => {
    const id = parseInt(req.params.id, 10);
    db.exec("UPDATE todos SET done = NOT done WHERE id = ?", [id]);
    const row = db.query("SELECT id, title, done FROM todos WHERE id = ?", [id])[0];
    if (!row) { res.status(404); return; }
    if (htmx.is(req)) {
        res.html(template.render("partials/todo_row.html", row));
    } else {
        res.redirect("/");
    }
});

app.delete("/todos/:id", (req, res) => {
    const id = parseInt(req.params.id, 10);
    db.exec("DELETE FROM todos WHERE id = ?", [id]);
    if (htmx.is(req)) {
        res.html("");
    } else {
        res.redirect("/");
    }
});

log.info("hypermedia app loaded");
]]

templates.htmx_base_html = [[<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{% block title %}Hull HTMX app{% end %}</title>
  <link rel="stylesheet" nonce="{{ csp_nonce }}" href="/static/vendor/pico.classless.min.css">
  <link rel="stylesheet" nonce="{{ csp_nonce }}" href="/static/app.css">
  <script nonce="{{ csp_nonce }}" src="/static/vendor/htmx.min.js"></script>
</head>
<body>
  <main>
    {% block body %}{% end %}
  </main>
</body>
</html>
]]

templates.htmx_page_home = [[{% extends "base.html" %}

{% block title %}Todos{% end %}

{% block body %}
<h1>Todos</h1>

{% include "partials/todo_form.html" %}

<ul id="todos">
{% for t in todos %}
  {% include "partials/todo_row.html" %}
{% end %}
</ul>
{% end %}
]]

templates.htmx_partial_todo_form = [[<form id="new-todo"
      hx-post="/todos"
      hx-target="#todos"
      hx-swap="afterbegin">
  <input type="hidden" name="_csrf" value="{{ csrf_token }}">
  <input type="text" name="title" placeholder="New todo" required autofocus>
  <button type="submit">Add</button>
</form>
]]

templates.htmx_partial_todo_row = [[<li id="todo-{{ id }}">
  <input type="checkbox"
         {% if done %}checked{% end %}
         hx-post="/todos/{{ id }}/toggle"
         hx-target="#todo-{{ id }}"
         hx-swap="outerHTML">
  <span {% if done %}style="text-decoration: line-through"{% end %}>{{ title }}</span>
  <button hx-delete="/todos/{{ id }}"
          hx-target="#todo-{{ id }}"
          hx-swap="delete"
          hx-confirm="Delete this todo?">×</button>
</li>
]]

templates.htmx_app_css = [[/* hypermedia_todo custom styles. Layer on top of Pico's classless base. */
:root {
  --pico-form-element-spacing-vertical: 0.5rem;
}

ul#todos {
  list-style: none;
  padding-left: 0;
}

ul#todos li {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.25rem 0;
}

ul#todos li button {
  margin-left: auto;
  padding: 0 0.5rem;
}
]]

templates.htmx_migration = [[-- Migration: 001_init
-- Schema for the htmx todos demo.

CREATE TABLE IF NOT EXISTS todos (
    id    INTEGER PRIMARY KEY AUTOINCREMENT,
    title TEXT NOT NULL,
    done  INTEGER NOT NULL DEFAULT 0
);
]]

templates.htmx_test_lua = [[-- Tests for both plain HTML and HTMX request paths.
-- All requests use opts.middleware = true so the full middleware chain
-- (CSP nonce, session bootstrap, CSRF) runs; without it test.get/post
-- dispatches the handler directly and skips middleware.

test("GET / returns full HTML page", function()
    local res = test.get("/", { middleware = true })
    test.eq(res.status, 200)
    test.ok(string.find(res.body, "<!doctype html>"), "should be a full page")
    test.ok(string.find(res.body, "id=\"todos\""), "should contain todo list")
end)

test("POST /todos with hx-request returns fragment, not redirect", function()
    -- Get a CSRF token + session cookie by hitting / first.
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    test.ok(token and #token > 0, "csrf token should be in form")
    -- Send htmx-flavored POST with token + cookie.
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
    test.ok(string.find(res.body, "buy milk"), "fragment should contain new todo")
    test.ok(not string.find(res.body, "<!doctype"), "fragment must NOT be full page")
end)

test("POST /todos with empty title returns validation fragment", function()
    local home = test.get("/", { middleware = true })
    local token = string.match(home.body, 'name="_csrf" value="([^"]+)"')
    local res = test.post("/todos", {
        middleware = true,
        body = "title=&_csrf=" .. token,
        headers = {
            ["hx-request"] = "true",
            ["content-type"] = "application/x-www-form-urlencoded",
            ["cookie"] = home.headers["set-cookie"] or "",
            ["x-csrf-token"] = token,
        },
    })
    test.eq(res.status, 200)
    test.ok(string.find(res.body, "cannot be empty"), "should show validation error")
    test.eq(res.headers["hx-retarget"], "#new-todo",
            "should retarget to the form, not the list")
end)
]]

templates.htmx_test_js = [[// Tests for both plain HTML and HTMX request paths.
// All requests pass `middleware: true` so the full middleware chain
// (CSP nonce, session bootstrap, CSRF) runs; without it test.get/post
// dispatches the handler directly and skips middleware.

test("GET / returns full HTML page", async () => {
    const res = await test.get("/", { middleware: true });
    test.eq(res.status, 200);
    test.ok(res.body.includes("<!doctype html>"), "should be a full page");
    test.ok(res.body.includes('id="todos"'), "should contain todo list");
});

test("POST /todos with hx-request returns fragment, not redirect", async () => {
    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    test.ok(token && token.length > 0, "csrf token should be in form");
    const res = await test.post("/todos", {
        middleware: true,
        body: "title=buy+milk&_csrf=" + token,
        headers: {
            "hx-request": "true",
            "content-type": "application/x-www-form-urlencoded",
            "cookie": home.headers["set-cookie"] || "",
            "x-csrf-token": token,
        },
    });
    test.eq(res.status, 200);
    test.ok(res.body.includes("buy milk"), "fragment should contain new todo");
    test.ok(!res.body.includes("<!doctype"), "fragment must NOT be full page");
});

test("POST /todos with empty title returns validation fragment", async () => {
    const home = await test.get("/", { middleware: true });
    const token = home.body.match(/name="_csrf" value="([^"]+)"/)?.[1];
    const res = await test.post("/todos", {
        middleware: true,
        body: "title=&_csrf=" + token,
        headers: {
            "hx-request": "true",
            "content-type": "application/x-www-form-urlencoded",
            "cookie": home.headers["set-cookie"] || "",
            "x-csrf-token": token,
        },
    });
    test.eq(res.status, 200);
    test.ok(res.body.includes("cannot be empty"), "should show validation error");
    test.eq(res.headers["hx-retarget"], "#new-todo",
            "should retarget to the form, not the list");
});
]]

-- Asset metadata: pinned versions + SHA-256 of htmx + pico. Mirrors
-- the values in Hull's own Makefile (vendor/htmx, vendor/pico). Keep
-- in sync when Hull bumps; user apps can bump independently in their
-- own Makefile.
local HTMX_VERSION    = "v2.0.9"
local HTMX_SHA256     = "57d9191515339922bd1356d7b2d80b1ee3b29f1b3a2c65a078bb8b2e8fd9ae5f"
local PICO_VERSION    = "v2.1.1"
local PICO_SHA256     = "61207a40ffc02a42d1e50143651c121beab70ed413c934c1ff84fa263ba436b0"

templates.htmx_makefile = string.format([[# Generated by `hull init --profile htmx`.
# Bump HTMX/Pico versions + SHAs when you upgrade; the SHA verification
# stops on a mismatch.

HTMX_VERSION    := %s
HTMX_SHA256     := %s
HTMX_URL        := https://github.com/bigskysoftware/htmx/releases/download/$(HTMX_VERSION)/htmx.min.js

PICO_VERSION    := %s
PICO_SHA256     := %s
PICO_URL        := https://raw.githubusercontent.com/picocss/pico/$(PICO_VERSION)/css/pico.classless.min.css

VENDOR_DIR      := static/vendor

.PHONY: dev test build fetch-vendor verify-vendor clean

dev:
	hull dev

test:
	hull test

build:
	hull build

fetch-vendor: $(VENDOR_DIR)/htmx.min.js $(VENDOR_DIR)/pico.classless.min.css
	@echo "Vendor assets in place."

verify-vendor:
	@actual=$$(shasum -a 256 $(VENDOR_DIR)/htmx.min.js | awk '{print $$1}'); \
	if [ "$$actual" != "$(HTMX_SHA256)" ]; then \
	    echo "htmx.min.js SHA-256 mismatch (expected $(HTMX_SHA256), got $$actual)"; exit 1; fi
	@actual=$$(shasum -a 256 $(VENDOR_DIR)/pico.classless.min.css | awk '{print $$1}'); \
	if [ "$$actual" != "$(PICO_SHA256)" ]; then \
	    echo "pico.classless.min.css SHA-256 mismatch (expected $(PICO_SHA256), got $$actual)"; exit 1; fi
	@echo "Vendor SHAs OK."

$(VENDOR_DIR)/htmx.min.js:
	@mkdir -p $(VENDOR_DIR)
	curl -fsSL $(HTMX_URL) -o $@
	@actual=$$(shasum -a 256 $@ | awk '{print $$1}'); \
	if [ "$$actual" != "$(HTMX_SHA256)" ]; then \
	    echo "htmx.min.js SHA-256 mismatch (expected $(HTMX_SHA256), got $$actual)"; \
	    rm -f $@; exit 1; fi
	@echo "htmx $(HTMX_VERSION) OK ($$(wc -c < $@) bytes)."

$(VENDOR_DIR)/pico.classless.min.css:
	@mkdir -p $(VENDOR_DIR)
	curl -fsSL $(PICO_URL) -o $@
	@actual=$$(shasum -a 256 $@ | awk '{print $$1}'); \
	if [ "$$actual" != "$(PICO_SHA256)" ]; then \
	    echo "pico SHA-256 mismatch (expected $(PICO_SHA256), got $$actual)"; \
	    rm -f $@; exit 1; fi
	@echo "pico $(PICO_VERSION) OK ($$(wc -c < $@) bytes)."

clean:
	rm -rf build data.db data.db-*
]], HTMX_VERSION, HTMX_SHA256, PICO_VERSION, PICO_SHA256)

templates.htmx_readme = [[# HTMX + Pico app

Hypermedia-driven app scaffolded by `hull init --profile htmx`.

## First-run setup

```sh
make fetch-vendor   # downloads htmx + pico into static/vendor/ (SHA-pinned)
make dev            # starts hull dev server on :8080
```

`make fetch-vendor` is one-time: the assets get committed to your repo.
Re-run it (or `make verify-vendor`) when bumping HTMX or Pico versions
in the Makefile.

## What's wired in

- **HTMX** for partial page updates (full-page render on plain GET;
  fragment render on `HX-Request`).
- **Pico v2 classless** for default styling. Drop into `static/app.css`
  for custom rules.
- **Per-request CSP nonce** (`hull/middleware/csp@1` with the htmx
  profile: nonce-required for `<script>` and `<style>` blocks; inline
  `style="…"` attributes allowed for Pico's component styles).
- **Session-cookie storage** (basic, optional). Customize in `app.{lua,js}`.
- **CSRF protection** on unsafe methods (form posts + htmx requests).
  Change `secret = "CHANGE-ME-IN-PRODUCTION"` before deploying.

## Layout

```
app.{lua,js}             handlers + middleware
templates/
  base.html              <head>+<body> skeleton with nonce'd <script>/<link>
  pages/home.html        full-page render
  partials/
    todo_form.html       reusable form + CSRF token field
    todo_row.html        single-row fragment for htmx swaps
static/
  vendor/htmx.min.js          (fetched by make fetch-vendor)
  vendor/pico.classless.min.css   (fetched by make fetch-vendor)
  app.css                custom styles
migrations/
  001_init.sql           todos table
tests/
  test_app.{lua,js}      covers plain + htmx paths
```

## Pattern: when to return a fragment vs a full page

```
if htmx.is(req) then
    res:html(template.render("partials/foo.html", data))
else
    res:html(template.render("pages/foo.html", data))
end
```

See `docs/htmx.md` (in the Hull repo) for the full pattern guide.

## Next steps

- Replace `CHANGE-ME-IN-PRODUCTION` in `app.{lua,js}` with a real
  high-entropy secret loaded from env.
- Run `hull build` to produce a single binary with templates + static
  assets embedded.
]]

templates.htmx_gitignore = [[data.db
data.db-*
*.key
hull.sig
build/

# static/vendor/ is committed; fetched once via `make fetch-vendor`.
]]

-- ── Helpers ──────────────────────────────────────────────────────────

local function file_exists(path)
    return tool.file_exists(path)
end

local function ensure_dir(path)
    if not file_exists(path) then
        tool.mkdir(path)
        return true   -- created
    end
    return false      -- already existed
end

-- Write file only if it does not already exist. Returns true if written.
local function ensure_file(path, content)
    if not file_exists(path) then
        tool.write_file(path, content)
        return true
    end
    return false
end

local function detect_existing_runtime(dir)
    if file_exists(dir .. "/app.lua") then return "lua" end
    if file_exists(dir .. "/app.js")  then return "js"  end
    return nil
end

-- Detect mode from an existing app file: returns "cli" if app.main is
-- registered, otherwise "server". Returns nil if the file is missing or
-- unreadable.
local function detect_existing_mode(dir, runtime)
    if not runtime then return nil end
    local ext = (runtime == "js") and ".js" or ".lua"
    local path = dir .. "/app" .. ext
    if not file_exists(path) then return nil end
    local content = tool.read_file(path)
    if not content then return nil end
    if content:find("app%.main%s*%(", 1, false) then return "cli" end
    return "server"
end

-- ── Argument parsing ─────────────────────────────────────────────────

local function parse_args()
    local opts = {
        dir     = ".",
        runtime = nil,   -- nil = auto-detect from existing files, else "lua"/"js"
        cli     = nil,   -- nil = auto-detect or default server; true = force CLI
        profile = nil,   -- nil = default minimal scaffold; "htmx" = HTMX + Pico
    }

    local i = 1
    while i <= #arg do
        local a = arg[i]
        if a == "--runtime" then
            i = i + 1
            opts.runtime = arg[i]
        elseif a == "--cli" then
            opts.cli = true
        elseif a == "--profile" then
            i = i + 1
            opts.profile = arg[i]
        elseif a:sub(1, 10) == "--profile=" then
            opts.profile = a:sub(11)
        elseif a:sub(1, 1) ~= "-" then
            opts.dir = a
        end
        i = i + 1
    end

    return opts
end

-- Scaffold extra files for the `htmx` profile. Called from main()
-- AFTER the default scaffold has run, so it only adds files that
-- don't conflict with the base set. Uses the same track_file/track_dir
-- helpers (passed in) so created/skipped output stays consistent.
local function scaffold_htmx(dir, runtime, track_file, track_dir)
    local ext  = (runtime == "js") and ".js" or ".lua"
    local app_template  = (runtime == "js") and templates.htmx_js_app  or templates.htmx_lua_app
    local test_template = (runtime == "js") and templates.htmx_test_js or templates.htmx_test_lua

    -- Override the default app.lua and test_app.lua with the htmx
    -- versions. ensure_file is no-op when the file exists, so this
    -- only writes when the default scaffold didn't have to (first run
    -- in an empty dir). For idempotency in subsequent runs the user's
    -- modified copy is preserved.
    track_file(dir .. "/app" .. ext,            app_template)
    track_file(dir .. "/tests/test_app" .. ext, test_template)

    -- Template hierarchy.
    track_dir(dir .. "/templates")
    track_dir(dir .. "/templates/pages")
    track_dir(dir .. "/templates/partials")
    track_file(dir .. "/templates/base.html",
               templates.htmx_base_html)
    track_file(dir .. "/templates/pages/home.html",
               templates.htmx_page_home)
    track_file(dir .. "/templates/partials/todo_form.html",
               templates.htmx_partial_todo_form)
    track_file(dir .. "/templates/partials/todo_row.html",
               templates.htmx_partial_todo_row)

    -- Static assets (custom CSS lands directly; vendor files come
    -- via `make fetch-vendor` because Hull doesn't carry curl in its
    -- tool-spawn allowlist).
    track_dir(dir .. "/static")
    track_dir(dir .. "/static/vendor")
    track_file(dir .. "/static/app.css", templates.htmx_app_css)

    -- Override the default 001_init.sql + .gitignore with the htmx
    -- versions. ensure_file is no-op if a real one already exists.
    track_file(dir .. "/migrations/001_init.sql",
               templates.htmx_migration)
    track_file(dir .. "/.gitignore", templates.htmx_gitignore)

    -- Makefile + README only land for the htmx profile (the default
    -- scaffold doesn't generate either).
    track_file(dir .. "/Makefile", templates.htmx_makefile)
    track_file(dir .. "/README.md", templates.htmx_readme)
end

-- ── Main ─────────────────────────────────────────────────────────────

local function main()
    local opts = parse_args()
    local dir  = opts.dir

    -- Validate runtime flag if given
    if opts.runtime and opts.runtime ~= "lua" and opts.runtime ~= "js" then
        tool.stderr("hull init: invalid runtime '" .. opts.runtime .. "' (use lua or js)\n")
        tool.exit(1)
    end

    -- Create target directory if needed
    local dir_created = false
    if not file_exists(dir) then
        tool.mkdir(dir)
        dir_created = true
    end

    -- Detect or choose runtime
    local existing_runtime = detect_existing_runtime(dir)
    local runtime = opts.runtime or existing_runtime or "lua"

    -- Detect or choose mode (CLI vs server). Priority:
    --   1. --cli flag (explicit opt-in)
    --   2. existing app file content (app.main → cli, else server)
    --   3. default: server
    local existing_mode = detect_existing_mode(dir, existing_runtime)
    local cli_mode = opts.cli or (existing_mode == "cli")

    local ext = runtime == "js" and ".js" or ".lua"
    local app_template, test_template
    if cli_mode then
        app_template  = runtime == "js" and templates.js_cli_app  or templates.lua_cli_app
        test_template = runtime == "js" and templates.js_cli_test or templates.lua_cli_test
    else
        app_template  = runtime == "js" and templates.js_app  or templates.lua_app
        test_template = runtime == "js" and templates.js_test or templates.lua_test
    end

    -- Track what we create vs skip
    local created = {}
    local skipped = {}

    local function track_file(path, content)
        if ensure_file(path, content) then
            created[#created + 1] = path
        else
            skipped[#skipped + 1] = path
        end
    end

    local function track_dir(path)
        if ensure_dir(path) then
            created[#created + 1] = path .. "/"
        end
    end

    if not cli_mode then
        track_dir(dir .. "/migrations")
    end
    track_dir(dir .. "/tests")

    -- Profile scaffolds run FIRST so their files exist before the
    -- generic versions try to write; ensure_file is no-op on conflict.
    -- Validation of the profile name happens here, not at arg-parse
    -- time, so the error message can mention the supported list.
    if opts.profile then
        if opts.profile == "htmx" then
            if cli_mode then
                tool.stderr("hull init: --profile htmx requires server mode (omit --cli)\n")
                tool.exit(1)
            end
            scaffold_htmx(dir, runtime, track_file, track_dir)
        else
            tool.stderr("hull init: unknown profile '" .. opts.profile ..
                        "' (supported: htmx)\n")
            tool.exit(1)
        end
    end

    -- Generic / fallback files. ensure_file (track_file) won't
    -- overwrite a profile's version that was just written above.
    track_file(dir .. "/app" .. ext,            app_template)
    track_file(dir .. "/tests/test_app" .. ext, test_template)
    if not cli_mode then
        track_file(dir .. "/migrations/001_init.sql", templates.migration_init)
    end
    track_file(dir .. "/.gitignore", templates.gitignore)

    -- Output
    local display_dir = (dir == ".") and "./" or dir .. "/"
    if dir_created then
        print("hull init: created " .. display_dir)
    else
        print("hull init: initialized " .. display_dir)
    end
    print("  runtime   " .. runtime)
    print("  mode      " .. (cli_mode and "cli" or "server"))
    if opts.profile then
        print("  profile   " .. opts.profile)
    end
    print("")

    if #created > 0 then
        print("  created:")
        for _, f in ipairs(created) do
            print("    " .. f)
        end
    end

    if #skipped > 0 then
        print("  already present (unchanged):")
        for _, f in ipairs(skipped) do
            print("    " .. f)
        end
    end

    print("")

    local app_path = (dir == "." and "" or dir .. "/") .. "app" .. ext
    print("Next steps:")
    if opts.profile == "htmx" then
        local cd_prefix = (dir == ".") and "" or ("cd " .. dir .. " && ")
        print("  " .. cd_prefix .. "make fetch-vendor   # download htmx + pico (SHA-pinned)")
        print("  " .. cd_prefix .. "make dev            # serve on :8080")
        print("  " .. cd_prefix .. "make test           # run the htmx + plain-form tests")
    elseif cli_mode then
        print("  hull run " .. app_path .. " -- world")
    else
        print("  hull " .. app_path)
    end
end

main()
