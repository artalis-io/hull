--[[
  hull.kv._util - shared internals for the KV/cache subsystem.

  Coded errors (docs/stdlib_style.md section 1), key/value validation, a
  binary-safe base64 codec (so the SQL backend can store arbitrary bytes in
  portable TEXT columns - Postgres TEXT rejects embedded NULs), and the
  capability-name vocabulary. Consumed by hull.kv, hull.cache, and the
  hull.kv._memstore / hull.kv._sql backends. Internal (underscore-prefixed);
  not a user-facing module.

  SPDX-License-Identifier: AGPL-3.0-or-later
]]

local time = require("hull.time")

local M = {}

-- Coded error: err.code is the stable, branch-on identity. Level 0 so the
-- Lua source position is not prepended to the message.
local _err_mt = { __tostring = function(e) return e.message end }
function M.error(code, message)
    error(setmetatable({ code = code, message = message }, _err_mt), 0)
end

M.MAX_KEY = 1024   -- bytes; bounds the SQL primary key and memory table keys

-- Far-future sentinel for "no expiry" in the SQL backend, so expires_at stays
-- NOT NULL and never appears mid-array as a nil (which would truncate a Lua
-- params table). Fits a 64-bit BIGINT; any real ms-epoch is far below it.
M.NO_EXPIRY = math.maxinteger

-- A key is arbitrary NON-EMPTY bytes (a Lua string). Values are arbitrary
-- bytes including empty. We do not assume UTF-8.
function M.check_key(k)
    if type(k) ~= "string" then
        M.error("invalid_argument", "kv: key must be a string (bytes), got " .. type(k))
    end
    if #k == 0 then
        M.error("invalid_argument", "kv: key must be non-empty")
    end
    if #k > M.MAX_KEY then
        M.error("invalid_argument", "kv: key exceeds " .. M.MAX_KEY .. " bytes")
    end
    return k
end

function M.check_value(v)
    if type(v) ~= "string" then
        M.error("invalid_argument", "kv: value must be a string (bytes), got " .. type(v))
    end
    return v
end

-- ttl (seconds) -> absolute expiry in epoch ms, or nil for no expiry.
--   nil      -> use the store default (may itself be nil = no expiry)
--   number>0 -> that many seconds from now
--   0        -> expire immediately (an explicit "already stale" write)
--   false    -> no expiry, overriding any default
function M.expiry_ms(ttl, default_ttl)
    if ttl == nil then ttl = default_ttl end
    if ttl == nil or ttl == false then return nil end
    if type(ttl) ~= "number" then
        M.error("invalid_argument", "kv: ttl must be a number of seconds")
    end
    return time.now_ms() + math.floor(ttl * 1000)
end

function M.now_ms() return time.now_ms() end

-- Validate an optional non-negative integer (a count/limit); nil passes
-- through. string.format("%d", ...) already rejects non-integers loudly, but
-- validating up front gives a stable coded error instead of a format raise.
function M.check_count(v, what)
    if v == nil then return nil end
    if type(v) ~= "number" or v ~= math.floor(v) or v < 0 then
        M.error("invalid_argument", "kv: " .. what .. " must be a non-negative integer")
    end
    return v
end

-- Parse a stored value as a base-10 integer for incr(); a fresh key is 0.
function M.to_int(bytes)
    local n = tonumber(bytes)
    if not n or n ~= math.floor(n) then
        M.error("invalid_argument", "kv: value is not an integer for incr()")
    end
    return math.floor(n)
end

-- ---- binary-safe base64 (standard alphabet, padded) ----
local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local DEC = {}
for i = 1, #B64 do DEC[B64:sub(i, i)] = i - 1 end

function M.b64encode(bytes)
    local out, n = {}, #bytes
    local i = 1
    while i <= n do
        local b1 = bytes:byte(i)
        local b2 = i + 1 <= n and bytes:byte(i + 1) or nil
        local b3 = i + 2 <= n and bytes:byte(i + 2) or nil
        local n1 = b1 >> 2
        local n2 = ((b1 & 0x3) << 4) | ((b2 or 0) >> 4)
        local n3 = b2 and (((b2 & 0xf) << 2) | ((b3 or 0) >> 6)) or nil
        local n4 = b3 and (b3 & 0x3f) or nil
        out[#out + 1] = B64:sub(n1 + 1, n1 + 1)
        out[#out + 1] = B64:sub(n2 + 1, n2 + 1)
        out[#out + 1] = n3 and B64:sub(n3 + 1, n3 + 1) or "="
        out[#out + 1] = n4 and B64:sub(n4 + 1, n4 + 1) or "="
        i = i + 3
    end
    return table.concat(out)
end

function M.b64decode(str)
    local out = {}
    local buf, bits = 0, 0
    for i = 1, #str do
        local c = str:sub(i, i)
        if c ~= "=" then
            local d = DEC[c]
            if not d then M.error("invalid_argument", "kv: corrupt base64 in store") end
            buf = (buf << 6) | d
            bits = bits + 6
            if bits >= 8 then
                bits = bits - 8
                out[#out + 1] = string.char((buf >> bits) & 0xff)
            end
        end
    end
    return table.concat(out)
end

-- ---- hex (prefix-preserving: hex(prefix) is a prefix of hex(key), so the SQL
-- backend can range-scan keys with a plain LIKE) ----
function M.hexencode(bytes)
    return (bytes:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

function M.hexdecode(hex)
    if #hex % 2 ~= 0 then M.error("invalid_argument", "kv: corrupt hex in store") end
    return (hex:gsub("..", function(cc)
        local n = tonumber(cc, 16)
        if not n then M.error("invalid_argument", "kv: corrupt hex in store") end
        return string.char(n)
    end))
end

return M
