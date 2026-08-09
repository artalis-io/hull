/*
 * hull:kv:_handle - the semantic surface over a backend store (JS mirror of
 * hull.kv._handle). Key/value validation + capability enforcement (an optional
 * op on a backend that lacks the cap throws `unsupported`). Shared by hull.kv
 * and hull.cache; hull.cache layers fetch() on top.
 *
 * Internal module. SPDX-License-Identifier: AGPL-3.0-or-later
 */

import util from "hull:kv:_util";

function build(store, meta) {
    const need = (cap) => {
        if (!store.caps[cap])
            util.error("unsupported",
                "kv: backend '" + meta.backend + "' does not support " + cap);
    };
    return {
        namespace: meta.namespace,
        backend: meta.backend,
        caps: store.caps,

        get(k) { util.checkKey(k); return store.get(k); },

        set(k, v, opts) {
            util.checkKey(k); util.checkValue(v);
            store.put(k, v, opts && opts.ttl);
            return true;
        },

        delete(k) { util.checkKey(k); return store.del(k); },

        exists(k) { util.checkKey(k); return store.has(k); },

        incr(k, by, opts) {
            need("atomic_increment"); util.checkKey(k);
            if (by === undefined) by = 1;
            if (typeof by !== "number" || !Number.isInteger(by))
                util.error("invalid_argument", "kv: incr amount must be an integer");
            return store.incr(k, by, opts && opts.ttl);
        },

        cas(k, expected, newVal, opts) {
            need("compare_exchange"); util.checkKey(k); util.checkValue(newVal);
            if (expected !== undefined && expected !== null) util.checkValue(expected);
            return store.cas(k, expected, newVal, opts && opts.ttl);
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
}

export default { build };
