--
-- hull.csrf -- Stateless CSRF token generation and verification
--
-- Tokens are "hex_timestamp.hmac_hex" where the HMAC covers
-- session_id .. ":" .. timestamp.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local csrf = {}

--- Convert a raw string to hex representation.
local function str_to_hex(s)
    local hex = {}
    for i = 1, #s do
        hex[i] = string.format("%02x", string.byte(s, i))
    end
    return table.concat(hex)
end

--- Constant-time comparison of two strings.
local function constant_time_compare(a, b)
    if #a ~= #b then return false end
    local diff = 0
    for i = 1, #a do
        diff = diff | (string.byte(a, i) ~ string.byte(b, i))
    end
    return diff == 0
end

--- Generate a CSRF token for the given session ID and secret.
-- Returns "hex_timestamp.hmac_hex"
function csrf.generate(session_id, secret)
    local ts = tostring(time.now())
    local ts_hex = str_to_hex(ts)

    local message = session_id .. ":" .. ts
    local key_hex = str_to_hex(secret)
    local mac = crypto.hmac_sha256(message, key_hex)

    return ts_hex .. "." .. mac
end

--- Verify a CSRF token against the session ID and secret.
-- max_age: maximum token age in seconds (default 3600)
-- Returns true if valid, false otherwise.
function csrf.verify(token, session_id, secret, max_age)
    max_age = max_age or 3600

    if not token or not session_id or not secret then
        return false
    end

    -- Split token into timestamp_hex and mac
    local dot = token:find(".", 1, true)
    if not dot then
        return false
    end

    local ts_hex = token:sub(1, dot - 1)
    local mac = token:sub(dot + 1)

    if ts_hex == "" or mac == "" then
        return false
    end

    -- Decode hex timestamp back to string
    if #ts_hex % 2 ~= 0 then
        return false
    end
    local ts_chars = {}
    for i = 1, #ts_hex, 2 do
        local byte = tonumber(ts_hex:sub(i, i + 1), 16)
        if not byte then
            return false
        end
        ts_chars[#ts_chars + 1] = string.char(byte)
    end
    local ts_str = table.concat(ts_chars)
    local ts = tonumber(ts_str)
    if not ts then
        return false
    end

    -- Check expiry
    local now = time.now()
    if now - ts > max_age then
        return false
    end

    -- Recompute HMAC and compare
    local message = session_id .. ":" .. ts_str
    local key_hex = str_to_hex(secret)
    local expected_mac = crypto.hmac_sha256(message, key_hex)

    return constant_time_compare(mac, expected_mac)
end

--- Create a CSRF middleware function for use with app.use().
-- opts.secret (required): HMAC secret string
-- opts.session_key: key in req.ctx for session ID (default "session_id")
-- opts.max_age: max token age in seconds (default 3600)
-- opts.safe_methods: methods that skip verification (default {"GET","HEAD","OPTIONS"})
-- opts.header_name: header to read token from (default "x-csrf-token")
-- opts.field_name: form field name for token (default "_csrf")
function csrf.middleware(opts)
    if not opts or not opts.secret then
        error("csrf.middleware requires opts.secret")
    end

    local secret = opts.secret
    local session_key = opts.session_key or "session_id"
    local max_age = opts.max_age or 3600
    local header_name = opts.header_name or "x-csrf-token"
    local field_name = opts.field_name or "_csrf"

    -- Build safe methods lookup
    local safe_list = opts.safe_methods or { "GET", "HEAD", "OPTIONS" }
    local safe_methods = {}
    for _, m in ipairs(safe_list) do
        safe_methods[m] = true
    end

    return function(req, res)
        -- Safe methods skip verification
        if safe_methods[req.method] then
            -- Generate a token and attach it to ctx for templates
            local sid = req.ctx[session_key]
            if sid then
                req.ctx.csrf_token = csrf.generate(sid, secret)
            end
            return 0
        end

        -- Get session ID from context
        local sid = req.ctx[session_key]
        if not sid then
            res:status(403):json({ error = "csrf: no session" })
            return 1
        end

        -- Look for token in header first, then body field
        local token = req.headers[header_name]
        if not token and req.body then
            -- Try to parse as form-encoded body for _csrf field
            local body = req.body
            for pair in body:gmatch("[^&]+") do
                local eq = pair:find("=", 1, true)
                if eq then
                    local key = pair:sub(1, eq - 1)
                    local val = pair:sub(eq + 1)
                    if key == field_name then
                        token = val
                        break
                    end
                end
            end
        end

        if not csrf.verify(token, sid, secret, max_age) then
            res:status(403):json({ error = "csrf: invalid token" })
            return 1
        end

        return 0
    end
end

return csrf
