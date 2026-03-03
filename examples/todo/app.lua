--
-- Todo App — full auth, CSRF, rate limiting, server-side rendered, i18n
--
-- A todo list with user authentication, sessions, CSRF protection,
-- per-user data isolation, and English/Hungarian language support.
-- Pure HTML forms, no client-side JS.
--
-- Graceful shutdown: Ctrl+C triggers drain mode (finishes in-flight
-- requests within 5s). Second Ctrl+C stops immediately.
--
-- Run:  hull dev examples/todo/app.lua -d /tmp/todo.db
--

local template  = require("hull.template")
local form      = require("hull.form")
local validate  = require("hull.validate")
local session   = require("hull.middleware.session")
local auth      = require("hull.middleware.auth")
local csrf      = require("hull.middleware.csrf")
local ratelimit = require("hull.middleware.ratelimit")
local logger    = require("hull.middleware.logger")
local cookie    = require("hull.cookie")
local i18n      = require("hull.i18n")

app.manifest({})  -- sandbox: no fs, no env, no outbound HTTP; default CSP

-- ── i18n setup ─────────────────────────────────────────────────────

i18n.load("en", require("./locales/en"))
i18n.load("hu", require("./locales/hu"))
i18n.locale("en")

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

-- Language detection middleware: cookie → Accept-Language → default "en"
app.use("*", "/*", function(req, _res)
    local cookies = cookie.parse(req.headers["cookie"] or "")
    local lang = cookies["hull.lang"]
    if not lang or (lang ~= "en" and lang ~= "hu") then
        lang = i18n.detect(req.headers["accept-language"]) or "en"
    end
    i18n.locale(lang)
    return 0
end)

app.use("POST", "/login", ratelimit.middleware({ limit = 10, window = 60 }))
app.use("POST", "/register", ratelimit.middleware({ limit = 5, window = 60 }))
-- CSRF needs body access → post-body middleware
app.use_post("*", "/*", csrf.middleware({ secret = csrf_secret }))

-- ── Helpers ─────────────────────────────────────────────────────────

local function require_session(req, res)
    if not req.ctx.session then
        res:redirect("/login")
        return nil
    end
    return req.ctx.session
end

--- Build template context with translated strings.
local function render(page, req, extra)
    extra = extra or {}
    extra.year = time.date():sub(1, 4)
    extra.csrf_token = req.ctx.csrf_token or ""
    extra.user = req.ctx.session
    extra.logged_in = req.ctx.session ~= nil
    extra.lang = i18n.locale()

    -- Inject translated strings as t.* for templates
    extra.t = {
        -- site
        site_title    = i18n.t("site.title"),
        powered_by    = i18n.t("site.powered_by"),
        -- nav
        nav_brand     = i18n.t("nav.brand"),
        nav_tasks     = i18n.t("nav.tasks"),
        nav_logout    = i18n.t("nav.logout"),
        nav_login     = i18n.t("nav.login"),
        nav_register  = i18n.t("nav.register"),
        -- index
        my_todos      = i18n.t("index.title"),
        placeholder   = i18n.t("index.placeholder"),
        add           = i18n.t("index.add"),
        remaining     = i18n.t("index.remaining"),
        completed     = i18n.t("index.completed"),
        total         = i18n.t("index.total"),
        empty         = i18n.t("index.empty"),
        -- login
        login_title   = i18n.t("login.page_title"),
        login_email   = i18n.t("login.email"),
        login_pass    = i18n.t("login.password"),
        login_submit  = i18n.t("login.submit"),
        no_account    = i18n.t("login.no_account"),
        register_link = i18n.t("login.register_link"),
        -- register
        reg_title     = i18n.t("register.page_title"),
        reg_name      = i18n.t("register.name"),
        reg_email     = i18n.t("register.email"),
        reg_pass      = i18n.t("register.password"),
        reg_submit    = i18n.t("register.submit"),
        has_account   = i18n.t("register.has_account"),
        login_link    = i18n.t("register.login_link"),
        -- language names
        lang_en       = i18n.t("lang.en"),
        lang_hu       = i18n.t("lang.hu"),
    }

    return template.render(page, extra)
end

-- ── Health check ────────────────────────────────────────────────────

app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- ── Language switch ─────────────────────────────────────────────────

app.get("/lang/:code", function(req, res)
    local code = req.params.code
    if code ~= "en" and code ~= "hu" then code = "en" end
    res:header("Set-Cookie", cookie.serialize("hull.lang", code, {
        path = "/", max_age = 365 * 24 * 3600, httponly = false,
    }))
    -- Redirect back to referrer or home
    local referer = req.headers["referer"]
    res:redirect(referer or "/")
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
    local params = form.parse(req.body)
    local ok, errors = validate.check(params, {
        email    = { required = true },
        password = { required = true },
    })
    if not ok then
        local msg = errors.email or errors.password
        return res:html(render("pages/login.html", req, { error = msg }))
    end

    local email = params.email
    local password = params.password

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
    local params = form.parse(req.body)
    local ok, errors = validate.check(params, {
        email    = { required = true },
        password = { required = true, min = 8 },
        name     = { required = true },
    })
    if not ok then
        local msg = errors.email or errors.password or errors.name
        return res:html(render("pages/register.html", req, { error = msg }))
    end

    local email = params.email
    local password = params.password
    local name = params.name

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

    local params = form.parse(req.body)
    local ok, _errors = validate.check(params, {
        title = { required = true },
    })
    if not ok then
        return res:redirect("/")
    end

    local title = params.title
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

log.info("Todo app loaded — routes registered (en/hu i18n)")
