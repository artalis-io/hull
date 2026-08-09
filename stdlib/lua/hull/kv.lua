--[[
  hull.kv - portable key/value STORE.

  KV STORE semantics (distinct from hull.cache): externally meaningful state,
  no eviction unless explicitly requested, persistence where the backend
  provides it, potentially shared across processes. Keys and values are
  arbitrary bytes (Lua strings); we do not assume UTF-8.

  Backends (milestone 1): "memory" (in-process, ephemeral, process-shared by
  namespace), "sqlite" and "postgres" (durable, over an existing Hull db
  connection). Each backend advertises a capability set (handle.caps); an
  optional op on a backend that lacks the cap throws `unsupported`.

    local kv = require("hull.kv").open{ backend = "sqlite", database = db,
                                        namespace = "agent-state" }
    kv:set("run:123", payload)          -- bytes -> bytes
    local v = kv:get("run:123")         -- bytes | nil (miss)
    kv:set(k, v, { ttl = 3600 })        -- optional TTL (seconds)
    kv:cas(k, expected, new)            -- atomic compare-and-swap
    kv:incr("hits", 1)                  -- atomic increment
    kv:scan("run:")                     -- prefix iteration -> { keys }
    kv:delete(k); kv:exists(k); kv:clear()

  Security: SQL backends run through the db capability, so a SQLite file
  honors the fs sandbox and a network DB honors the host allowlist +
  network_outbound grant - the KV layer opens no hidden fs/network access.

  SPDX-License-Identifier: AGPL-3.0-or-later
]]

local u      = require("hull.kv._util")
local handle = require("hull.kv._handle")

local M = { _VERSION = "1" }

local function memory_backend(opts, store_ns)
    -- KV memory: eviction OFF. A max_bytes/max_items cap (if given) makes a
    -- full store raise on write rather than silently drop externally-meaningful
    -- state. Unbounded by default.
    local store = require("hull.kv._memstore").get(store_ns, {
        evict       = false,
        default_ttl = opts.default_ttl,
        max_bytes   = opts.max_bytes,
        max_items   = opts.max_items,
    })
    return store, "memory"
end

local function sql_backend(opts, store_ns)
    -- Reuse the app's existing db connection - no second connection stack.
    local conn = opts.database
    if type(conn) ~= "table" or type(conn.exec) ~= "function" then
        u.error("invalid_argument",
            "kv.open: sql backend needs database = <db connection> "
            .. "(e.g. require('hull.db').default())")
    end
    local store = require("hull.kv._sql").new(conn, store_ns, {
        evict = false, default_ttl = opts.default_ttl,
    })
    return store, conn.backend_name or "sql"
end

--- Open a KV handle.
-- @param opts { backend = "memory"|"sqlite"|"postgres", namespace = "default",
--              database = <db conn> (sql), dsn|path = <dsn> (sql convenience),
--              default_ttl = <seconds>, max_bytes, max_items (memory) }
function M.open(opts)
    if type(opts) ~= "table" then
        u.error("invalid_argument", "kv.open: options table required")
    end
    local backend = opts.backend or "memory"
    local namespace = opts.namespace or "default"   -- do not mutate the caller's opts
    -- Prefix the physical store namespace so a kv and a cache that happen to
    -- share a namespace name never collide in the same memory table / _hull_kv.
    local store_ns = "kv:" .. namespace

    local store, bname
    if backend == "memory" then
        store, bname = memory_backend(opts, store_ns)
    elseif backend == "sqlite" or backend == "postgres" then
        store, bname = sql_backend(opts, store_ns)
    elseif backend == "valkey" or backend == "redis" then
        -- Networked KV over the composed Valkey/Redis backend. Caller-owned
        -- connection (close it via the handle's :close(), or let GC finalize).
        store, bname = require("hull.kv._valkey").new(opts, store_ns)
    else
        u.error("invalid_argument", "kv.open: unknown backend '" .. tostring(backend) .. "'")
    end

    return handle.build(store, { namespace = namespace, backend = bname })
end

return M
