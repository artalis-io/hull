--- Authentication middleware factories (session + JWT) and login/logout helpers.
--
-- @module hull.middleware.auth
-- @license AGPL-3.0-or-later
--
-- @usage
-- local auth = require("hull.middleware.auth")
-- local session = require("hull.middleware.session")
-- session.init()
--
-- -- Cookie-based session auth for /app/*; redirect to /login on failure
-- app.use("*", "/app/*", auth.session_middleware({ login_path = "/login" }))
--
-- -- Bearer-token JWT auth for /api/*
-- app.use("*", "/api/*", auth.jwt_middleware({ secret = env.get("JWT_SECRET") }))

local cookie = require("hull.cookie")
local session = require("hull.middleware.session")
local jwt_mod = require("hull.jwt")

local auth = {}

--- Session-based authentication middleware.
--
-- On match, populates `req.ctx.session_id` and `req.ctx.session` (the
-- session data table). On miss/failure, either:
--   - sends `401 {"error": "..."}` (default), OR
--   - redirects to `opts.login_path` if set (302), OR
--   - returns 0 to continue if `opts.optional = true`.
--
-- @tparam[opt] table opts  Options:
--
--   - `cookie_name` (string, default `"hull_session"`)
--   - `optional`    (boolean, default `false`)
--   - `login_path`  (string, default `nil`): on failure, redirect here
--
-- @treturn function  Middleware `(req, res) -> integer`.
-- @usage
-- app.use("*", "/app/*", auth.session_middleware({ login_path = "/login" }))
function auth.session_middleware(opts)
    opts = opts or {}
    local cookie_name = opts.cookie_name or "hull_session"
    local optional = opts.optional or false
    local login_path = opts.login_path

    return function(req, res)
        -- Parse cookies from request
        local cookies = cookie.parse(req.headers["cookie"])
        local session_id = cookies[cookie_name]

        if session_id then
            local data = session.load(session_id)
            if data then
                req.ctx.session = data
                req.ctx.session_id = session_id
                return 0
            end
        end

        -- No valid session
        if optional then
            return 0
        end

        if login_path then
            res:redirect(login_path)
            return 1
        end

        res:status(401):json({ error = "authentication required" })
        return 1
    end
end

--- JWT Bearer-token authentication middleware.
--
-- Reads `Authorization: Bearer <token>`, verifies it via @{hull.jwt.verify},
-- and on success populates `req.ctx.user` with the decoded payload.
-- On failure sends `401 {"error":"..."}` (or returns 0 if
-- `opts.optional = true`).
--
-- Unlike `session_middleware`, this one is **stateless** — no DB lookup
-- per request. Suitable for API-Bearer workflows; CSRF protection is
-- NOT needed for Bearer auth (tokens aren't sent automatically by
-- browsers).
--
-- @tparam table opts  Options:
--
--   - `secret`   (string, **required**): HMAC-SHA256 secret used by @{hull.jwt.verify}.
--   - `optional` (boolean, default `false`): continue without a valid token.
--
-- @treturn function  Middleware `(req, res) -> integer`.
-- @raise At factory time if `opts.secret` is missing.
function auth.jwt_middleware(opts)
    opts = opts or {}
    if not opts.secret then
        error("auth.jwt_middleware requires opts.secret")
    end

    local secret = opts.secret
    local optional = opts.optional or false

    return function(req, res)
        -- Read Authorization: Bearer <token>
        local auth_header = req.headers["authorization"]
        if not auth_header then
            if optional then
                return 0
            end
            res:status(401):json({ error = "missing authorization header" })
            return 1
        end

        -- Extract Bearer token
        local token = auth_header:match("^[Bb]earer%s+(.+)$")
        if not token then
            if optional then
                return 0
            end
            res:status(401):json({ error = "invalid authorization format" })
            return 1
        end

        -- Verify JWT
        local payload, err = jwt_mod.verify(token, secret)
        if not payload then
            if optional then
                return 0
            end
            res:status(401):json({ error = err or "invalid token" })
            return 1
        end

        -- Set user context
        req.ctx.user = payload
        return 0
    end
end

--- Create a session and set the auth cookie on the response.
--
-- @tparam table _req         Request (currently unused; reserved for future use).
-- @tparam table res          Response — `Set-Cookie` is added here.
-- @tparam table user_data    Data to persist in the session, e.g. `{ user_id = 1 }`.
-- @tparam[opt] table opts    Options:
--
--   - `cookie_name` (string, default `"hull_session"`)
--   - `cookie_opts` (table): forwarded to @{hull.cookie.serialize}.
--
-- @treturn string  Newly-created session id (hex).
-- @usage
-- app.post("/login", function(req, res)
--     local user = authenticate(req)
--     if not user then return res:status(401):json({ error = "bad credentials" }) end
--     auth.login(req, res, { user_id = user.id })
--     res:json({ ok = true })
-- end)
function auth.login(_req, res, user_data, opts)
    opts = opts or {}
    local cookie_name = opts.cookie_name or "hull_session"
    local cookie_opts = opts.cookie_opts or {}

    local session_id = session.create(user_data)

    -- Set the session cookie
    local serialized = cookie.serialize(cookie_name, session_id, cookie_opts)
    res:header("Set-Cookie", serialized)

    return session_id
end

--- Destroy the current session and clear the auth cookie.
--
-- @tparam table req           Request — reads the session cookie.
-- @tparam table res           Response — emits a `Set-Cookie` that clears it.
-- @tparam[opt] table opts     Options:
--
--   - `cookie_name` (string, default `"hull_session"`)
--   - `cookie_opts` (table): forwarded to @{hull.cookie.clear} so `path`
--     / `domain` match the original cookie.
function auth.logout(req, res, opts)
    opts = opts or {}
    local cookie_name = opts.cookie_name or "hull_session"
    local cookie_opts = opts.cookie_opts or {}

    -- Get session ID from cookie
    local cookies = cookie.parse(req.headers["cookie"])
    local session_id = cookies[cookie_name]

    -- Destroy server-side session
    if session_id then
        session.destroy(session_id)
    end

    -- Clear the cookie
    local cleared = cookie.clear(cookie_name, cookie_opts)
    res:header("Set-Cookie", cleared)
end

return auth
