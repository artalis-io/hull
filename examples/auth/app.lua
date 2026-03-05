-- Auth Example — Hull + Lua
--
-- Run: hull app.lua -p 3000
-- Session-based auth API: register, login, logout, protected routes

local validate = require("hull.validate")
local session  = require("hull.middleware.session")
local auth     = require("hull.middleware.auth")

app.manifest({})

-- Initialize sessions
session.init({ ttl = 3600 })

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
    local decode_ok, body = pcall(json.decode, req.body)
    if not decode_ok or not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local ok, errors = validate.check(body, {
        email    = { required = true },
        password = { required = true, min = 8 },
        name     = { required = true },
    })
    if not ok then
        return res:status(400):json({ errors = errors })
    end

    local email = body.email
    local password = body.password
    local name = body.name

    -- Atomic check+insert to prevent TOCTOU race on email uniqueness
    local hash = crypto.hash_password(password)
    local id
    local ok_txn, txn_err = pcall(function()
        db.batch(function()
            local existing = db.query("SELECT id FROM users WHERE email = ?", { email })
            if #existing > 0 then
                error("email already registered")
            end
            db.exec("INSERT INTO users (email, password_hash, name, created_at) VALUES (?, ?, ?, ?)",
                    { email, hash, name, time.now() })
            id = db.last_id()
        end)
    end)

    if not ok_txn then
        if tostring(txn_err):match("email already registered") then
            return res:status(409):json({ error = "email already registered" })
        end
        return res:status(500):json({ error = "registration failed" })
    end

    res:status(201):json({ id = id, email = email, name = name })
end)

-- Login
app.post("/login", function(req, res)
    local decode_ok, body = pcall(json.decode, req.body)
    if not decode_ok or not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local ok, errors = validate.check(body, {
        email    = { required = true },
        password = { required = true },
    })
    if not ok then
        return res:status(400):json({ errors = errors })
    end

    local email = body.email
    local password = body.password

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
