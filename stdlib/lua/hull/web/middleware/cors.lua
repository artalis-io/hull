--- CORS middleware factory.
--
-- @module hull.web.middleware.cors
-- @license AGPL-3.0-or-later

local cors = {}

--- Check whether an origin is in the allowed list.
--
-- Returns true iff `origins` contains `"*"` or an exact match of @{origin}.
-- Wildcard subdomains (`*.example.com`) are not supported - list each
-- explicit origin.
--
-- @tparam string origin   Incoming `Origin` header value.
-- @tparam {string,...} origins  Allowed origins list.
-- @treturn boolean  `true` if the origin is allowed.
function cors.is_allowed_origin(origin, origins)
    if not origin or not origins then return false end
    for _, o in ipairs(origins) do
        if o == "*" or o == origin then return true end
    end
    return false
end

--- Create a CORS middleware function for use with `app.use()`.
--
-- The returned middleware:
--   - skips requests without an `Origin` header (returns 0, "continue");
--   - rejects with no headers (silently - browser will block) when the
--     origin is not in the allowlist;
--   - on allowed origins, emits `Access-Control-Allow-Origin` + `Vary`
--     + (when `opts.credentials`) `Access-Control-Allow-Credentials`;
--   - on `OPTIONS` preflight: emits methods/headers/max-age and replies
--     `204 No Content` (returns 1, "short-circuit").
--
-- Phase 6 fail-fast: `error()` is raised at factory time if
-- `credentials = true` is combined with `origins = {"*"}` - that
-- combination is silently rejected by browsers and a common bug.
--
-- @tparam[opt] table opts  Options:
--
--   - `origins` (`{string,...}`, default `{"*"}`): allowed origins.
--   - `methods` (string, default `"GET, POST, PUT, DELETE, OPTIONS"`)
--   - `headers` (string, default `"Content-Type, Authorization"`)
--   - `credentials` (boolean, default `false`)
--   - `max_age` (integer, preflight cache seconds, default `86400`)
--
-- @treturn function  Middleware function with signature `(req, res) -> integer`.
-- @raise If `credentials = true` and `origins = {"*"}`.
-- @usage
-- local cors = require("hull.web.middleware.cors")
-- app.use("*", "/api/*", cors.middleware({
--     origins = { "https://app.example.com" },
--     credentials = true,
-- }))
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
