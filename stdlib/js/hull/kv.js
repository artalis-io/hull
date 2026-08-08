/*
 * hull:kv - portable key/value STORE (JS mirror of hull.kv).
 *
 * KV STORE semantics (distinct from hull:cache): externally meaningful state,
 * no eviction unless requested, persistence where the backend provides it,
 * potentially shared across processes. Keys and values are byte strings (each
 * char a code unit 0-255 - Hull's JS byte convention).
 *
 * Backends (milestone 1): "memory" (in-process, process-shared by namespace),
 * "sqlite" / "postgres" (durable, over an EXISTING db connection passed as
 * `database` - no second connection stack). Each backend advertises a
 * capability set (handle.caps); an optional op a backend lacks throws
 * `unsupported`.
 *
 *   import { kv } from "hull:kv";
 *   import { db as dbModule } from "hull:db";
 *   const store = kv.open({ backend: "sqlite", database: dbModule.default(),
 *                           namespace: "agent-state" });
 *   store.set("run:123", payload);       // bytes -> bytes
 *   const v = store.get("run:123");      // bytes | null (miss)
 *   store.set(k, v, { ttl: 3600 });      // optional TTL (seconds)
 *   store.cas(k, expected, next);        // atomic compare-and-swap
 *   store.incr("hits", 1);               // atomic increment
 *   store.scan("run:");                  // prefix iteration -> [keys]
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import util from "hull:kv:_util";
import handle from "hull:kv:_handle";
import memstore from "hull:kv:_memstore";
import sqlstore from "hull:kv:_sql";

function open(opts) {
    if (typeof opts !== "object" || opts === null)
        util.error("invalid_argument", "kv.open: options object required");
    const backend = opts.backend || "memory";
    const namespace = opts.namespace || "default";
    // Prefix the physical namespace so a kv and a cache sharing a namespace
    // name never collide in the same store / _hull_kv rows.
    const storeNs = "kv:" + namespace;

    let store, bname;
    if (backend === "memory") {
        store = memstore.get(storeNs, {
            evict: false, defaultTtl: opts.defaultTtl,
            maxBytes: opts.maxBytes, maxItems: opts.maxItems,
        });
        bname = "memory";
    } else if (backend === "sqlite" || backend === "postgres") {
        const conn = opts.database;
        if (!conn || typeof conn.exec !== "function")
            util.error("invalid_argument",
                "kv.open: sql backend needs database: <db connection> "
                + "(e.g. dbModule.default())");
        store = sqlstore.new(conn, storeNs, { evict: false, defaultTtl: opts.defaultTtl });
        bname = conn.backendName || "sql";
    } else {
        util.error("invalid_argument", "kv.open: unknown backend '" + backend + "'");
    }

    return handle.build(store, { namespace, backend: bname });
}

const kv = { open, _VERSION: "1" };

export { kv };
export default kv;
