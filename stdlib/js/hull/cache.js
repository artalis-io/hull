/**
 * @file hull:cache
 * @module hull:cache
 * @description In-memory key/value cache with TTL and get-or-compute
 *   memoization. Lua parity: `hull.cache`.
 *
 * A bounded, lazily-expiring cache for the common "compute once, reuse for a
 * while" pattern - derived values, parsed config, a hot DB row, an API response.
 * In-memory and process-local (no persistence, no cross-node sharing); values
 * are stored as-is (any value, no serialization). One small dep (time).
 *
 * The default instance backs the top-level `cache.*` calls; `cache.new(opts)`
 * gives an isolated instance (own keyspace + cap) for a library or subsystem.
 *
 * @license AGPL-3.0-or-later
 * @example
 *   import { cache } from "hull:cache";
 *   const u = cache.fetch("user:" + id, 300, () => loadUser(id));  // memoize
 *   cache.set("k", value, 60);   // ttl seconds; null = no expiry; 0 = expire now
 *   const v = cache.get("k");    // value or null
 *
 *   // async producers (http.fetch / compute.async): compose get + set so the
 *   // app controls the await:
 *   let v = cache.get(k);
 *   if (v === null) { v = await fetchRemote(); cache.set(k, v, 60); }
 */

import { time } from "hull:time";
import kvUtil from "hull:kv:_util";
import kvHandle from "hull:kv:_handle";
import kvMemstore from "hull:kv:_memstore";
import kvSql from "hull:kv:_sql";
import kvValkey from "hull:kv:_valkey";

const DEFAULT_MAX = 1000;
// Bounds cache.open applies when the caller gives neither maxItems nor
// maxBytes (the byte bound only on the memory backend).
const DEFAULT_OPEN_MAX_ITEMS = 10000;
const DEFAULT_OPEN_MAX_BYTES = 16 * 1024 * 1024;

/**
 * Create an isolated cache instance.
 * @param {Object} [opts]  `maxEntries` (default 1000), `defaultTtl` (seconds;
 *   used by set/fetch when no ttl is passed; default no expiry).
 * @returns {Object} Instance with get/set/fetch/has/delete/clear/size.
 */
function newCache(opts) {
    opts = opts || {};
    // key -> { value, expires (ms|null) }. The Map's insertion order IS the
    // LRU order: a touched entry is deleted and re-inserted at the end, so the
    // first key is always the least recently used and eviction is O(1). It
    // used to walk every entry for the lowest sequence number - a full scan per
    // new key once full, which ratelimit (10k buckets, a new one per client
    // address) paid on every request from a spread of addresses.
    let store = new Map();
    const max = opts.maxEntries || DEFAULT_MAX;
    const defaultTtl = opts.defaultTtl;

    const touch = (key, e) => { store.delete(key); store.set(key, e); };

    // Return the entry if live; drop + return null if it has expired.
    const live = (key) => {
        const e = store.get(key);
        if (!e) return null;
        if (e.expires != null && time.nowMs() >= e.expires) {
            store.delete(key);
            return null;
        }
        return e;
    };

    // Make room: drop the least-recently-used entry (the first key).
    const evictOne = () => {
        const first = store.keys().next();
        if (!first.done) store.delete(first.value);
    };

    const get = (key) => {
        const e = live(key);
        if (!e) return null;
        touch(key, e);
        return e.value;
    };

    const has = (key) => live(key) !== null;

    const set = (key, value, ttl) => {
        if (ttl === undefined) ttl = defaultTtl;
        let expires = null;
        if (ttl != null) expires = time.nowMs() + ttl * 1000;
        if (store.has(key)) {
            store.delete(key);
        } else if (store.size >= max) {
            evictOne();
        }
        store.set(key, { value, expires });
        return value;
    };

    const fetch = (key, ttl, fn) => {
        if (fn === undefined && typeof ttl === "function") {
            fn = ttl;
            ttl = undefined;
        }
        const e = live(key);
        if (e) {
            touch(key, e);
            return e.value;
        }
        const v = fn();
        // null / undefined is a miss, not a value: returned, not cached (it
        // read as a hit until the ttl). A Promise (an async fn) is cached so
        // concurrent fetches share it, but dropped when it rejects or resolves
        // to null / undefined - a cached rejection failed every later fetch.
        if (v === null || v === undefined) return v;
        set(key, v, ttl);
        if (v instanceof Promise) {
            const drop = () => {
                const cur = store.get(key);
                if (cur && cur.value === v) store.delete(key);
            };
            v.then((r) => { if (r === null || r === undefined) drop(); }, drop);
        }
        return v;
    };

    const del = (key) => store.delete(key);

    const clear = () => { store = new Map(); };

    const size = () => store.size;

    return { get, has, set, fetch, delete: del, clear, size };
}

// Default instance backs the top-level cache.* convenience API. A cache is
// intentionally cross-request shared state (like ratelimit's buckets), so a
// module-level default is correct here, not the request-scoped-global footgun.
const _default = newCache();

const cache = {
    new: newCache,
    get: _default.get,
    set: _default.set,
    fetch: _default.fetch,
    has: _default.has,
    delete: _default.delete,
    clear: _default.clear,
    size: _default.size,
};

// ---------------------------------------------------------------------------
// cache.open({}) - the byte-oriented, backend-selectable CACHE handle.
//
// Distinct from cache.new() above: cache.new is a lightweight in-process
// memoizer for arbitrary VALUES (item-count bound). cache.open deals in BYTE
// STRINGS and adds byte accounting, pluggable backends (memory / sqlite), and
// namespaces - sharing the same store cores as hull:kv but with CACHE policy
// (eviction ON). Use cache.open for max_bytes / a durable SQL cache / explicit
// namespaces; cache.new for a quick local value memoizer.
//
//   const c = cache.open({ backend: "memory", namespace: "query-ir",
//                          maxBytes: 512*1024*1024, defaultTtl: 600 });
//   c.set(k, bytes); const v = c.get(k);           // bytes | null
//   c.fetch(k, 60, () => render());                // get-or-compute (bytes)
//   c.stats();   // { hits, misses, evictions, items, bytes }
// ---------------------------------------------------------------------------
cache.open = function (opts) {
    if (typeof opts !== "object" || opts === null)
        kvUtil.error("invalid_argument", "cache.open: options object required");
    if (opts.encrypt !== undefined && opts.encrypt !== null) {
        // Refused rather than ignored: silently storing plaintext for a caller
        // that asked for encryption is the one wrong answer. Encrypted values
        // belong in hull:kv (docs/kv_cache.md "Encryption at rest").
        kvUtil.error("invalid_argument", "cache.open: encrypt is not supported; use hull:kv");
    }
    const backend = opts.backend || "memory";
    const namespace = opts.namespace || "default";
    const storeNs = "cache:" + namespace; // isolated from hull:kv's "kv:" namespaces

    // A cache is bounded unless the caller says otherwise: with neither bound
    // given, the memory and SQL caches defaulted to NONE, so keying one by
    // request path grew it until the VM's memory limit (or the SQL table
    // forever). An explicit 0 still means "no limit".
    let maxItems = opts.maxItems, maxBytes = opts.maxBytes;
    if ((maxItems === undefined || maxItems === null) &&
        (maxBytes === undefined || maxBytes === null)) {
        maxItems = DEFAULT_OPEN_MAX_ITEMS;
        if (backend === "memory") maxBytes = DEFAULT_OPEN_MAX_BYTES;
    }

    let store, bname;
    if (backend === "memory") {
        store = kvMemstore.get(storeNs, {
            evict: true, defaultTtl: opts.defaultTtl, maxBytes, maxItems,
        });
        bname = "memory";
    } else if (backend === "sqlite") {
        const conn = opts.database;
        if (!conn || typeof conn.exec !== "function")
            kvUtil.error("invalid_argument", "cache.open: sqlite backend needs database: <db connection>");
        store = kvSql.new(conn, storeNs, {
            evict: true, defaultTtl: opts.defaultTtl, maxItems,
        });
        bname = conn.backendName || "sqlite";
    } else if (backend === "valkey" || backend === "redis") {
        // Networked cache over the composed Valkey/Redis backend. Server-side
        // TTL + maxmemory eviction; caller-owned connection (h.close() / GC).
        [store, bname] = kvValkey.new(opts, storeNs);
    } else {
        kvUtil.error("invalid_argument", "cache.open: unknown backend '" + backend + "'");
    }

    const h = kvHandle.build(store, { namespace, backend: bname });

    // Get key, or compute + cache it. fetch(key, ttl, fn) or fetch(key, fn).
    // fn must return bytes (a string); it is synchronous.
    h.fetch = function (key, ttl, fn) {
        if (fn === undefined && typeof ttl === "function") { fn = ttl; ttl = undefined; }
        kvUtil.checkKey(key);
        const v = h._store.get(key);
        if (v !== null) return v;
        const produced = fn();
        kvUtil.checkValue(produced);
        h._store.put(key, produced, ttl);
        return produced;
    };

    return h;
};

export { cache };
export default cache;
