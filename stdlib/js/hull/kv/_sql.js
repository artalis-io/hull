/*
 * hull:kv:_sql - SQL-backed byte store over an existing Hull db connection (JS
 * mirror of hull.kv._sql). Reuses the backend-agnostic db capability (no second
 * connection stack); SQLite + PostgreSQL served by one implementation. Keys are
 * hex-encoded (prefix-preserving for scan), values base64-encoded, both into
 * portable TEXT columns (Postgres TEXT rejects embedded NULs). One `_hull_kv`
 * table serves hull.kv (durable) and hull.cache (SQL max_items eviction).
 *
 * Internal module. SPDX-License-Identifier: AGPL-3.0-or-later
 */

import util from "hull:kv:_util";

const SUPPORTED = { sqlite: true, postgres: true };

const DDL =
    "CREATE TABLE IF NOT EXISTS _hull_kv (" +
    "ns TEXT NOT NULL, k TEXT NOT NULL, v TEXT NOT NULL, " +
    "expires_at BIGINT NOT NULL, version BIGINT NOT NULL DEFAULT 1, " +
    "created_at BIGINT NOT NULL, updated_at BIGINT NOT NULL, " +
    "PRIMARY KEY (ns, k))";

const created = new WeakMap();
function ensureTable(conn) {
    if (created.get(conn)) return;
    conn.exec(DDL);
    created.set(conn, true);
}

class SqlStore {
    constructor(conn, namespace, policy) {
        if (!conn || typeof conn.exec !== "function")
            util.error("invalid_argument", "kv: sql backend needs a db connection (database: ...)");
        const backend = conn.backendName || "?";
        if (!SUPPORTED[backend])
            util.error("unsupported",
                "kv: sql backend does not support '" + backend + "' yet (sqlite/postgres)");
        ensureTable(conn);
        this.conn = conn;
        this.ns = namespace;
        this.defaultTtl = policy.defaultTtl;
        this.evict = !!policy.evict;
        this.maxItems = policy.maxItems || 0;
        this.caps = {
            ttl: true, atomic_increment: true, compare_exchange: true,
            scan: true, persistent: true, shared: (backend === "postgres"),
            eviction: !!policy.evict, transactions: true,
        };
    }

    get(k) {
        const rows = this.conn.query(
            "SELECT v FROM _hull_kv WHERE ns = ? AND k = ? AND expires_at > ?",
            [this.ns, util.hexencode(k), util.nowMs()]);
        if (!rows || rows.length === 0) return null;
        return util.b64decode(rows[0].v);
    }

    has(k) { return this.get(k) !== null; }

    _evictItems() {
        if (!this.evict || this.maxItems <= 0) return;
        const rows = this.conn.query(
            "SELECT COUNT(*) AS n FROM _hull_kv WHERE ns = ?", [this.ns]);
        const n = rows && rows[0] ? Number(rows[0].n) : 0;
        if (n <= this.maxItems) return;
        const over = n - this.maxItems;
        this.conn.exec(
            "DELETE FROM _hull_kv WHERE ns = ? AND k IN (" +
            "SELECT k FROM _hull_kv WHERE ns = ? ORDER BY updated_at ASC LIMIT " +
            String(over) + ")",
            [this.ns, this.ns]);
    }

    put(k, v, ttl) {
        const now = util.nowMs();
        const exp = util.expiryMs(ttl, this.defaultTtl);
        const expVal = exp === null ? util.NO_EXPIRY : exp;
        this.conn.exec(
            "INSERT INTO _hull_kv (ns, k, v, expires_at, version, created_at, updated_at) " +
            "VALUES (?, ?, ?, ?, 1, ?, ?) " +
            "ON CONFLICT (ns, k) DO UPDATE SET " +
            "v = excluded.v, expires_at = excluded.expires_at, " +
            "version = _hull_kv.version + 1, updated_at = excluded.updated_at",
            [this.ns, util.hexencode(k), util.b64encode(v), expVal, now, now]);
        this._evictItems();
        return v;
    }

    del(k) {
        const n = this.conn.exec(
            "DELETE FROM _hull_kv WHERE ns = ? AND k = ?", [this.ns, util.hexencode(k)]);
        return (n || 0) > 0;
    }

    cas(k, expected, newVal, ttl) {
        const now = util.nowMs();
        const exp = util.expiryMs(ttl, this.defaultTtl);
        const expVal = exp === null ? util.NO_EXPIRY : exp;
        if (expected === undefined || expected === null) {
            const n = this.conn.exec(
                "INSERT INTO _hull_kv (ns, k, v, expires_at, version, created_at, updated_at) " +
                "VALUES (?, ?, ?, ?, 1, ?, ?) ON CONFLICT (ns, k) DO NOTHING",
                [this.ns, util.hexencode(k), util.b64encode(newVal), expVal, now, now]);
            return (n || 0) === 1;
        }
        const n = this.conn.exec(
            "UPDATE _hull_kv SET v = ?, version = version + 1, updated_at = ?, expires_at = ? " +
            "WHERE ns = ? AND k = ? AND v = ? AND expires_at > ?",
            [util.b64encode(newVal), now, expVal, this.ns, util.hexencode(k),
             util.b64encode(expected), now]);
        return (n || 0) === 1;
    }

    incr(k, by, ttl) {
        const khex = util.hexencode(k);
        for (let attempt = 0; attempt < 16; attempt++) {
            const now = util.nowMs();
            const rows = this.conn.query(
                "SELECT v, version FROM _hull_kv WHERE ns = ? AND k = ? AND expires_at > ?",
                [this.ns, khex, now]);
            if (!rows || rows.length === 0) {
                const exp = util.expiryMs(ttl, this.defaultTtl);
                const expVal = exp === null ? util.NO_EXPIRY : exp;
                const n = this.conn.exec(
                    "INSERT INTO _hull_kv (ns, k, v, expires_at, version, created_at, updated_at) " +
                    "VALUES (?, ?, ?, ?, 1, ?, ?) ON CONFLICT (ns, k) DO NOTHING",
                    [this.ns, khex, util.b64encode(String(by)), expVal, now, now]);
                if ((n || 0) === 1) return by;
            } else {
                const cur = util.toInt(util.b64decode(rows[0].v));
                const ver = Number(rows[0].version);
                const nv = cur + by;
                const n = this.conn.exec(
                    "UPDATE _hull_kv SET v = ?, version = version + 1, updated_at = ? " +
                    "WHERE ns = ? AND k = ? AND version = ?",
                    [util.b64encode(String(nv)), now, this.ns, khex, ver]);
                if ((n || 0) === 1) return nv;
            }
            // lost the race; retry
        }
        util.error("conflict", "kv: incr contended past retry budget");
    }

    clear() { this.conn.exec("DELETE FROM _hull_kv WHERE ns = ?", [this.ns]); }

    scan(prefix, limit) {
        prefix = prefix || "";
        let sql = "SELECT k FROM _hull_kv WHERE ns = ? AND k LIKE ? AND expires_at > ?";
        if (limit && limit > 0) sql += " LIMIT " + String(Math.floor(limit));
        const rows = this.conn.query(sql, [this.ns, util.hexencode(prefix) + "%", util.nowMs()]);
        const out = [];
        for (let i = 0; i < (rows ? rows.length : 0); i++) out.push(util.hexdecode(rows[i].k));
        return out;
    }

    cleanup() {
        return this.conn.exec(
            "DELETE FROM _hull_kv WHERE ns = ? AND expires_at <= ?",
            [this.ns, util.nowMs()]) || 0;
    }

    stats() {
        const rows = this.conn.query(
            "SELECT COUNT(*) AS n FROM _hull_kv WHERE ns = ? AND expires_at > ?",
            [this.ns, util.nowMs()]);
        return { items: rows && rows[0] ? Number(rows[0].n) : 0 };
    }
}

function newStore(conn, namespace, policy) {
    return new SqlStore(conn, namespace, policy || {});
}

export default { new: newStore };
