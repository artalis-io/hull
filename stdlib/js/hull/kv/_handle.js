/*
 * hull:kv:_handle - the semantic surface over a backend store (JS mirror of
 * hull.kv._handle). Key/value validation + capability enforcement (an optional
 * op on a backend that lacks the cap throws `unsupported`). Shared by hull.kv
 * and hull.cache; hull.cache layers fetch() on top.
 *
 * Internal module. SPDX-License-Identifier: AGPL-3.0-or-later
 */

import util from "hull:kv:_util";

// `meta.seal` (a hull:kv:_seal) makes the handle encrypted: values are sealed
// on the way in and opened on the way out. Only hull:kv passes one.
function build(store, meta) {
    const seal = meta.seal || null;
    const need = (cap) => {
        if (!store.caps[cap])
            util.error("unsupported",
                "kv: backend '" + meta.backend + "' does not support " + cap);
    };
    const absent = (v) => v === undefined || v === null;
    const handle = {
        namespace: meta.namespace,
        backend: meta.backend,
        caps: store.caps,

        get(k) {
            util.checkKey(k);
            const stored = store.get(k);
            if (absent(stored) || !seal) return stored;
            return seal.open(k, stored)[0];
        },

        set(k, v, opts) {
            util.checkKey(k); util.checkValue(v);
            store.put(k, seal ? seal.seal(k, v) : v, opts && opts.ttl);
            return true;
        },

        delete(k) { util.checkKey(k); return store.del(k); },

        exists(k) { util.checkKey(k); return store.has(k); },

        incr(k, by, opts) {
            need("atomic_increment"); util.checkKey(k);
            if (seal) {
                // The backend adds to the stored bytes, which it cannot do to
                // a sealed value. A secret counter is a value updated with cas.
                util.error("unsupported", "kv: incr is not available on an encrypted "
                    + "handle; store the counter as a value and update it with cas");
            }
            if (by === undefined) by = 1;
            if (typeof by !== "number" || !Number.isInteger(by))
                util.error("invalid_argument", "kv: incr amount must be an integer");
            return store.incr(k, by, opts && opts.ttl);
        },

        cas(k, expected, newVal, opts) {
            need("compare_exchange"); util.checkKey(k); util.checkValue(newVal);
            if (!absent(expected)) util.checkValue(expected);
            const ttl = opts && opts.ttl;
            if (!seal) return store.cas(k, expected, newVal, ttl);
            // Encrypted: compare the OPENED value, then swap on the exact
            // stored bytes (each seal has a fresh nonce, so sealed bytes never
            // equal `expected`). Still atomic: the backend compares what it holds.
            const sealedNew = seal.seal(k, newVal);
            if (absent(expected)) return store.cas(k, null, sealedNew, ttl);
            const stored = store.get(k);
            if (absent(stored)) return false;
            if (seal.open(k, stored)[0] !== expected) return false;
            return store.cas(k, stored, sealedNew, ttl);
        },

        // Re-seal every value under `prefix` (all, by default) not sealed
        // with the current key, so an old key can then be dropped. Returns how
        // many were re-sealed. Safe to interrupt and rerun; a value changed by
        // someone else meanwhile is left alone. Needs scan and compare-and-swap.
        rekey(prefix, opts) {
            if (!seal) util.error("invalid_argument", "kv: rekey needs an encrypted handle");
            need("scan"); need("compare_exchange");
            let n = 0;
            for (const k of handle.scan(prefix || "")) {
                const stored = store.get(k);
                if (absent(stored)) continue;
                const [value, version] = seal.open(k, stored);
                if (!seal.isCurrent(version)
                    && store.cas(k, stored, seal.seal(k, value), opts && opts.ttl)) n++;
            }
            return n;
        },

        clear() { store.clear(); return true; },

        scan(prefix, opts) {
            need("scan");
            if (prefix !== undefined && prefix !== null && typeof prefix !== "string")
                util.error("invalid_argument", "kv: scan prefix must be a string (bytes)");
            const limit = util.checkCount(opts && opts.limit, "scan limit");
            return store.scan(prefix, limit);
        },

        cleanup() { return store.cleanup(); },

        stats() { return store.stats(); },

        // Release an owned backend connection (the networked valkey/redis
        // store). A no-op for the memory/sql stores (borrowed/shared); GC
        // finalizes an unclosed network connection anyway.
        close() { if (store.close) store.close(); },

        // Exposed for hull.cache.open's fetch() (byte-cache get-or-compute).
        _store: store,
    };
    return handle;
}

export default { build };
