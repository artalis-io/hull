--
-- hull.cors -- CORS middleware factory
--
-- cors.middleware(opts)                      - returns middleware function
-- cors.is_allowed_origin(origin, origins)    - pure helper, testable
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local cors = {}

--- Check whether an origin is in the allowed list.
-- Returns true if origins contains "*" or the exact origin string.
function cors.is_allowed_origin(origin, origins)
    if not origin or not origins then return false end
    for _, o in ipairs(origins) do
        if o == "*" or o == origin then return true end
    end
    return false
end

--- Create a CORS middleware function for use with app.use().
-- opts.origins: list of allowed origin strings, or {"*"} (required)
-- opts.methods: allowed methods string (default "GET, POST, PUT, DELETE, OPTIONS")
-- opts.headers: allowed headers string (default "Content-Type, Authorization")
-- opts.credentials: boolean, send Allow-Credentials header (default false)
-- opts.max_age: preflight cache max-age in seconds (default 86400)
function cors.middleware(opts)
    opts = opts or {}

    local origins = opts.origins or { "*" }
    local methods = opts.methods or "GET, POST, PUT, DELETE, OPTIONS"
    local headers = opts.headers or "Content-Type, Authorization"
    local credentials = opts.credentials or false
    local max_age = tostring(opts.max_age or 86400)

    return function(req, res)
        local origin = req.headers["origin"]
        if not origin then return 0 end

        if not cors.is_allowed_origin(origin, origins) then return 0 end

        res:header("Access-Control-Allow-Origin", origin)
        res:header("Access-Control-Allow-Methods", methods)
        res:header("Access-Control-Allow-Headers", headers)
        res:header("Access-Control-Max-Age", max_age)

        if credentials then
            res:header("Access-Control-Allow-Credentials", "true")
        end

        -- Handle preflight
        if req.method == "OPTIONS" then
            res:status(204):text("")
            return 1
        end

        return 0
    end
end

return cors
