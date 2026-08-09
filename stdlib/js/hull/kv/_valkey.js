// hull:kv:_valkey - Valkey/Redis store backing hull:kv and hull:cache (JS).
//
// Wraps the base-resident KV connection cap (hull:kv:_native, over cap/kv.c ->
// the composed HlKvBackend) in the store interface _handle expects
// (get/put/del/has/incr/cas/clear/scan/cleanup/stats/close + caps). Reached only
// when the app composes the feature (hull build --with=valkey); on a plain base
// the native open() throws with an install hint.
//
// Values and keys are byte strings at the kv API (each char a byte 0-255,
// matching the memory/sql stores). The native connection is binary-safe over
// ArrayBuffers, so this wrapper converts byte string <-> ArrayBuffer at the
// boundary - which also means the get() copy (JS_NewArrayBufferCopy in the
// binding) lands in a runtime-owned buffer before any later op reuses the
// receive buffer (the borrow-copy guard).
//
// Physical-key namespacing matches the Lua wrapper byte-for-byte:
// `<type>:<hex(namespace)>:<key>` (type "kv"/"cache") so a kv and a cache
// namespace, or two namespaces one a prefix of the other, never collide on the
// shared server, and SCAN globs never see a metacharacter.
//
// Internal module. SPDX-License-Identifier: AGPL-3.0-or-later

import util from "hull:kv:_util";
import { open as nativeOpen } from "hull:kv:_native";

// HL_KV_CAP_* bit values (must match include/hull/cap/kv_backend.h).
const CAP_TTL = 1, CAP_INCR = 2, CAP_CAS = 4, CAP_SCAN = 8,
      CAP_PERSISTENT = 16, CAP_SHARED = 32, CAP_EVICTION = 64, CAP_TRANSACTIONS = 128;

// byte string -> ArrayBuffer (each char's low byte).
function toBuf(s) {
    const n = s.length, u8 = new Uint8Array(n);
    for (let i = 0; i < n; i++) u8[i] = s.charCodeAt(i) & 0xff;
    return u8.buffer;
}
// ArrayBuffer -> byte string (chunked to avoid a call-stack overflow on big buffers).
function fromBuf(ab) {
    const u8 = new Uint8Array(ab), CH = 8192;
    let out = "";
    for (let i = 0; i < u8.length; i += CH) {
        out += String.fromCharCode.apply(null, u8.subarray(i, i + CH));
    }
    return out;
}

const HEX = "0123456789abcdef";
function hex(s) {
    let out = "";
    for (let i = 0; i < s.length; i++) {
        const b = s.charCodeAt(i) & 0xff;
        out += HEX[b >> 4] + HEX[b & 15];
    }
    return out;
}

// "kv:agent-state" / "cache:x" -> "<type>:<hex(namespace)>:" physical prefix.
function physPrefix(storeNs) {
    const i = storeNs.indexOf(":");
    const ty = i < 0 ? "" : storeNs.slice(0, i);
    const rest = i < 0 ? storeNs : storeNs.slice(i + 1);
    return ty + ":" + hex(rest) + ":";
}

function ttlMs(store, ttl) {
    // Mirror util.expiryMs: undefined/null -> use the default; an explicit
    // `false` (or a nil default) means NO expiry, matching the memory/SQL
    // backends so `ttl: false` behaves the same on every backend.
    if (ttl === undefined || ttl === null) ttl = store.defaultTtl;
    if (ttl === undefined || ttl === null || ttl === false) return undefined;
    if (typeof ttl !== "number" || ttl < 0)
        util.error("invalid_argument", "kv: ttl must be a non-negative number of seconds");
    return Math.floor(ttl * 1000);
}

function makeStore(conn, storeNs, opts) {
    const pfx = physPrefix(storeNs);
    const capsBits = conn.caps();
    const physKey = (k) => toBuf(pfx + k);

    return {
        _conn: conn,
        defaultTtl: opts.defaultTtl,
        caps: {
            ttl:              (capsBits & CAP_TTL) !== 0,
            atomic_increment: (capsBits & CAP_INCR) !== 0,
            compare_exchange: (capsBits & CAP_CAS) !== 0,
            scan:             (capsBits & CAP_SCAN) !== 0,
            persistent:       (capsBits & CAP_PERSISTENT) !== 0,
            shared:           (capsBits & CAP_SHARED) !== 0,
            eviction:         (capsBits & CAP_EVICTION) !== 0,
            transactions:     (capsBits & CAP_TRANSACTIONS) !== 0,
        },

        get(k) {
            const ab = conn.get(physKey(k));
            return ab === null ? null : fromBuf(ab);
        },
        put(k, v, ttl) { conn.set(physKey(k), toBuf(v), ttlMs(this, ttl)); },
        del(k) { return conn.del(physKey(k)); },
        has(k) { return conn.has(physKey(k)); },
        incr(k, by, ttl) { return conn.incr(physKey(k), by, ttlMs(this, ttl)); },
        // native cas -> 0 ok / 1 mismatch / 2 conflict; boolean store contract is
        // "did the swap happen" (a retry-exhausted conflict is a non-swap); an
        // ERROR throws inside the native call.
        cas(k, expected, newVal, ttl) {
            const exp = (expected === undefined || expected === null) ? null : toBuf(expected);
            return conn.cas(physKey(k), exp, toBuf(newVal), ttlMs(this, ttl)) === 0;
        },
        clear() { conn.clear(toBuf(pfx)); },
        scan(prefix, limit) {
            prefix = prefix || "";
            // Match <phys_prefix><user_prefix>*; the backend strips the whole
            // match prefix, returning the suffix after the user prefix. Re-prepend
            // the user prefix to reconstruct the full app key (matching mem/sql).
            const suffixes = conn.scan(toBuf(pfx + prefix), limit || 0);
            const out = [];
            for (let i = 0; i < suffixes.length; i++) out.push(prefix + fromBuf(suffixes[i]));
            return out;
        },
        // Redis/Valkey expire keys server-side; nothing for the client to sweep.
        cleanup() { return 0; },
        stats() { return { backend: "valkey", namespace: opts.namespace, shared: true }; },
        close() { conn.close(); },
    };
}

// Open a Valkey/Redis store. opts: { dsn|url, namespace, defaultTtl }.
// storeNs: "kv:<namespace>" / "cache:<namespace>" (the collision prefix).
function newStore(opts, storeNs) {
    const dsn = opts.dsn || opts.url;
    if (typeof dsn !== "string" || dsn === "")
        util.error("invalid_argument",
            "kv.open: valkey backend needs dsn = 'valkey://...' (or 'redis://...')");
    const conn = nativeOpen(dsn);   // throws backend_error on connect failure
    const store = makeStore(conn, storeNs, opts);
    return [store, conn.backendName()];
}

export default { new: newStore };
