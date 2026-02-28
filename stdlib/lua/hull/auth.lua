--
-- hull.auth -- Authentication middleware factories
--
-- Provides session-based and JWT-based authentication middleware,
-- plus login/logout helpers.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local cookie = require("hull.cookie")
local session = require("hull.session")
local jwt_mod = require("hull.jwt")

local auth = {}

--- Create a session-based authentication middleware.
-- opts.cookie_name: cookie name (default "hull_session")
-- opts.optional: if true, continue even without valid session (default false)
-- opts.login_path: if set, redirect to this path on auth failure instead of 401
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

--- Create a JWT-based authentication middleware.
-- opts.secret (required): HMAC secret for verification
-- opts.optional: if true, continue even without valid token (default false)
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

--- Log in a user by creating a session and setting the cookie.
-- req, res: the current request/response objects
-- user_data: table of data to store in the session (e.g. { user_id = 1 })
-- opts.cookie_name: cookie name (default "hull_session")
-- opts.cookie_opts: additional cookie options (path, secure, etc.)
-- opts.ttl: session TTL override (passed to session.create indirectly via session.init)
-- Returns the session ID.
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

--- Log out the current user by destroying the session and clearing the cookie.
-- req, res: the current request/response objects
-- opts.cookie_name: cookie name (default "hull_session")
-- opts.cookie_opts: additional cookie options for clearing
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
