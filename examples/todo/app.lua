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

local cookie      = require("hull.cookie")
local crypto      = require("hull.crypto")
local csv         = require("hull.csv")
local db          = require("hull.db")
local form        = require("hull.form")
local http_client        = require("hull.http-client")
local i18n        = require("hull.i18n")
local search      = require("hull.search")
local template    = require("hull.template")
local time        = require("hull.time")
local validate    = require("hull.validate")
local auth        = require("hull.middleware.auth")
local csrf        = require("hull.middleware.csrf")
local logger      = require("hull.middleware.logger")
local ratelimit   = require("hull.middleware.ratelimit")
local rbac        = require("hull.middleware.rbac")
local session     = require("hull.middleware.session")
local transaction = require("hull.middleware.transaction")

local log = require("hull.log")
app.manifest({
    hosts = {"127.0.0.1"},  -- allow self-fetch for /api/stats
    modules = {
        "hull/http-server@1",
        "hull/log@1",
        "hull/cookie@1",
        "hull/crypto@1",
        "hull/csv@1",
        "hull/db@1",
        "hull/form@1",
        "hull/http-client@1",
        "hull/i18n@1",
        "hull/search@1",
        "hull/template@1",
        "hull/time@1",
        "hull/validate@1",
        "hull/middleware/auth@1",
        "hull/middleware/csrf@1",
        "hull/middleware/logger@1",
        "hull/middleware/ratelimit@1",
        "hull/middleware/rbac@1",
        "hull/middleware/session@1",
        "hull/middleware/transaction@1",
    },
})

-- ── i18n setup ─────────────────────────────────────────────────────

i18n.load("en", require("./locales/en.json"))
i18n.load("hu", require("./locales/hu.json"))
i18n.locale("en")

-- ── Session setup ──────────────────────────────────────────────────

session.init({ ttl = 3600 })

-- ── RBAC setup ────────────────────────────────────────────────────

rbac.init()
rbac.define_role("user", {"todos.manage"})
rbac.define_role("admin", {"todos.manage", "admin.dashboard"})

-- ── Search index ──────────────────────────────────────────────────

search.create_index("todos", {"title"})
search.reindex("todos", "todos", { columns = {title = "title"}, id_column = "id" })

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
-- Wrap POST mutations in BEGIN IMMEDIATE..COMMIT for atomicity
-- (e.g., register's check+insert becomes atomic, preventing TOCTOU races)
app.use_post("POST", "/*", transaction.middleware())

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
    extra.is_admin = false
    if req.ctx.session then
        extra.is_admin = rbac.has_role(tostring(req.ctx.session.user_id), "admin")
    end

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
        -- search & export
        search_placeholder = i18n.t("index.search_placeholder"),
        search_btn    = i18n.t("index.search"),
        clear_search  = i18n.t("index.clear_search"),
        export_csv    = i18n.t("index.export_csv"),
        -- admin
        nav_admin     = i18n.t("nav.admin"),
        admin_title   = i18n.t("admin.title"),
        admin_name    = i18n.t("admin.name"),
        admin_email   = i18n.t("admin.email"),
        admin_todos   = i18n.t("admin.todos"),
        admin_role    = i18n.t("admin.role"),
        admin_joined  = i18n.t("admin.joined"),
        admin_no_users = i18n.t("admin.no_users"),
        admin_total_users = i18n.t("admin.total_users"),
        no_results    = i18n.t("index.no_results"),
    }

    return template.render(page, extra)
end

-- ── Health check ────────────────────────────────────────────────────

app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- Stats endpoint: uses http_client.async.get() to check own health
app.get("/api/stats", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local port = req.headers["host"]:match(":(%d+)$") or "3000"
    local health = http_client.async.get("http://127.0.0.1:" .. port .. "/health")
    local count = db.query("SELECT COUNT(*) as n FROM todos WHERE user_id = ?",
                           { sess.user_id })
    res:json({
        todo_count = count[1].n,
        server_healthy = health.status == 200,
    })
end)

-- ── Language switch ─────────────────────────────────────────────────

app.get("/lang/:code", function(req, res)
    local code = req.params.code
    if code ~= "en" and code ~= "hu" then code = "en" end
    res:header("Set-Cookie", cookie.serialize("hull.lang", code, {
        path = "/", max_age = 365 * 24 * 3600, httponly = false,
    }))
    -- Redirect back to referrer or home (validate to prevent open redirect)
    local referer = req.headers["referer"]
    local target = (referer and referer:sub(1, 1) == "/") and referer or "/"
    res:redirect(target)
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

    -- Wrap check+insert in a transaction to prevent TOCTOU race on email uniqueness
    local hash = crypto.hash_password(password)
    local user_id
    local ok_txn, txn_err = transaction.try(function()
        local existing = db.query("SELECT id FROM users WHERE email = ?", { email })
        if #existing > 0 then
            error("Email already registered")
        end
        db.exec("INSERT INTO users (email, password_hash, name, created_at) VALUES (?, ?, ?, ?)",
                { email, hash, name, time.now() })
        user_id = db.last_id()
    end)

    if not ok_txn then
        local msg = tostring(txn_err):match("Email already registered") and "Email already registered"
                    or "Registration failed"
        return res:html(render("pages/register.html", req, { error = msg }))
    end

    -- Assign RBAC roles (first user gets admin)
    rbac.assign(tostring(user_id), "user")
    local user_count = db.query("SELECT COUNT(*) as cnt FROM users")
    if user_count[1].cnt == 1 then
        rbac.assign(tostring(user_id), "admin")
    end

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

    local q = req.query and req.query.q or nil
    local todos

    if q and q ~= "" then
        local ok_q, results = pcall(search.query, "todos", q, { limit = 100 })
        if ok_q and #results > 0 then
            local ids = {}
            local placeholders = {}
            for _, r in ipairs(results) do
                ids[#ids + 1] = r.id
                placeholders[#placeholders + 1] = "?"
            end
            ids[#ids + 1] = sess.user_id
            todos = db.query(
                "SELECT * FROM todos WHERE id IN (" .. table.concat(placeholders, ",") ..
                ") AND user_id = ? ORDER BY created_at DESC", ids)
        else
            todos = {}
        end
    else
        todos = db.query(
            "SELECT * FROM todos WHERE user_id = ? ORDER BY created_at DESC",
            { sess.user_id })
    end

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
        search_query = q or "",
        searching  = q ~= nil and q ~= "",
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
    local todo_id = db.last_id()
    search.index("todos", todo_id, { title = title })
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

    local todo_id = tonumber(req.params.id)
    db.exec("DELETE FROM todos WHERE id = ? AND user_id = ?",
            { todo_id, sess.user_id })
    search.remove("todos", todo_id)
    res:redirect("/")
end)

-- ── CSV export ────────────────────────────────────────────────────

app.get("/export", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local todos = db.query(
        "SELECT title, done, created_at FROM todos WHERE user_id = ? ORDER BY created_at DESC",
        { sess.user_id })

    for _, t in ipairs(todos) do
        t.done = t.done == 1 and "yes" or "no"
    end

    local out = csv.encode(todos, { headers = true })
    res:header("Content-Disposition", "attachment; filename=\"todos.csv\"")
    res:text(out)
end)

-- ── Admin dashboard (RBAC-protected) ──────────────────────────────

app.get("/admin", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    if not rbac.has_role(tostring(sess.user_id), "admin") then
        res:status(403):json({ error = "forbidden" })
        return
    end

    local users = db.query([[
        SELECT u.id, u.name, u.email, u.created_at,
               COUNT(t.id) as todo_count
        FROM users u
        LEFT JOIN todos t ON t.user_id = u.id
        GROUP BY u.id
        ORDER BY u.created_at DESC
    ]])

    for _, u in ipairs(users) do
        local roles = rbac.roles(tostring(u.id))
        u.role = #roles > 0 and table.concat(roles, ", ") or "none"
    end

    res:html(render("pages/admin.html", req, {
        users = users,
        has_users = #users > 0,
        user_count = #users,
    }))
end)

log.info("Todo app loaded — routes registered (en/hu i18n, csv, search, rbac)")
