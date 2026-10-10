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

-- Validate a ttl / default_ttl: nil and false pass through; anything else
-- must be a finite number of seconds >= 0 (audit 12). NaN made a value that
-- never expired on the memory backend (every comparison with it is false),
-- math.huge one that overflowed the SQL expiry, and a negative one a write
-- that was already stale (and was refused by the Valkey backend only).
function M.check_ttl(ttl, what)
    if ttl == nil or ttl == false then return ttl end
    if type(ttl) ~= "number" or ttl ~= ttl or ttl < 0 or ttl == math.huge then
        M.error("invalid_argument",
            "kv: " .. (what or "ttl") .. " must be a finite number of seconds >= 0")
    end
    return ttl
end

-- ttl (seconds) -> absolute expiry in epoch ms, or nil for no expiry.
--   nil      -> use the store default (may itself be nil = no expiry)
--   number>0 -> that many seconds from now
--   0        -> expire immediately (an explicit "already stale" write)
--   false    -> no expiry, overriding any default
function M.expiry_ms(ttl, default_ttl)
    if ttl == nil then ttl = default_ttl end
    if ttl == nil or ttl == false then return nil end
    M.check_ttl(ttl)
    return time.now_ms() + math.floor(ttl * 1000)
end

function M.now_ms() return time.now_ms() end

-- A cas() ttl meaning "keep the replaced value's expiry". Internal: rekey uses
-- it to re-seal values in place without changing when they expire.
M.KEEP_TTL = setmetatable({}, { __tostring = function() return "kv.KEEP_TTL" end })

-- Validate an optional non-negative integer (a count/limit); nil passes
-- through. string.format("%d", ...) already rejects non-integers loudly, but
-- validating up front gives a stable coded error instead of a format raise.
function M.check_count(v, what)
    if v == nil then return nil end
    -- math.huge passed the floor test (floor(inf) == inf): reject it too.
    if type(v) ~= "number" or v ~= math.floor(v) or v < 0 or v == math.huge then
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

-- ---- the store's encodings: standard padded base64 for values, lowercase
-- hex for keys (prefix-preserving: hex(prefix) is a prefix of hex(key), so the
-- SQL backend can range-scan keys with a plain LIKE). A value that does not
-- decode is corruption in the store, reported with kv's own code. ----
local encoding = require("hull.encoding")

function M.b64encode(bytes) return encoding.base64.encode(bytes) end
function M.hexencode(bytes) return encoding.hex.encode(bytes) end

function M.b64decode(str)
    return encoding.base64.decode(str)
        or M.error("corrupt", "kv: corrupt base64 in store")
end

function M.hexdecode(hex)
    return encoding.hex.decode(hex)
        or M.error("corrupt", "kv: corrupt hex in store")
end

return M
