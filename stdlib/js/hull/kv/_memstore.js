/*
 * hull:kv:_memstore - native in-process byte store (JS mirror of
 * hull.kv._memstore). One physical store, two policies: hull.kv memory
 * (eviction off) and hull.cache memory (LRU eviction on). Byte-string keys and
 * values; lazy TTL expiry; byte + item accounting; LRU by Map insertion
 * order (a touch deletes and re-inserts, so the first key is the least
 * recently used and eviction is O(1)).
 * Single-threaded (event-loop affinity); no locking. Stores are keyed by
 * namespace at module level, so same-namespace opens share state.
 *
 * Internal module. SPDX-License-Identifier: AGPL-3.0-or-later
 */

import util from "hull:kv:_util";

const REGISTRY = new Map(); // namespace -> Store
const ENTRY_OVERHEAD = 48;

class Store {
    constructor(policy) {
        this.data = new Map(); // key -> { v, exp(ms|null), bytes }, in recency order
        this.items = 0;
        this.bytes = 0;
        this.maxBytes = util.checkCount(policy.maxBytes, "max_bytes") || 0;
        this.maxItems = util.checkCount(policy.maxItems, "max_items") || 0;
        this.defaultTtl = util.checkTtl(policy.defaultTtl, "default_ttl");
        this.evict = !!policy.evict;
        this.st = { hits: 0, misses: 0, evictions: 0, expirations: 0 };
        this.caps = {
            ttl: true, atomic_increment: true, compare_exchange: true,
            scan: true, persistent: false, shared: false,
            eviction: !!policy.evict, transactions: false,
        };
    }

    _drop(k, e) { this.data.delete(k); this.items -= 1; this.bytes -= e.bytes; }

    _live(k) {
        const e = this.data.get(k);
        if (!e) return null;
        if (e.exp !== null && util.nowMs() >= e.exp) {
            this._drop(k, e);
            this.st.expirations += 1;
            return null;
        }
        return e;
    }

    _touch(k, e) { this.data.delete(k); this.data.set(k, e); }

    // Evict least-recently-used entries until the incoming bytes fit. `keep`
    // (the key being overwritten) is never evicted - see the Lua sibling.
    _makeRoom(addBytes, addingItem, keep) {
        const over = () =>
            (this.maxItems > 0 && this.items + (addingItem ? 1 : 0) > this.maxItems) ||
            (this.maxBytes > 0 && this.bytes + addBytes > this.maxBytes);
        if (!over()) return;
        if (!this.evict)
            util.error("capacity_exceeded", "kv: in-memory store is full (eviction disabled)");
        const it = this.data.keys();
        while (over()) {
            const n = it.next();
            if (n.done) break;
            if (n.value === keep) continue;
            this._drop(n.value, this.data.get(n.value));
            this.st.evictions += 1;
        }
        if (over())
            util.error("capacity_exceeded", "kv: value larger than the cache byte budget");
    }

    _checkFits(nb) {
        if (this.maxBytes > 0 && nb > this.maxBytes)
            util.error("capacity_exceeded", "kv: value larger than the cache byte budget");
    }

    get(k) {
        const e = this._live(k);
        if (!e) { this.st.misses += 1; return null; }
        this._touch(k, e);
        this.st.hits += 1;
        return e.v;
    }

    has(k) { return this._live(k) !== null; }

    put(k, v, ttl) {
        const exp = util.expiryMs(ttl, this.defaultTtl);
        const nb = k.length + v.length + ENTRY_OVERHEAD;
        this._checkFits(nb);
        const old = this.data.get(k);
        if (old) {
            const delta = nb - old.bytes;
            if (delta > 0) this._makeRoom(delta, false, k);
            this._touch(k, old);
            this.bytes = this.bytes - old.bytes + nb;
            old.v = v; old.exp = exp; old.bytes = nb;
            return v;
        }
        this._makeRoom(nb, true);
        this.data.set(k, { v, exp, bytes: nb });
        this.items += 1; this.bytes += nb;
        return v;
    }

    del(k) {
        const e = this.data.get(k);
        if (!e) return false;
        this._drop(k, e);
        return true;
    }

    incr(k, by, ttl) {
        const e = this._live(k);
        if (e) {
            // Preserve the existing expiry (matches the SQL backend and Redis
            // INCR); only the value changes.
            const newv = String(util.toInt(e.v) + by);
            const nb = k.length + newv.length + ENTRY_OVERHEAD;
            const delta = nb - e.bytes;
            this._checkFits(nb);
            if (delta > 0) this._makeRoom(delta, false, k);
            this._touch(k, e);
            this.bytes = this.bytes - e.bytes + nb;
            e.v = newv; e.bytes = nb;
            return util.toInt(newv);
        }
        this.put(k, String(by), ttl); // fresh key: apply ttl
        return by;
    }


    // compare-and-swap; expected undefined/null = set only if absent.
    cas(k, expected, newVal, ttl) {
        const e = this._live(k);
        const cur = e ? e.v : null;
        const exp = (expected === undefined || expected === null) ? null : expected;
        if (cur !== exp) return false;
        if (ttl === util.KEEP_TTL) {
            const keepExp = e ? e.exp : null;
            this.put(k, newVal, false);
            this.data.get(k).exp = keepExp;
        } else {
            this.put(k, newVal, ttl);
        }
        return true;
    }

    clear() { this.data = new Map(); this.items = 0; this.bytes = 0; }

    scan(prefix, limit) {
        prefix = prefix || "";
        const out = [];
        for (const k of this.data.keys()) {
            const e = this.data.get(k);
            if ((e.exp === null || util.nowMs() < e.exp) && k.startsWith(prefix)) {
                out.push(k);
                if (limit && out.length >= limit) break;
            }
        }
        return out;
    }

    cleanup() {
        const now = util.nowMs();
        let removed = 0;
        for (const k of this.data.keys()) {
            const e = this.data.get(k);
            if (e.exp !== null && now >= e.exp) { this._drop(k, e); removed += 1; }
        }
        this.st.expirations += removed;
        return removed;
    }

    stats() {
        return {
            hits: this.st.hits, misses: this.st.misses,
            evictions: this.st.evictions, expirations: this.st.expirations,
            items: this.items, bytes: this.bytes,
        };
    }
}

function get(namespace, policy) {
    let s = REGISTRY.get(namespace);
    if (s) return s;
    s = new Store(policy || {});
    REGISTRY.set(namespace, s);
    return s;
}

function _forget(namespace) { REGISTRY.delete(namespace); }

export default { get, _forget };
