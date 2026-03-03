--
-- Todo App — full auth, CSRF, rate limiting, server-side rendered
--
-- A todo list with user authentication, sessions, CSRF protection,
-- and per-user data isolation. Pure HTML forms, no client-side JS.
--
-- Run:  hull dev examples/todo/app.lua -d /tmp/todo.db
--

local template = require("hull.template")
local session  = require("hull.middleware.session")
local auth     = require("hull.middleware.auth")
local csrf     = require("hull.middleware.csrf")
local ratelimit = require("hull.middleware.ratelimit")
local logger   = require("hull.middleware.logger")

app.manifest({})  -- sandbox: no fs, no env, no outbound HTTP; default CSP

-- ── Session setup ──────────────────────────────────────────────────

session.init({ ttl = 3600 })

-- ── CSRF secret ─────────────────────────────────────────────────────

local function to_hex(s)
    local hex = {}
    for i = 1, #s do hex[i] = string.format("%02x", string.byte(s, i)) end
    return table.concat(hex)
end
local csrf_secret = to_hex(crypto.random(32))

-- ── Middleware stack ─────────────────────────────────────────────────

app.use("*", "/*", logger.middleware({ skip = {"/health"} }))
app.use("*", "/*", auth.session_middleware({ optional = true }))
app.use("POST", "/login", ratelimit.middleware({ limit = 10, window = 60 }))
app.use("POST", "/register", ratelimit.middleware({ limit = 5, window = 60 }))
-- CSRF needs body access → post-body middleware
app.use_post("*", "/*", csrf.middleware({ secret = csrf_secret }))

-- ── Helpers ─────────────────────────────────────────────────────────

local function parse_form(body)
    local params = {}
    if not body then return params end
    for pair in body:gmatch("[^&]+") do
        local k, v = pair:match("^([^=]*)=(.*)$")
        if k then
            k = k:gsub("+", " "):gsub("%%(%x%x)", function(h)
                return string.char(tonumber(h, 16))
            end)
            v = v:gsub("+", " "):gsub("%%(%x%x)", function(h)
                return string.char(tonumber(h, 16))
            end)
            params[k] = v
        end
    end
    return params
end

local function require_session(req, res)
    if not req.ctx.session then
        res:redirect("/login")
        return nil
    end
    return req.ctx.session
end

local function render(page, req, extra)
    extra = extra or {}
    extra.year = time.date():sub(1, 4)
    extra.csrf_token = req.ctx.csrf_token or ""
    extra.user = req.ctx.session
    extra.logged_in = req.ctx.session ~= nil
    return template.render(page, extra)
end

-- ── Health check ────────────────────────────────────────────────────

app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- ── Auth routes ─────────────────────────────────────────────────────

app.get("/login", function(req, res)
    if req.ctx.session then return res:redirect("/") end
    res:html(render("pages/login.html", req, { error = nil }))
end)

app.get("/register", function(req, res)
    if req.ctx.session then return res:redirect("/") end
    res:html(render("pages/register.html", req, { error = nil }))
end)

app.post("/login", function(req, res)
    local form = parse_form(req.body)
    local email = form.email
    local password = form.password

    if not email or email == "" or not password or password == "" then
        return res:html(render("pages/login.html", req, { error = "Email and password are required" }))
    end

    local rows = db.query("SELECT * FROM users WHERE email = ?", { email })
    if #rows == 0 then
        return res:html(render("pages/login.html", req, { error = "Invalid credentials" }))
    end

    local user = rows[1]
    if not crypto.verify_password(password, user.password_hash) then
        return res:html(render("pages/login.html", req, { error = "Invalid credentials" }))
    end

    auth.login(req, res, { user_id = user.id, email = user.email, name = user.name })
    res:redirect("/")
end)

app.post("/register", function(req, res)
    local form = parse_form(req.body)
    local email = form.email
    local password = form.password
    local name = form.name

    if not email or email == "" then
        return res:html(render("pages/register.html", req, { error = "Email is required" }))
    end
    if not password or #password < 8 then
        return res:html(render("pages/register.html", req, { error = "Password must be at least 8 characters" }))
    end
    if not name or name == "" then
        return res:html(render("pages/register.html", req, { error = "Name is required" }))
    end

    local existing = db.query("SELECT id FROM users WHERE email = ?", { email })
    if #existing > 0 then
        return res:html(render("pages/register.html", req, { error = "Email already registered" }))
    end

    local hash = crypto.hash_password(password)
    db.exec("INSERT INTO users (email, password_hash, name, created_at) VALUES (?, ?, ?, ?)",
            { email, hash, name, time.now() })
    local user_id = db.last_id()

    auth.login(req, res, { user_id = user_id, email = email, name = name })
    res:redirect("/")
end)

app.post("/logout", function(req, res)
    auth.logout(req, res)
    res:redirect("/login")
end)

-- ── Todo routes (authenticated) ─────────────────────────────────────

app.get("/", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local todos = db.query(
        "SELECT * FROM todos WHERE user_id = ? ORDER BY created_at DESC",
        { sess.user_id })

    local done_count = 0
    for _, t in ipairs(todos) do
        t.done = (t.done == 1)
        if t.done then done_count = done_count + 1 end
    end

    res:html(render("pages/index.html", req, {
        todos      = todos,
        has_todos  = #todos > 0,
        total      = #todos,
        done_count = done_count,
        remaining  = #todos - done_count,
    }))
end)

app.post("/add", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local form = parse_form(req.body)
    local title = form.title
    if not title or #title == 0 then
        return res:redirect("/")
    end

    if #title > 500 then title = title:sub(1, 500) end

    db.exec("INSERT INTO todos (user_id, title, created_at) VALUES (?, ?, ?)",
            { sess.user_id, title, time.now() })
    res:redirect("/")
end)

app.post("/toggle/:id", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    db.exec(
        "UPDATE todos SET done = CASE WHEN done = 0 THEN 1 ELSE 0 END WHERE id = ? AND user_id = ?",
        { tonumber(req.params.id), sess.user_id })
    res:redirect("/")
end)

app.post("/delete/:id", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    db.exec("DELETE FROM todos WHERE id = ? AND user_id = ?",
            { tonumber(req.params.id), sess.user_id })
    res:redirect("/")
end)

log.info("Todo app loaded — routes registered")
