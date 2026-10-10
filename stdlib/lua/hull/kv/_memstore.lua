--[[
  hull.kv._memstore - native in-process byte store.

  One physical store backs two policies: hull.kv memory (eviction OFF, KV
  semantics) and hull.cache memory (LRU eviction ON, cache semantics). Keys
  and values are arbitrary bytes (Lua strings, binary-safe as table keys).
  Lazy TTL expiry on access, byte + item accounting, LRU through a doubly-
  linked recency list (touch and eviction are O(1)).
  Single-threaded (event-loop affinity); no locking.

  Stores are keyed by namespace at MODULE level, so two opens of the same
  namespace in one process share state (in-process "shared"); policy is fixed
  by the first open of a namespace.

  Internal module. SPDX-License-Identifier: AGPL-3.0-or-later
]]

local u = require("hull.kv._util")

local M = {}
local Store = {}
Store.__index = Store

-- namespace -> Store (process-wide, so same-namespace opens share)
local REGISTRY = {}

local ENTRY_OVERHEAD = 48 -- rough per-entry bookkeeping charged to byte accounting

-- policy: { max_bytes=0, max_items=0, default_ttl=nil, evict=false }
function M.get(namespace, policy)
    local s = REGISTRY[namespace]
    if s then return s end
    policy = policy or {}
    s = setmetatable({
        data        = {},   -- key -> { k, v, exp(ms|nil), bytes, prev, next }
        head        = nil,  -- most recently used entry
        tail        = nil,  -- least recently used entry
        items       = 0,
        bytes       = 0,
        max_bytes   = u.check_count(policy.max_bytes, "max_bytes") or 0,
        max_items   = u.check_count(policy.max_items, "max_items") or 0,
        default_ttl = u.check_ttl(policy.default_ttl, "default_ttl"),
        evict       = policy.evict and true or false,
        st          = { hits = 0, misses = 0, evictions = 0, expirations = 0 },
        caps        = {
            ttl = true, atomic_increment = true, compare_exchange = true,
            scan = true, persistent = false, shared = false,
            eviction = policy.evict and true or false, transactions = false,
        },
    }, Store)
    REGISTRY[namespace] = s
    return s
end

-- Recency list: head = most recently used, tail = least.
local function unlink(self, e)
    if e.prev then e.prev.next = e.next else self.head = e.next end
    if e.next then e.next.prev = e.prev else self.tail = e.prev end
    e.prev, e.next = nil, nil
end

local function push_front(self, e)
    e.prev, e.next = nil, self.head
    if self.head then self.head.prev = e else self.tail = e end
    self.head = e
end

local function touch(self, e)
    if self.head ~= e then unlink(self, e); push_front(self, e) end
end

local function drop(self, k, e)
    unlink(self, e)
    self.data[k] = nil
    self.items = self.items - 1
    self.bytes = self.bytes - e.bytes
end

-- Return the entry if live; lazily drop + return nil if expired.
local function live(self, k)
    local e = self.data[k]
    if not e then return nil end
    if e.exp and u.now_ms() >= e.exp then
        drop(self, k, e)
        self.st.expirations = self.st.expirations + 1
        return nil
    end
    return e
end

-- Evict least-recently-used entries until the incoming bytes fit, or
-- (eviction disabled) raise once a cap would be exceeded. `keep` is the key
-- being overwritten: it is never evicted - an overwrite larger than the
-- budget used to evict its own key (the only entry) and then account the new
-- value against a store that no longer held it, driving `bytes` negative.
local function make_room(self, add_bytes, adding_item, keep)
    local function over()
        return (self.max_items > 0 and self.items + (adding_item and 1 or 0) > self.max_items)
            or (self.max_bytes > 0 and self.bytes + add_bytes > self.max_bytes)
    end
    if not over() then return end
    if not self.evict then
        u.error("capacity_exceeded", "kv: in-memory store is full (eviction disabled)")
    end
    local e = self.tail
    while over() and e do
        local prev = e.prev
        if e.k ~= keep then
            drop(self, e.k, e)
            self.st.evictions = self.st.evictions + 1
        end
        e = prev
    end
    if over() then
        -- A single value larger than the whole budget.
        u.error("capacity_exceeded", "kv: value larger than the cache byte budget")
    end
end

-- An entry that alone exceeds the byte budget can never fit: refuse it before
-- anything is evicted for it.
local function check_fits(self, nb)
    if self.max_bytes > 0 and nb > self.max_bytes then
        u.error("capacity_exceeded", "kv: value larger than the cache byte budget")
    end
end

function Store:get(k)
    local e = live(self, k)
    if not e then self.st.misses = self.st.misses + 1; return nil end
    touch(self, e)
    self.st.hits = self.st.hits + 1
    return e.v
end

function Store:has(k) return live(self, k) ~= nil end

function Store:put(k, v, ttl)
    local exp = u.expiry_ms(ttl, self.default_ttl)
    local nb = #k + #v + ENTRY_OVERHEAD
    check_fits(self, nb)
    local old = self.data[k]
    if old then
        -- Overwrite: make room for the byte delta without evicting the key
        -- being updated (make_room's `keep`), then bump it to MRU.
        local delta = nb - old.bytes
        if delta > 0 then make_room(self, delta, false, k) end
        touch(self, old)
        self.bytes = self.bytes - old.bytes + nb
        old.v, old.exp, old.bytes = v, exp, nb
        return v
    end
    make_room(self, nb, true)
    local e = { k = k, v = v, exp = exp, bytes = nb }
    self.data[k] = e
    push_front(self, e)
    self.items = self.items + 1
    self.bytes = self.bytes + nb
    return v
end

function Store:del(k)
    local e = self.data[k]
    if not e then return false end
    drop(self, k, e)
    return true
end

function Store:incr(k, by, ttl)
    local e = live(self, k)
    if e then
        -- Preserve the existing expiry (matches the SQL backend and Redis
        -- INCR); only the value changes.
        local newv = tostring(u.to_int(e.v) + by)
        local nb = #k + #newv + ENTRY_OVERHEAD
        local delta = nb - e.bytes
        check_fits(self, nb)
        if delta > 0 then make_room(self, delta, false, k) end
        touch(self, e)
        self.bytes = self.bytes - e.bytes + nb
        e.v, e.bytes = newv, nb
        return u.to_int(newv)
    end
    self:put(k, tostring(by), ttl)   -- fresh key: apply ttl
    return by
end

-- compare-and-swap: expected may be nil (set only if absent). Returns bool.
function Store:cas(k, expected, new, ttl)
    local e = live(self, k)
    local cur = e and e.v or nil
    if cur ~= expected then return false end
    if ttl == u.KEEP_TTL then
        local exp = e and e.exp
        self:put(k, new, false)
        self.data[k].exp = exp
    else
        self:put(k, new, ttl)
    end
    return true
end

function Store:clear()
    self.data, self.items, self.bytes = {}, 0, 0
    self.head, self.tail = nil, nil
end

function Store:scan(prefix, limit)
    prefix = prefix or ""
    local out = {}
    for k, e in pairs(self.data) do
        if (not e.exp or u.now_ms() < e.exp) and k:sub(1, #prefix) == prefix then
            out[#out + 1] = k
            -- limit = 0 means no limit, as on the SQL backend and in JS
            -- (0 is truthy in Lua: it returned one key).
            if limit and limit > 0 and #out >= limit then break end
        end
    end
    return out
end

-- Sweep expired entries eagerly (optional hygiene; access already lazy-expires).
function Store:cleanup()
    local now, removed = u.now_ms(), 0
    for k, e in pairs(self.data) do
        if e.exp and now >= e.exp then drop(self, k, e); removed = removed + 1 end
    end
    self.st.expirations = self.st.expirations + removed
    return removed
end

function Store:stats()
    return {
        hits = self.st.hits, misses = self.st.misses,
        evictions = self.st.evictions, expirations = self.st.expirations,
        items = self.items, bytes = self.bytes,
    }
end

-- Test/lifecycle helper: forget a namespace's store entirely.
function M._forget(namespace) REGISTRY[namespace] = nil end

return M
