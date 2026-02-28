--
-- hull.cookie -- Cookie parsing and serialization
--
-- Pure functions for HTTP cookie handling.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local cookie = {}

--- Parse a Cookie header string into a key-value table.
-- "session=abc; theme=dark" -> { session = "abc", theme = "dark" }
function cookie.parse(header_string)
    local result = {}
    if not header_string or header_string == "" then
        return result
    end

    for pair in string.gmatch(header_string, "[^;]+") do
        -- Trim leading/trailing whitespace
        pair = pair:match("^%s*(.-)%s*$")
        if pair ~= "" then
            local eq = pair:find("=", 1, true)
            if eq then
                local name = pair:sub(1, eq - 1):match("^%s*(.-)%s*$")
                local value = pair:sub(eq + 1):match("^%s*(.-)%s*$")
                if name ~= "" then
                    result[name] = value
                end
            end
        end
    end

    return result
end

--- Serialize a cookie name/value pair with options into a Set-Cookie header value.
-- Defaults: HttpOnly=true, Secure=true, SameSite=Lax, Path=/
function cookie.serialize(name, value, opts)
    opts = opts or {}

    local parts = { name .. "=" .. (value or "") }

    -- Path (default "/")
    local path = opts.path
    if path == nil then path = "/" end
    if path then
        parts[#parts + 1] = "Path=" .. path
    end

    -- HttpOnly (default true)
    local httponly = opts.httponly
    if httponly == nil then httponly = true end
    if httponly then
        parts[#parts + 1] = "HttpOnly"
    end

    -- Secure (default true)
    local secure = opts.secure
    if secure == nil then secure = true end
    if secure then
        parts[#parts + 1] = "Secure"
    end

    -- SameSite (default "Lax")
    local samesite = opts.samesite
    if samesite == nil then samesite = "Lax" end
    if samesite then
        parts[#parts + 1] = "SameSite=" .. samesite
    end

    -- Max-Age
    if opts.max_age then
        parts[#parts + 1] = "Max-Age=" .. tostring(opts.max_age)
    end

    -- Domain
    if opts.domain then
        parts[#parts + 1] = "Domain=" .. opts.domain
    end

    -- Expires
    if opts.expires then
        parts[#parts + 1] = "Expires=" .. opts.expires
    end

    return table.concat(parts, "; ")
end

--- Clear a cookie by setting Max-Age=0 and an empty value.
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
