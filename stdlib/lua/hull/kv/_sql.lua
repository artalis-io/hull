--[[
  hull.kv._sql - SQL-backed byte store over an existing Hull db connection.

  Reuses the backend-agnostic db capability (no second connection stack): all
  writes go through the same parameterized query/exec the rest of the stdlib
  uses, so SQLite and PostgreSQL are served by one implementation. Keys are
  hex-encoded (prefix-preserving, so scan() is a LIKE range) and values are
  base64-encoded, both into portable TEXT columns - Postgres TEXT rejects
  embedded NULs, so raw binary can't be stored directly.

  One physical table `_hull_kv` (namespace + key primary key) serves both
  hull.kv (durable, no eviction) and hull.cache (SQL-expressed max_items
  eviction) via policy. Protected from app code by the `_hull_` prefix
  (stdlib callers bypass the namespace guard).

  Internal module. SPDX-License-Identifier: AGPL-3.0-or-later
]]

local u = require("hull.kv._util")

local M = {}
local Store = {}
Store.__index = Store

-- ON CONFLICT is SQLite + PostgreSQL (+ DuckDB) syntax; MySQL differs and is a
-- later milestone. Gate explicitly so the failure is a clear message, not a
-- syntax error at first write.
local SUPPORTED = { sqlite = true, postgres = true }

local DDL = [[
CREATE TABLE IF NOT EXISTS _hull_kv (
    ns TEXT NOT NULL,
    k TEXT NOT NULL,
    v TEXT NOT NULL,
    expires_at BIGINT NOT NULL,
    version BIGINT NOT NULL DEFAULT 1,
    created_at BIGINT NOT NULL,
    updated_at BIGINT NOT NULL,
    PRIMARY KEY (ns, k)
)]]

-- Create the table once per connection (idempotent, but skip the round-trip).
local created = setmetatable({}, { __mode = "k" })
local function ensure_table(conn)
    if created[conn] then return end
    conn.exec(DDL)
    created[conn] = true
end

-- policy: { default_ttl, evict(bool), max_items }
function M.new(conn, namespace, policy)
    if type(conn) ~= "table" or type(conn.exec) ~= "function" then
        u.error("invalid_argument", "kv: sql backend needs a db connection (database = ...)")
    end
    local backend = conn.backend_name or "?"
    if not SUPPORTED[backend] then
        u.error("unsupported",
            "kv: sql backend does not support '" .. backend .. "' yet (sqlite/postgres)")
    end
    ensure_table(conn)
    policy = policy or {}
    return setmetatable({
        conn = conn, ns = namespace,
        default_ttl = policy.default_ttl,
        evict = policy.evict and true or false,
        max_items = u.check_count(policy.max_items, "max_items") or 0,
        caps = {
            ttl = true, atomic_increment = true, compare_exchange = true,
            scan = true, persistent = true, shared = (backend == "postgres"),
            eviction = policy.evict and true or false, transactions = true,
        },
    }, Store)
end

local function kenc(k) return u.hexencode(k) end
local function venc(v) return u.b64encode(v) end
local function vdec(s) return u.b64decode(s) end

function Store:get(k)
    local rows = self.conn.query(
        "SELECT v FROM _hull_kv WHERE ns = ? AND k = ? AND expires_at > ?",
        { self.ns, kenc(k), u.now_ms() })
    if not rows or #rows == 0 then return nil end
    return vdec(rows[1].v)
end

function Store:has(k) return self:get(k) ~= nil end

-- Enforce max_items for a cache-over-SQL store by deleting the oldest rows.
local function evict_items(self)
    if not self.evict or self.max_items <= 0 then return end
    local rows = self.conn.query(
        "SELECT COUNT(*) AS n FROM _hull_kv WHERE ns = ?", { self.ns })
    local n = rows and rows[1] and tonumber(rows[1].n) or 0
    if n <= self.max_items then return end
    local over = n - self.max_items
    -- Delete the `over` least-recently-updated keys in this namespace.
    self.conn.exec(
        "DELETE FROM _hull_kv WHERE ns = ? AND k IN ("
        .. "SELECT k FROM _hull_kv WHERE ns = ? ORDER BY updated_at ASC LIMIT "
        .. string.format("%d", over) .. ")",
        { self.ns, self.ns })
end

function Store:put(k, v, ttl)
    local now = u.now_ms()
    local exp = u.expiry_ms(ttl, self.default_ttl) or u.NO_EXPIRY
    self.conn.exec(
        "INSERT INTO _hull_kv (ns, k, v, expires_at, version, created_at, updated_at) "
        .. "VALUES (?, ?, ?, ?, 1, ?, ?) "
        .. "ON CONFLICT (ns, k) DO UPDATE SET "
        .. "v = excluded.v, expires_at = excluded.expires_at, "
        .. "version = _hull_kv.version + 1, updated_at = excluded.updated_at",
        { self.ns, kenc(k), venc(v), exp, now, now })
    evict_items(self)
    return v
end

function Store:del(k)
    local n = self.conn.exec(
        "DELETE FROM _hull_kv WHERE ns = ? AND k = ?", { self.ns, kenc(k) })
    return (n or 0) > 0
end

-- Atomic compare-and-swap. expected == nil means "set only if absent".
-- CAS/incr correctness relies on `conn.exec` returning rows CHANGED (not rows
-- matched): `ON CONFLICT ... DO NOTHING` on an existing row must report 0, and a
-- real insert/update 1. Both shipped backends satisfy this (SQLite
-- sqlite3_changes(); Postgres command tag). A backend reporting rows-matched
-- would break set-if-absent (a DO-NOTHING on an existing key would falsely
-- report success) -- keep that invariant in mind when adding a backend.
function Store:cas(k, expected, new, ttl)
    local now = u.now_ms()
    local exp = u.expiry_ms(ttl, self.default_ttl) or u.NO_EXPIRY
    if expected == nil then
        local n = self.conn.exec(
            "INSERT INTO _hull_kv (ns, k, v, expires_at, version, created_at, updated_at) "
            .. "VALUES (?, ?, ?, ?, 1, ?, ?) ON CONFLICT (ns, k) DO NOTHING",
            { self.ns, kenc(k), venc(new), exp, now, now })
        return (n or 0) == 1
    end
    local n = self.conn.exec(
        "UPDATE _hull_kv SET v = ?, version = version + 1, updated_at = ?, expires_at = ? "
        .. "WHERE ns = ? AND k = ? AND v = ? AND expires_at > ?",
        { venc(new), now, exp, self.ns, kenc(k), venc(expected), now })
    return (n or 0) == 1
end

-- Atomic increment via an optimistic version-guarded retry loop (portable;
-- needs no row locks). A fresh key starts at 0.
function Store:incr(k, by, ttl)
    local khex = kenc(k)
    for _ = 1, 16 do
        local now = u.now_ms()
        local rows = self.conn.query(
            "SELECT v, version FROM _hull_kv WHERE ns = ? AND k = ? AND expires_at > ?",
            { self.ns, khex, now })
        if not rows or #rows == 0 then
            local exp = u.expiry_ms(ttl, self.default_ttl) or u.NO_EXPIRY
            local n = self.conn.exec(
                "INSERT INTO _hull_kv (ns, k, v, expires_at, version, created_at, updated_at) "
                .. "VALUES (?, ?, ?, ?, 1, ?, ?) ON CONFLICT (ns, k) DO NOTHING",
                { self.ns, khex, venc(tostring(by)), exp, now, now })
            if (n or 0) == 1 then return by end
        else
            local cur = u.to_int(vdec(rows[1].v))
            local ver = tonumber(rows[1].version)
            local nv = cur + by
            local n = self.conn.exec(
                "UPDATE _hull_kv SET v = ?, version = version + 1, updated_at = ? "
                .. "WHERE ns = ? AND k = ? AND version = ?",
                { venc(tostring(nv)), now, self.ns, khex, ver })
            if (n or 0) == 1 then return nv end
        end
        -- lost the race; re-read and retry
    end
    u.error("conflict", "kv: incr contended past retry budget")
end

function Store:clear()
    self.conn.exec("DELETE FROM _hull_kv WHERE ns = ?", { self.ns })
end

function Store:scan(prefix, limit)
    prefix = prefix or ""
    local sql = "SELECT k FROM _hull_kv WHERE ns = ? AND k LIKE ? AND expires_at > ?"
    if limit and limit > 0 then sql = sql .. " LIMIT " .. string.format("%d", limit) end
    local rows = self.conn.query(sql, { self.ns, u.hexencode(prefix) .. "%", u.now_ms() })
    local out = {}
    for _, r in ipairs(rows or {}) do out[#out + 1] = u.hexdecode(r.k) end
    return out
end

-- Delete expired rows in this namespace; returns the count removed.
function Store:cleanup()
    return self.conn.exec(
        "DELETE FROM _hull_kv WHERE ns = ? AND expires_at <= ?",
        { self.ns, u.now_ms() }) or 0
end

function Store:stats()
    local rows = self.conn.query(
        "SELECT COUNT(*) AS n FROM _hull_kv WHERE ns = ? AND expires_at > ?",
        { self.ns, u.now_ms() })
    return { items = rows and rows[1] and tonumber(rows[1].n) or 0 }
end

return M
