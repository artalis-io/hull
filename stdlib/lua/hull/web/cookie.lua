--- HTTP cookie parsing and serialization.
--
-- @module hull.web.cookie
-- @license AGPL-3.0-or-later

local _text = require("hull._text")   -- linear trims (see hull._text)

local cookie = {}

-- A value as the JS sibling (and so a browser's document.cookie user)
-- decodes it: surrounding double quotes stripped, then %XX escapes decoded.
-- A malformed escape, or bytes that do not decode to UTF-8, leave the value
-- as it was - what decodeURIComponent's throw does in JS. The two runtimes
-- used to read one cookie differently (audit 9).
local function decode_value(v)
    if #v >= 2 and v:sub(1, 1) == '"' and v:sub(-1) == '"' then
        v = v:sub(2, -2)
    end
    if not v:find("%", 1, true) then return v end
    for pos in v:gmatch("()%%") do
        if not v:sub(pos + 1, pos + 2):match("^%x%x$") then return v end
    end
    local out = v:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)
    if not utf8.len(out) then return v end
    return out
end

-- encodeURIComponent's set: everything but A-Z a-z 0-9 - _ . ! ~ * ' ( )
-- is written %XX (upper-case hex), as the JS sibling's serialize writes it.
local function encode_value(v)
    return (v:gsub("[^%w%-_%.!~%*'%(%)]", function(c)
        return string.format("%%%02X", string.byte(c))
    end))
end

--- Parse a `Cookie` header string into a name-value table.
--
-- Each cookie is split on `=`, trimmed of surrounding whitespace, and its
-- value decoded as the JS sibling decodes it (quotes stripped, %XX escapes
-- decoded; a malformed one leaves the value as sent). When a name appears
-- more than once the FIRST occurrence wins (audit 9): browsers send the
-- cookie with the most specific path first, and a later duplicate - one a
-- sibling subdomain or a path-scoped page set - used to override it.
-- Empty input or `nil` returns an empty table (never errors).
--
-- @tparam string|nil header_string  Value of the inbound `Cookie` header.
-- @treturn {[string]=string} Parsed cookies. Empty table when input is `nil` or empty.
-- @usage
-- local cookies = cookie.parse(req.headers["cookie"])
-- if cookies.session_id then ... end
function cookie.parse(header_string)
    local result = {}
    if not header_string or header_string == "" then
        return result
    end

    for pair in string.gmatch(header_string, "[^;]+") do
        -- Trim leading/trailing whitespace
        pair = _text.trim(pair)
        if pair ~= "" then
            local eq = pair:find("=", 1, true)
            if eq then
                local name = _text.trim(pair:sub(1, eq - 1))
                local value = _text.trim(pair:sub(eq + 1))
                if name ~= "" and result[name] == nil then
                    result[name] = decode_value(value)
                end
            end
        end
    end

    return result
end

--- Serialize a cookie into a `Set-Cookie` header value.
--
-- @tparam string name   Cookie name.
-- @tparam string value  Cookie value. Written percent-encoded (the
--   encodeURIComponent set, as the JS sibling writes it), so @{parse} - in
--   either runtime - reads back exactly this value.
-- @tparam[opt] table opts  Options:
--
--   - `path`     (string, default `"/"`)
--   - `httponly` (boolean, default `true`)
--   - `secure`   (boolean, default `true`; set `false` explicitly for local
--                 HTTP dev). Secure-by-default matches the code.
--   - `samesite` (string, default `"Lax"`; one of `"Lax"|"Strict"|"None"`)
--   - `max_age`  (integer, seconds; omits attribute if `nil`)
--   - `domain`   (string)
--   - `expires`  (string, RFC 1123 date; omits if `nil`)
--
-- @treturn string  `Set-Cookie` header value. Pair with `res:header("Set-Cookie", v)`.
-- @usage
-- res:header("Set-Cookie", cookie.serialize("session", sid, { secure = true }))
function cookie.serialize(name, value, opts)
    opts = opts or {}

    -- Validate cookie name (RFC 6265 token)
    if not name or name == "" or name:find("[%c;= ,]") then
        error("cookie: invalid cookie name")
    end
    -- Reject control characters and semicolons in value
    if value and value:find("[%c;]") then
        error("cookie: invalid cookie value")
    end

    local parts = { name .. "=" .. encode_value(value or "") }

    -- Path (default "/")
    local path = opts.path
    if path == nil then path = "/" end
    if path then
        -- A path from user input could otherwise add attributes
        -- ("/; Domain=evil.com"). Same refusal as the JS sibling.
        if type(path) ~= "string" or path:find("[%c;]") then
            error("cookie: invalid path (contains ';' or a control character)")
        end
        parts[#parts + 1] = "Path=" .. path
    end

    -- HttpOnly (default true)
    local httponly = opts.httponly
    if httponly == nil then httponly = true end
    if httponly then
        parts[#parts + 1] = "HttpOnly"
    end

    -- Secure=true by default. Set secure=false explicitly for local HTTP dev.
    local secure = opts.secure
    if secure == nil then secure = true end
    if secure then
        parts[#parts + 1] = "Secure"
    end

    -- SameSite (default "Lax")
    local samesite = opts.samesite
    if samesite == nil then samesite = "Lax" end
    if samesite then
        local canon = type(samesite) == "string"
            and ({ strict = "Strict", lax = "Lax", none = "None" })[samesite:lower()]
        if not canon then
            error("cookie: samesite must be Strict, Lax or None")
        end
        parts[#parts + 1] = "SameSite=" .. canon
    end

    -- Max-Age: an integer (a string went in verbatim, attributes and all)
    if opts.max_age ~= nil then
        local n = tonumber(opts.max_age)
        if type(opts.max_age) ~= "number" or not n or n ~= n
           or n == math.huge or n == -math.huge then
            error("cookie: max_age must be a number")
        end
        parts[#parts + 1] = "Max-Age=" .. string.format("%d", math.floor(n))
    end

    -- Domain - RFC 6265 attribute syntax. Accepts an optional leading "."
    -- (legacy subdomain-matching form, still in widespread use). Rejects
    -- length > 253, double dots, trailing dots, and any non-host characters.
    if opts.domain then
        local d = opts.domain
        if #d > 253
           or not d:match("^%.?[a-zA-Z0-9][a-zA-Z0-9%-%.]*[a-zA-Z0-9]$")
           or d:find("%.%.", 1, false) then
            error("cookie: invalid domain")
        end
        parts[#parts + 1] = "Domain=" .. d
    end

    -- Expires
    if opts.expires then
        if type(opts.expires) ~= "string" or opts.expires:find("[%c;]") then
            error("cookie: invalid expires value")
        end
        parts[#parts + 1] = "Expires=" .. opts.expires
    end

    return table.concat(parts, "; ")
end

--- Build a `Set-Cookie` value that deletes the named cookie.
--
-- Sets `Max-Age=0` and an empty value. The browser deletes the cookie
-- on receipt.
--
-- @tparam string name  Cookie name to clear.
-- @tparam[opt] table opts  Same options as @{cookie.serialize}; `path` /
--   `domain` should match the original to ensure the right cookie is cleared.
-- @treturn string  `Set-Cookie` header value.
-- @usage
-- res:header("Set-Cookie", cookie.clear("session", { path = "/" }))
function cookie.clear(name, opts)
    opts = opts or {}
    local clear_opts = {}
    -- Copy relevant options
    clear_opts.path = opts.path
    clear_opts.domain = opts.domain
    clear_opts.httponly = opts.httponly
    clear_opts.secure = opts.secure
    clear_opts.samesite = opts.samesite
    clear_opts.max_age = 0
    return cookie.serialize(name, "", clear_opts)
end

return cookie
