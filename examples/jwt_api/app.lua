-- JWT API — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- Token-based auth API: register, login, protected routes using Bearer tokens

local jwt = require("hull.jwt")

local JWT_SECRET = "change-me-in-production"

-- Initialize database
db.exec([[CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    email TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    name TEXT NOT NULL,
    created_at INTEGER
)]])

-- Middleware: extract and verify JWT on every request (optional — won't block)
app.use("*", "/*", function(req, _res)
    local auth_header = req.headers["authorization"]
    if not auth_header then return 0 end

    local token = auth_header:match("^[Bb]earer%s+(.+)$")
    if not token then return 0 end

    local payload, _err = jwt.verify(token, JWT_SECRET)
    if payload then
        req.ctx.user = payload
    end
    return 0
end)

-- Helper: require authenticated user or respond 401
local function require_auth(req, res)
    if not req.ctx.user then
        res:status(401):json({ error = "authentication required" })
        return nil
    end
    return req.ctx.user
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

    local existing = db.query("SELECT id FROM users WHERE email = ?", { email })
    if #existing > 0 then
        return res:status(409):json({ error = "email already registered" })
    end

    local hash = crypto.hash_password(password)
    db.exec("INSERT INTO users (email, password_hash, name, created_at) VALUES (?, ?, ?, ?)",
            { email, hash, name, time.now() })
    local id = db.lastId()

    res:status(201):json({ id = id, email = email, name = name })
end)

-- Login — returns JWT token
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

    -- Issue JWT (expires in 1 hour — relative exp < 2e9 is auto-converted)
    local token = jwt.sign({
        sub = user.id,
        email = user.email,
        exp = 3600,
    }, JWT_SECRET)

    res:json({ token = token, user = { id = user.id, email = user.email, name = user.name } })
end)

-- Get current user (requires token)
app.get("/me", function(req, res)
    local user = require_auth(req, res)
    if not user then return end

    local rows = db.query("SELECT id, email, name, created_at FROM users WHERE id = ?",
                          { user.sub })
    if #rows == 0 then
        return res:status(404):json({ error = "user not found" })
    end

    res:json(rows[1])
end)

-- Refresh token (requires valid token, returns new one)
app.post("/refresh", function(req, res)
    local user = require_auth(req, res)
    if not user then return end

    local token = jwt.sign({
        sub = user.sub,
        email = user.email,
        exp = 3600,
    }, JWT_SECRET)

    res:json({ token = token })
end)

log.info("JWT API loaded — routes registered")
