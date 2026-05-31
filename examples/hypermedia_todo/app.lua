-- HTMX + Pico hypermedia app scaffold.
-- Returns full pages for plain navigation; returns fragments when
-- HX-Request is set. CSRF + per-request CSP nonce wired in by default.
local htmx     = require("hull.web.htmx")
local csp      = require("hull.web.middleware.csp")
local csrf     = require("hull.web.middleware.csrf")
local session  = require("hull.web.middleware.session")
local cookie   = require("hull.web.cookie")
local template = require("hull.template")
local form     = require("hull.web.form")
local log      = require("hull.log")
local db       = require("hull.db")

app.manifest({
    modules = {
        "hull/http-server@1",
        "hull/web/htmx@1",
        "hull/web/middleware/csp@1",
        "hull/web/middleware/csrf@1",
        "hull/web/middleware/session@1",
        "hull/web/cookie@1",
        "hull/template@1",
        "hull/web/form@1",
        "hull/db@1",
        "hull/log@1",
    },
})

-- Sessions table (creates _hull_sessions on first run).
session.init({ ttl = 86400 })

-- CSRF secret. CHANGE-ME-IN-PRODUCTION is the placeholder shipped by
-- the scaffold; load from env (or a sealed secret) before deploying.
-- The scaffold logs a one-time warning at startup if it's still the
-- placeholder so a deploy can't silently inherit it.
local CSRF_SECRET = "CHANGE-ME-IN-PRODUCTION"
if CSRF_SECRET == "CHANGE-ME-IN-PRODUCTION" then
    log.warn("WARNING: CSRF secret is the scaffold placeholder. " ..
             "Replace `CSRF_SECRET` in app.lua with a real high-entropy " ..
             "value before deploying (load from env, etc.).")
end

-- Bootstrap an anonymous session on every request. CSRF binds to
-- this session id; without one, csrf.generate would fail. Creates a
-- new session on first visit (sets cookie); loads existing on
-- subsequent requests. Production apps would replace this with a
-- real login flow (auth.session_middleware).
--
-- `secure = false` on the cookie is intentional for the scaffold: it
-- lets `hull dev` (plain HTTP on :8080) work in a real browser.
-- Production over HTTPS should pass `{ secure = true }` so the cookie
-- is only sent over TLS.
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
    res:header("Set-Cookie", cookie.serialize("hull_session", sid, { secure = false }))
    return 0
end

-- Pre-body middleware (runs before req.body is read):
--   1. CSP nonce. Populates req.ctx.csp_nonce + sets the header.
--   2. Session bootstrap. Populates req.ctx.session_id.
app.use("*", "/*", csp.htmx())
app.use("*", "/*", session_bootstrap)

-- Post-body middleware (runs after req.body is parsed):
--   3. CSRF. Verifies on unsafe methods; injects token on safe.
app.use_post("*", "/*", csrf.middleware({ secret = CSRF_SECRET }))

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
