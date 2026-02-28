-- Auth Example — Hull + Lua
--
-- Run: hull app.lua -p 3000
-- Session-based auth API: register, login, logout, protected routes

local session = require("hull.session")
local auth = require("hull.auth")

-- Initialize database
session.init({ ttl = 3600 })

db.exec([[
    CREATE TABLE IF NOT EXISTS users (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        email TEXT UNIQUE NOT NULL,
        password_hash TEXT NOT NULL,
        name TEXT NOT NULL,
        created_at INTEGER
    )
]])

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

-- Register
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

    -- Check uniqueness
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

-- Login
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

-- Logout (requires session)
app.post("/logout", function(req, res)
    local sess = require_session(req, res)
    if not sess then return end

    auth.logout(req, res)
    res:json({ ok = true })
end)

-- Get current user (requires session)
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

log.info("Auth app loaded — routes registered")
