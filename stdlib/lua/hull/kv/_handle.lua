--[[
  hull.kv._handle - the semantic surface over a backend store.

  Wraps a memory/sql store with key/value validation and capability
  enforcement (an optional op on a backend that lacks the cap throws
  `unsupported`, never silently no-ops). Shared by hull.kv and hull.cache so
  the two present the same core methods with the same guarantees; hull.cache
  layers fetch() on top. Handles are used method-style (`h:get(k)`).

  Internal module. SPDX-License-Identifier: AGPL-3.0-or-later
]]

local u = require("hull.kv._util")

local M = {}
local H = {}
H.__index = H

-- `meta.seal` (a hull.kv._seal) makes the handle encrypted: values are sealed
-- on the way in and opened on the way out. Only hull.kv passes one.
function M.build(store, meta)
    return setmetatable({
        _s = store, caps = store.caps, _seal = meta.seal,
        namespace = meta.namespace, backend = meta.backend,
    }, H)
end

local function need(self, cap)
    if not self.caps[cap] then
        u.error("unsupported",
            "kv: backend '" .. self.backend .. "' does not support " .. cap)
    end
end

function H:get(k)
    u.check_key(k)
    local stored = self._s:get(k)
    if stored == nil or not self._seal then return stored end
    return (self._seal:open(k, stored))
end

function H:set(k, v, opts)
    u.check_key(k); u.check_value(v)
    if self._seal then v = self._seal:seal(k, v) end
    self._s:put(k, v, opts and opts.ttl)
    return true
end

function H:delete(k) u.check_key(k); return self._s:del(k) end

function H:exists(k) u.check_key(k); return self._s:has(k) end

function H:incr(k, by, opts)
    need(self, "atomic_increment"); u.check_key(k)
    if self._seal then
        -- The backend adds to the stored bytes, which it cannot do to a
        -- sealed value. A secret counter is a value updated with cas.
        u.error("unsupported", "kv: incr is not available on an encrypted handle; "
                .. "store the counter as a value and update it with cas")
    end
    by = by or 1
    if type(by) ~= "number" or by ~= math.floor(by) then
        u.error("invalid_argument", "kv: incr amount must be an integer")
    end
    return self._s:incr(k, by, opts and opts.ttl)
end

function H:cas(k, expected, new, opts)
    need(self, "compare_exchange"); u.check_key(k); u.check_value(new)
    if expected ~= nil then u.check_value(expected) end
    local ttl = opts and opts.ttl
    if not self._seal then return self._s:cas(k, expected, new, ttl) end

    -- Encrypted: a sealed value never equals `expected` as bytes (each seal
    -- has a fresh nonce), so compare the OPENED value, then swap on the exact
    -- stored bytes. Still atomic - the backend compares what it holds - and a
    -- concurrent writer in between makes the swap fail, as it should.
    local sealed_new = self._seal:seal(k, new)
    if expected == nil then return self._s:cas(k, nil, sealed_new, ttl) end
    local stored = self._s:get(k)
    if stored == nil then return false end
    if self._seal:open(k, stored) ~= expected then return false end
    return self._s:cas(k, stored, sealed_new, ttl)
end

--- Re-seal every value under `prefix` (all, by default) that is not sealed
--- with the current key, so an old key can then be dropped. Returns how many
--- were re-sealed. Safe to interrupt and run again; a value changed by
--- someone else meanwhile is left alone (they wrote it under the current key).
--- Needs the backend's scan and compare-and-swap.
function H:rekey(prefix, opts)
    if not self._seal then
        u.error("invalid_argument", "kv: rekey needs an encrypted handle")
    end
    need(self, "scan"); need(self, "compare_exchange")
    local n = 0
    for _, k in ipairs(self:scan(prefix or "")) do
        local stored = self._s:get(k)
        if stored ~= nil then
            local value, version = self._seal:open(k, stored)
            if not self._seal:is_current(version) then
                if self._s:cas(k, stored, self._seal:seal(k, value),
                               opts and opts.ttl) then
                    n = n + 1
                end
            end
        end
    end
    return n
end

function H:clear() self._s:clear(); return true end

function H:scan(prefix, opts)
    need(self, "scan")
    if prefix ~= nil and type(prefix) ~= "string" then
        u.error("invalid_argument", "kv: scan prefix must be a string (bytes)")
    end
    local limit = u.check_count(opts and opts.limit, "scan limit")
    return self._s:scan(prefix, limit)
end

function H:cleanup() return self._s:cleanup() end

function H:stats() return self._s:stats() end

-- Release an owned backend connection (the networked valkey/redis store). A
-- no-op for the memory/sql stores, whose connections are borrowed/shared. Safe
-- to call more than once; GC finalizes an unclosed network connection anyway.
function H:close() if self._s.close then self._s:close() end end

return M
