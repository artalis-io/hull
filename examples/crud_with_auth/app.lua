-- CRUD with Auth — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- Tasks API with session-based auth — each user only sees their own tasks

local session = require("hull.session")
local auth = require("hull.auth")

-- Initialize database
session.init({ ttl = 3600 })

db.exec([[CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    email TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    name TEXT NOT NULL,
    created_at INTEGER
)]])

db.exec([[CREATE TABLE IF NOT EXISTS tasks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    title TEXT NOT NULL,
    done INTEGER DEFAULT 0,
    created_at INTEGER,
    FOREIGN KEY (user_id) REFERENCES users(id)
)]])

db.exec("CREATE INDEX IF NOT EXISTS idx_tasks_user ON tasks (user_id)")

-- Load session on every request (optional — won't block unauthenticated)
app.use("*", "/*", auth.session_middleware({ optional = true }))

-- Helper: require session or respond 401
local function require_session(req, res)
    if not req.ctx.session then
        res:status(401):json({ error = "authentication required" })
        return nil
    end
    return req.ctx.session
end

-- Health check
app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- ── Auth routes ─────────────────────────────────────────────────────

app.post("/register", function(req, res)
    local body = json.decode(req.body)
    if not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local email = body.email
    local password = body.password
    local name = body.name

    if not email or email == "" then
        return res:status(400):json({ error = "email is required" })
    end
    if not password or #password < 8 then
        return res:status(400):json({ error = "password must be at least 8 characters" })
    end
    if not name or name == "" then
        return res:status(400):json({ error = "name is required" })
    end

    local existing = db.query("SELECT id FROM users WHERE email = ?", { email })
    if #existing > 0 then
        return res:status(409):json({ error = "email already registered" })
    end

    local hash = crypto.hash_password(password)
    db.exec("INSERT INTO users (email, password_hash, name, created_at) VALUES (?, ?, ?, ?)",
            { email, hash, name, time.now() })
    local id = db.last_id()

    res:status(201):json({ id = id, email = email, name = name })
end)

app.post("/login", function(req, res)
    local body = json.decode(req.body)
    if not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local email = body.email
    local password = body.password
    if not email or not password then
        return res:status(400):json({ error = "email and password are required" })
    end

    local rows = db.query("SELECT * FROM users WHERE email = ?", { email })
    if #rows == 0 then
        return res:status(401):json({ error = "invalid credentials" })
    end

    local user = rows[1]
    if not crypto.verify_password(password, user.password_hash) then
        return res:status(401):json({ error = "invalid credentials" })
    end

    auth.login(req, res, { user_id = user.id, email = user.email })
    res:json({ id = user.id, email = user.email, name = user.name })
end)

app.post("/logout", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    auth.logout(req, res)
    res:json({ ok = true })
end)

app.get("/me", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local rows = db.query("SELECT id, email, name, created_at FROM users WHERE id = ?",
                          { sess.user_id })
    if #rows == 0 then
        return res:status(404):json({ error = "user not found" })
    end

    res:json(rows[1])
end)

-- ── Task CRUD (scoped to current user) ──────────────────────────────

-- List my tasks
app.get("/tasks", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local tasks = db.query("SELECT id, title, done, created_at FROM tasks WHERE user_id = ? ORDER BY created_at DESC",
                           { sess.user_id })
    res:json(tasks)
end)

-- Get one of my tasks
app.get("/tasks/:id", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local rows = db.query("SELECT id, title, done, created_at FROM tasks WHERE id = ? AND user_id = ?",
                          { req.params.id, sess.user_id })
    if #rows == 0 then
        return res:status(404):json({ error = "task not found" })
    end

    res:json(rows[1])
end)

-- Create a task
app.post("/tasks", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local body = json.decode(req.body)
    if not body or not body.title then
        return res:status(400):json({ error = "title is required" })
    end

    db.exec("INSERT INTO tasks (user_id, title, created_at) VALUES (?, ?, ?)",
            { sess.user_id, body.title, time.now() })
    local id = db.last_id()

    res:status(201):json({ id = id, title = body.title, done = 0 })
end)

-- Update a task
app.put("/tasks/:id", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local body = json.decode(req.body)
    if not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local done = 0
    if body.done then done = 1 end

    local changes = db.exec("UPDATE tasks SET title = ?, done = ? WHERE id = ? AND user_id = ?",
                            { body.title, done, req.params.id, sess.user_id })
    if changes == 0 then
        return res:status(404):json({ error = "task not found" })
    end

    res:json({ ok = true })
end)

-- Delete a task
app.del("/tasks/:id", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    local changes = db.exec("DELETE FROM tasks WHERE id = ? AND user_id = ?",
                            { req.params.id, sess.user_id })
    if changes == 0 then
        return res:status(404):json({ error = "task not found" })
    end

    res:json({ ok = true })
end)

log.info("CRUD with Auth loaded — routes registered")
