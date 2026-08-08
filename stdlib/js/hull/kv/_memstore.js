/*
 * hull:kv:_memstore - native in-process byte store (JS mirror of
 * hull.kv._memstore). One physical store, two policies: hull.kv memory
 * (eviction off) and hull.cache memory (LRU eviction on). Byte-string keys and
 * values; lazy TTL expiry; byte + item accounting; LRU by access sequence.
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
        this.data = new Map(); // key -> { v, exp(ms|null), bytes, seq }
        this.seq = 0;
        this.items = 0;
        this.bytes = 0;
        this.maxBytes = util.checkCount(policy.maxBytes, "max_bytes") || 0;
        this.maxItems = util.checkCount(policy.maxItems, "max_items") || 0;
        this.defaultTtl = policy.defaultTtl;
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

    _makeRoom(addBytes, addingItem) {
        const over = () =>
            (this.maxItems > 0 && this.items + (addingItem ? 1 : 0) > this.maxItems) ||
            (this.maxBytes > 0 && this.bytes + addBytes > this.maxBytes);
        if (!over()) return;
        if (!this.evict)
            util.error("capacity_exceeded", "kv: in-memory store is full (eviction disabled)");
        while (over() && this.items > 0) {
            let lruK, lruSeq;
            for (const k of this.data.keys()) {
                const e = this.data.get(k);
                if (lruSeq === undefined || e.seq < lruSeq) { lruK = k; lruSeq = e.seq; }
            }
            if (lruK === undefined) break;
            this._drop(lruK, this.data.get(lruK));
            this.st.evictions += 1;
        }
        if (over())
            util.error("capacity_exceeded", "kv: value larger than the cache byte budget");
    }

    get(k) {
        const e = this._live(k);
        if (!e) { this.st.misses += 1; return null; }
        this.seq += 1; e.seq = this.seq;
        this.st.hits += 1;
        return e.v;
    }

    has(k) { return this._live(k) !== null; }

    put(k, v, ttl) {
        const exp = util.expiryMs(ttl, this.defaultTtl);
        const nb = k.length + v.length + ENTRY_OVERHEAD;
        const old = this.data.get(k);
        if (old) {
            // Bump to MRU first so make_room never evicts the key being updated.
            this.seq += 1; old.seq = this.seq;
            const delta = nb - old.bytes;
            if (delta > 0) this._makeRoom(delta, false);
            this.bytes = this.bytes - old.bytes + nb;
            old.v = v; old.exp = exp; old.bytes = nb;
            return v;
        }
        this._makeRoom(nb, true);
        this.seq += 1;
        this.data.set(k, { v, exp, bytes: nb, seq: this.seq });
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
            this.seq += 1; e.seq = this.seq;
            if (delta > 0) this._makeRoom(delta, false);
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
        this.put(k, newVal, ttl);
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
