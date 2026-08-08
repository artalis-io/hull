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

const DEFAULT_MAX = 1000;

/**
 * Create an isolated cache instance.
 * @param {Object} [opts]  `maxEntries` (default 1000), `defaultTtl` (seconds;
 *   used by set/fetch when no ttl is passed; default no expiry).
 * @returns {Object} Instance with get/set/fetch/has/delete/clear/size.
 */
function newCache(opts) {
    opts = opts || {};
    let store = new Map();     // key -> { value, expires (ms|null), seq }
    let count = 0;
    let seq = 0;
    const max = opts.maxEntries || DEFAULT_MAX;
    const defaultTtl = opts.defaultTtl;

    const nextSeq = () => ++seq;

    // Return the entry if live; drop + return null if it has expired.
    const live = (key) => {
        const e = store.get(key);
        if (!e) return null;
        if (e.expires != null && time.nowMs() >= e.expires) {
            store.delete(key);
            count -= 1;
            return null;
        }
        return e;
    };

    // Make room: drop an expired entry if any, else the least-recently-used.
    const evictOne = () => {
        const now = time.nowMs();
        let lruKey, lruSeq;
        for (const [k, e] of store) {
            if (e.expires != null && now >= e.expires) {
                store.delete(k);
                count -= 1;
                return;
            }
            if (lruSeq === undefined || e.seq < lruSeq) {
                lruKey = k;
                lruSeq = e.seq;
            }
        }
        if (lruKey !== undefined) {
            store.delete(lruKey);
            count -= 1;
        }
    };

    const get = (key) => {
        const e = live(key);
        if (!e) return null;
        e.seq = nextSeq();
        return e.value;
    };

    const has = (key) => live(key) !== null;

    const set = (key, value, ttl) => {
        if (ttl === undefined) ttl = defaultTtl;
        let expires = null;
        if (ttl != null) expires = time.nowMs() + ttl * 1000;
        const e = store.get(key);
        if (e === undefined) {
            if (count >= max) evictOne();
            store.set(key, { value, expires, seq: nextSeq() });
            count += 1;
        } else {
            e.value = value;
            e.expires = expires;
            e.seq = nextSeq();
        }
        return value;
    };

    const fetch = (key, ttl, fn) => {
        if (fn === undefined && typeof ttl === "function") {
            fn = ttl;
            ttl = undefined;
        }
        const e = live(key);
        if (e) {
            e.seq = nextSeq();
            return e.value;
        }
        const v = fn();
        set(key, v, ttl);
        return v;
    };

    const del = (key) => {
        if (store.has(key)) {
            store.delete(key);
            count -= 1;
            return true;
        }
        return false;
    };

    const clear = () => {
        store = new Map();
        count = 0;
    };

    const size = () => count;

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

export { cache };
export default cache;
