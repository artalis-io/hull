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

    -- Parity with JS cors M-1: refuse the unsafe `credentials=true` +
    -- wildcard origin combination at factory time. Browsers would
    -- reject the response too; failing fast here surfaces the
    -- misconfiguration in dev.
    if credentials then
        for _, o in ipairs(origins) do
            if o == "*" then
                error("cors: credentials=true is incompatible with origins={'*'}; list explicit origins.")
            end
        end
    end

    return function(req, res)
        local origin = req.headers["origin"]
        if not origin then return 0 end

        -- L-1: defense-in-depth. Reject any header with CR/LF/NUL before
        -- reflecting it. Keel's response layer also strips these, but
        -- stdlib should not depend on that contract.
        if origin:find("[\r\n%z]") then return 0 end

        if not cors.is_allowed_origin(origin, origins) then return 0 end

        res:header("Access-Control-Allow-Origin", origin)
        res:header("Vary", "Origin")

        if credentials then
            res:header("Access-Control-Allow-Credentials", "true")
        end

        -- Preflight: send method/header/max-age headers + 204
        if req.method == "OPTIONS" then
            res:header("Access-Control-Allow-Methods", methods)
            res:header("Access-Control-Allow-Headers", headers)
            res:header("Access-Control-Max-Age", max_age)
            res:status(204):text("")
            return 1
        end

        return 0
    end
end

return cors
