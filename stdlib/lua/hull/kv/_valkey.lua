--[[
  hull.kv._valkey - Valkey/Redis store backing hull.kv and hull.cache.

  Wraps the base-resident KV connection cap (hull.kv._native, over cap/kv.c ->
  the composed HlKvBackend) in the store interface _handle expects
  (get/put/del/has/incr/cas/clear/scan/cleanup/stats + .caps). The connection is
  reached only when the app composes the feature (hull build --with=valkey); on a
  plain base, hull.kv._native.open() fails closed with an install hint.

  Physical-key namespacing (collision- and glob-safe): every app key is stored
  under `<type>:<hex(namespace)>:<key>` where <type> is "kv" or "cache" (from the
  store_ns the caller passes). Hex-encoding the namespace bytes means a namespace
  can never contain a ':' that aliases another, nor a SCAN glob metacharacter -
  so a kv and a cache namespace, or two namespaces one a prefix of the other,
  never collide on the shared server.

  Values and keys are arbitrary bytes (Lua strings). TTLs are SECONDS at this
  layer (the handle's contract) and converted to milliseconds for the backend.

  Internal module. SPDX-License-Identifier: AGPL-3.0-or-later
]]

local u      = require("hull.kv._util")
local native = require("hull.kv._native")

local M = {}

-- HL_KV_CAP_* bit values (must match include/hull/cap/kv_backend.h).
local CAP_TTL          = 1
local CAP_INCR         = 2
local CAP_CAS          = 4
local CAP_SCAN         = 8
local CAP_PERSISTENT   = 16
local CAP_SHARED       = 32
local CAP_EVICTION     = 64
local CAP_TRANSACTIONS = 128

local HEX = "0123456789abcdef"
local function hex(s)
    local out = {}
    for i = 1, #s do
        local b = string.byte(s, i)
        out[i] = HEX:sub((b >> 4) + 1, (b >> 4) + 1) .. HEX:sub((b & 15) + 1, (b & 15) + 1)
    end
    return table.concat(out)
end

-- Split "kv:agent-state" / "cache:x" into the readable type + the namespace we
-- hex-encode. A store_ns without a ':' is treated as the whole namespace under
-- an empty type (defensive; the callers always pass "kv:"/"cache:").
local function phys_prefix(store_ns)
    local ty, rest = string.match(store_ns, "^([^:]*):(.*)$")
    if not ty then ty, rest = "", store_ns end
    return ty .. ":" .. hex(rest) .. ":"
end

local Store = {}
Store.__index = Store

local function ttl_ms(self, ttl)
    ttl = ttl or self.default_ttl
    if ttl == nil then return nil end
    if type(ttl) ~= "number" or ttl < 0 then
        u.error("invalid_argument", "kv: ttl must be a non-negative number of seconds")
    end
    return math.floor(ttl * 1000)
end

function Store:get(k) return self.c:get(self.pfx .. k) end

function Store:put(k, v, ttl) self.c:set(self.pfx .. k, v, ttl_ms(self, ttl)) end

function Store:del(k) return self.c:del(self.pfx .. k) end

function Store:has(k) return self.c:has(self.pfx .. k) end

function Store:incr(k, by, ttl) return self.c:incr(self.pfx .. k, by, ttl_ms(self, ttl)) end

function Store:cas(k, expected, new, ttl)
    -- native cas -> 0 ok / 1 mismatch / 2 conflict; the boolean store contract
    -- is "did the swap happen": a retry-exhausted conflict is a non-swap (false),
    -- and a transport ERROR raises inside the native call.
    return self.c:cas(self.pfx .. k, expected, new, ttl_ms(self, ttl)) == 0
end

function Store:clear() self.c:clear(self.pfx) end

function Store:scan(prefix, limit)
    prefix = prefix or ""
    -- Match on <phys_prefix><user_prefix>*; the backend strips that whole match
    -- prefix, so it hands back the SUFFIX after the user prefix. Re-prepend the
    -- user prefix to reconstruct the full app key (matching memstore/sql, which
    -- return full keys).
    local suffixes = self.c:scan(self.pfx .. prefix, limit)
    local out = {}
    for i = 1, #suffixes do out[i] = prefix .. suffixes[i] end
    return out
end

-- Redis/Valkey expire keys server-side; there is nothing for the client to
-- sweep. cleanup is a no-op returning 0 removed (matches the handle contract).
function Store:cleanup() return 0 end

function Store:stats()
    return { backend = "valkey", namespace = self.namespace, shared = true }
end

function Store:close() self.c:close() end

--- Open a Valkey/Redis store.
-- @param opts { dsn = "valkey://..."|"redis://..." (or url), namespace,
--              default_ttl = <seconds> }
-- @param store_ns "kv:<namespace>" / "cache:<namespace>" (the collision prefix)
function M.new(opts, store_ns)
    local dsn = opts.dsn or opts.url
    if type(dsn) ~= "string" or dsn == "" then
        u.error("invalid_argument",
            "kv.open: valkey backend needs dsn = 'valkey://...' (or 'redis://...')")
    end
    local conn = native.open(dsn)   -- raises backend_error on connect failure

    local caps_bits = conn:caps()
    local self = setmetatable({
        c           = conn,
        pfx         = phys_prefix(store_ns),
        namespace   = opts.namespace,
        default_ttl = opts.default_ttl,
        caps = {
            ttl              = (caps_bits & CAP_TTL) ~= 0,
            atomic_increment = (caps_bits & CAP_INCR) ~= 0,
            compare_exchange = (caps_bits & CAP_CAS) ~= 0,
            scan             = (caps_bits & CAP_SCAN) ~= 0,
            persistent       = (caps_bits & CAP_PERSISTENT) ~= 0,
            shared           = (caps_bits & CAP_SHARED) ~= 0,
            eviction         = (caps_bits & CAP_EVICTION) ~= 0,
            transactions     = (caps_bits & CAP_TRANSACTIONS) ~= 0,
        },
    }, Store)
    return self, conn:backend_name()
end

return M
