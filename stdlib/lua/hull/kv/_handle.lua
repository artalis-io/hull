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

function M.build(store, meta)
    return setmetatable({
        _s = store, caps = store.caps,
        namespace = meta.namespace, backend = meta.backend,
    }, H)
end

local function need(self, cap)
    if not self.caps[cap] then
        u.error("unsupported",
            "kv: backend '" .. self.backend .. "' does not support " .. cap)
    end
end

function H:get(k) u.check_key(k); return self._s:get(k) end

function H:set(k, v, opts)
    u.check_key(k); u.check_value(v)
    self._s:put(k, v, opts and opts.ttl)
    return true
end

function H:delete(k) u.check_key(k); return self._s:del(k) end

function H:exists(k) u.check_key(k); return self._s:has(k) end

function H:incr(k, by, opts)
    need(self, "atomic_increment"); u.check_key(k)
    by = by or 1
    if type(by) ~= "number" or by ~= math.floor(by) then
        u.error("invalid_argument", "kv: incr amount must be an integer")
    end
    return self._s:incr(k, by, opts and opts.ttl)
end

function H:cas(k, expected, new, opts)
    need(self, "compare_exchange"); u.check_key(k); u.check_value(new)
    if expected ~= nil then u.check_value(expected) end
    return self._s:cas(k, expected, new, opts and opts.ttl)
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

return M
