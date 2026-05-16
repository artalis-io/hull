--- Stateless CSRF tokens via HMAC-SHA256.
--
-- @module hull.middleware.csrf
-- @license AGPL-3.0-or-later
--
-- Tokens are `hex_timestamp.hmac_hex` where the HMAC covers
-- `session_id .. ":" .. timestamp`. No DB storage — verification is
-- pure HMAC + age check.
--
-- Only relevant for cookie-based session auth. JWT Bearer auth does
-- NOT need CSRF protection (browsers don't auto-send Bearer tokens).

local csrf = {}

--- URL-decode a percent-encoded string (e.g. form body values).
local function url_decode(s)
    s = s:gsub("+", " ")
    s = s:gsub("%%(%x%x)", function(hex)
        return string.char(tonumber(hex, 16))
    end)
    return s
end

--- Convert a raw string to hex representation.
local function str_to_hex(s)
    local hex = {}
    for i = 1, #s do
        hex[i] = string.format("%02x", string.byte(s, i))
    end
    return table.concat(hex)
end

--- Constant-time comparison of two strings.
-- Note: length leak is acceptable — both inputs are always fixed-length HMAC outputs
local function constant_time_compare(a, b)
    if #a ~= #b then return false end
    local diff = 0
    for i = 1, #a do
        diff = diff | (string.byte(a, i) ~ string.byte(b, i))
    end
    return diff == 0
end

--- Generate a CSRF token for `(session_id, secret)`.
--
-- @tparam string session_id  Session id (or any per-user secret nonce).
-- @tparam string secret      HMAC key.
-- @treturn string  Token in `hex_timestamp.hmac_hex` form. Embed in
--   form `<input type="hidden">` or send via `X-CSRF-Token` header.
-- @usage
-- local token = csrf.generate(req.ctx.session_id, app_secret)
function csrf.generate(session_id, secret)
    local ts = tostring(time.now())
    local ts_hex = str_to_hex(ts)

    local message = session_id .. ":" .. ts
    local key_hex = str_to_hex(secret)
    local mac = crypto.hmac_sha256(message, key_hex)

    return ts_hex .. "." .. mac
end

--- Verify a CSRF token.
--
-- Constant-time HMAC comparison + age check. Tokens older than
-- `max_age` are rejected even if the HMAC matches.
--
-- @tparam string token
-- @tparam string session_id
-- @tparam string secret
-- @tparam[opt] integer max_age  Max token age in seconds (default `3600`).
-- @treturn boolean  `true` if valid.
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

--- Build a CSRF middleware function. Register with `app.use_post()`.
--
-- Safe methods (`GET`/`HEAD`/`OPTIONS`): generates a token and attaches
-- it to `req.ctx.csrf_token` for templates to embed. Returns `0`.
--
-- Unsafe methods: reads the token from `X-CSRF-Token` header or `_csrf`
-- form field, verifies it, and sends `403` on failure. Returns `1` on
-- short-circuit, `0` on success.
--
-- Body parsing caps: 1 MiB body, 256 form pairs, 4 KiB per pair —
-- bounded work per unsafe request. Large multipart uploads should be
-- parsed before this middleware runs.
--
-- @tparam table opts  Options:
--
--   - `secret`       (string, **required**): HMAC secret.
--   - `session_key`  (string, default `"session_id"`): key in `req.ctx`.
--   - `max_age`      (integer, default `3600`).
--   - `safe_methods` (`{string,...}`, default `{"GET","HEAD","OPTIONS"}`).
--   - `header_name`  (string, default `"x-csrf-token"`).
--   - `field_name`   (string, default `"_csrf"`).
--
-- @treturn function  Middleware `(req, res) -> integer`.
-- @raise If `opts.secret` is missing.
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
            -- Generate a token and attach it to ctx for templates.
            -- L-5: a metatable-based lazy variant was considered but
            -- would conflict with downstream middleware that also
            -- inspects req.ctx. HMAC-SHA256 here is sub-millisecond
            -- and the audit's "wasted" qualification was a perf
            -- observation, not a correctness issue.
            local sid = req.ctx[session_key]
            if sid then
                req.ctx.csrf_token = csrf.generate(sid, secret)
            end
            return 0
        end

        -- CSRF only applies to authenticated sessions. Unauthenticated POST
        -- requests pass through — handlers must independently verify authentication.
        local sid = req.ctx[session_key]
        if not sid then
            return 0
        end

        -- Look for token in header first, then body field
        local token = req.headers[header_name]
        if not token and req.body then
            -- M-5: cap body size + max pairs to prevent DoS via a giant
            -- form body. 1 MiB / 256 pairs is generous for any real form.
            local body = req.body
            if #body > 1048576 then
                res:status(413):json({ error = "csrf: body too large" })
                return 1
            end
            local pair_count = 0
            -- Phase 6 audit M-4: per-pair-byte cap. Without this, a body
            -- with a single 1 MiB pair (under the 1 MiB total cap above)
            -- would force a 1 MiB url_decode on every unsafe-method
            -- request. 4 KiB is generous for any sane form field.
            local MAX_PAIR_BYTES = 4096
            for pair in body:gmatch("[^&]+") do
                pair_count = pair_count + 1
                if pair_count > 256 then break end
                if #pair > MAX_PAIR_BYTES then
                    -- Skip oversized pairs entirely; the _csrf field
                    -- itself is short by spec, so a real CSRF token
                    -- will never exceed this limit.
                    goto continue
                end
                local eq = pair:find("=", 1, true)
                if eq then
                    local key = pair:sub(1, eq - 1)
                    local val = pair:sub(eq + 1)
                    if key == field_name then
                        token = url_decode(val)
                        break
                    end
                end
                ::continue::
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
