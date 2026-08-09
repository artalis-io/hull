--- In-memory key/value cache with TTL and get-or-compute memoization.
--
-- A bounded, lazily-expiring cache for the common "compute once, reuse for a
-- while" pattern - derived values, parsed config, a hot DB row, an API response.
-- In-memory and process-local (no persistence, no cross-node sharing); values
-- are stored as-is (any Lua value, no serialization). One small dep (time).
--
-- The default instance backs the top-level `cache.*` calls; `cache.new(opts)`
-- gives an isolated instance (own keyspace + cap) for a library or subsystem.
--
-- @module hull.cache
-- @license AGPL-3.0-or-later
-- @usage
--   local cache = require("hull.cache")
--   -- memoize: hit returns cached; miss runs fn(), stores it, returns it
--   local u = cache.fetch("user:" .. id, 300, function() return load_user(id) end)
--   cache.set("k", value, 60)   -- ttl seconds; nil = no expiry; 0 = expire now
--   local v = cache.get("k")    -- value or nil
--
--   -- async producers (http.fetch / compute.async): compose get + set so the
--   -- app controls the await:
--   local v = cache.get(k)
--   if v == nil then v = fetch_remote(); cache.set(k, v, 60) end

local time = require("hull.time")

local cache = {}

local DEFAULT_MAX = 1000

--- Create an isolated cache instance.
-- @tparam[opt] table opts  `max_entries` (default 1000), `default_ttl`
--   (seconds; used by set/fetch when no ttl is passed; default no expiry).
-- @treturn table  Instance with get/set/fetch/has/delete/clear/size.
function cache.new(opts)
    opts = opts or {}
    local store = {}          -- key -> { value, expires (ms|nil), seq }
    local count = 0
    local seq = 0
    local max = opts.max_entries or DEFAULT_MAX
    local default_ttl = opts.default_ttl

    local function next_seq() seq = seq + 1; return seq end

    -- Return the entry if live; drop + return nil if it has expired.
    local function live(key)
        local e = store[key]
        if not e then return nil end
        if e.expires ~= nil and time.now_ms() >= e.expires then
            store[key] = nil
            count = count - 1
            return nil
        end
        return e
    end

    -- Make room: drop an expired entry if any, else the least-recently-used.
    local function evict_one()
        local now = time.now_ms()
        local lru_key, lru_seq
        for k, e in pairs(store) do
            if e.expires ~= nil and now >= e.expires then
                store[k] = nil
                count = count - 1
                return
            end
            if lru_seq == nil or e.seq < lru_seq then
                lru_key, lru_seq = k, e.seq
            end
        end
        if lru_key ~= nil then
            store[lru_key] = nil
            count = count - 1
        end
    end

    local self = {}

    --- Return the value for `key`, or nil on miss / expiry.
    function self.get(key)
        local e = live(key)
        if not e then return nil end
        e.seq = next_seq()
        return e.value
    end

    --- Whether `key` is present and unexpired.
    function self.has(key)
        return live(key) ~= nil
    end

    --- Store `value` under `key`. `ttl` is seconds (fractional ok); nil uses
    -- the instance `default_ttl` (no expiry if unset); 0 expires immediately.
    -- @return value
    function self.set(key, value, ttl)
        if ttl == nil then ttl = default_ttl end
        local expires = nil
        if ttl ~= nil then expires = time.now_ms() + ttl * 1000 end
        local e = store[key]
        if e == nil then
            if count >= max then evict_one() end
            store[key] = { value = value, expires = expires, seq = next_seq() }
            count = count + 1
        else
            e.value = value
            e.expires = expires
            e.seq = next_seq()
        end
        return value
    end

    --- Get `key`, or compute it: on miss run `fn()`, store the result under
    -- `ttl`, and return it. Call as `fetch(key, ttl, fn)` or `fetch(key, fn)`.
    -- `fn` is synchronous; for async producers compose get + set.
    function self.fetch(key, ttl, fn)
        if fn == nil and type(ttl) == "function" then
            fn = ttl
            ttl = nil
        end
        local e = live(key)
        if e then
            e.seq = next_seq()
            return e.value
        end
        local v = fn()
        self.set(key, v, ttl)
        return v
    end

    --- Remove `key`. Returns true if it was present.
    function self.delete(key)
        if store[key] ~= nil then
            store[key] = nil
            count = count - 1
            return true
        end
        return false
    end

    --- Remove every entry.
    function self.clear()
        store = {}
        count = 0
    end

    --- Current entry count (may include not-yet-swept expired entries).
    function self.size()
        return count
    end

    return self
end

-- ---------------------------------------------------------------------------
-- cache.open{} - the byte-oriented, backend-selectable CACHE handle.
--
-- Distinct from cache.new() above: cache.new is a lightweight in-process
-- memoizer for arbitrary Lua VALUES (item-count bound). cache.open deals in
-- BYTES (keys/values are strings) and adds byte accounting, pluggable backends
-- (memory / sqlite), and namespaces - sharing the same store cores as
-- hull.kv but with CACHE policy (eviction ON). Use cache.open when you need
-- max_bytes, a durable/SQL cache, or explicit namespaces; cache.new for a
-- quick local value memoizer.
--
--   local c = require("hull.cache").open{ backend = "memory",
--       namespace = "query-ir", max_bytes = 512*1024*1024, default_ttl = 600 }
--   c:set(k, bytes); local v = c:get(k)   -- bytes | nil
--   c:fetch(k, 60, function() return render() end)   -- get-or-compute (bytes)
--   c.stats()   -- { hits, misses, evictions, items, bytes }
-- ---------------------------------------------------------------------------
function cache.open(opts)
    local u      = require("hull.kv._util")
    local handle = require("hull.kv._handle")
    if type(opts) ~= "table" then
        u.error("invalid_argument", "cache.open: options table required")
    end
    local backend = opts.backend or "memory"
    local ns = opts.namespace or "default"
    local store_ns = "cache:" .. ns   -- isolated from hull.kv's "kv:" namespaces

    local store, bname
    if backend == "memory" then
        store = require("hull.kv._memstore").get(store_ns, {
            evict       = true,                 -- CACHE: LRU eviction ON
            default_ttl = opts.default_ttl,
            max_bytes   = opts.max_bytes,
            max_items   = opts.max_items,
        })
        bname = "memory"
    elseif backend == "sqlite" then
        local conn = opts.database
        if type(conn) ~= "table" or type(conn.exec) ~= "function" then
            u.error("invalid_argument",
                "cache.open: sqlite backend needs database = <db connection>")
        end
        store = require("hull.kv._sql").new(conn, store_ns, {
            evict = true, default_ttl = opts.default_ttl, max_items = opts.max_items,
        })
        bname = conn.backend_name or "sqlite"
    elseif backend == "valkey" or backend == "redis" then
        -- Networked cache over the composed Valkey/Redis backend. Server-side
        -- TTL + maxmemory eviction; caller-owned connection (h:close() / GC).
        store, bname = require("hull.kv._valkey").new(opts, store_ns)
    else
        u.error("invalid_argument", "cache.open: unknown backend '" .. tostring(backend) .. "'")
    end

    local h = handle.build(store, { namespace = ns, backend = bname })

    --- Get `key`, or compute + cache it. `fetch(key, ttl, fn)` or
    -- `fetch(key, fn)`. `fn` must return bytes (a string); it is synchronous.
    function h:fetch(key, ttl, fn)
        if fn == nil and type(ttl) == "function" then fn = ttl; ttl = nil end
        u.check_key(key)
        local v = self._s:get(key)
        if v ~= nil then return v end
        v = fn()
        u.check_value(v)
        self._s:put(key, v, ttl)
        return v
    end

    return h
end

-- Default instance backs the top-level cache.* convenience API. A cache is
-- intentionally cross-request shared state (like ratelimit's buckets), so a
-- module-level default is correct here, not the request-scoped-global footgun.
local _default = cache.new()
cache.get = _default.get
cache.set = _default.set
cache.fetch = _default.fetch
cache.has = _default.has
cache.delete = _default.delete
cache.clear = _default.clear
cache.size = _default.size

return cache
