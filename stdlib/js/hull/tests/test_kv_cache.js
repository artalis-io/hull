// test_kv_cache.js - hull:cache / hull:kv memory + SQL stores, rbac names
//
// Audit 5 (DA-L3..L6). Needs time + db: runs via run_js_test in the
// caps-bearing context (tests/hull/runtime/js/test_js.c).

import { cache } from "hull:cache";
import { kv } from "hull:kv";
import { rbac } from "hull:web:middleware:rbac";
import { db as dbMod } from "hull:db";

const db = dbMod.default();

let pass = 0;
let fail = 0;

function test(name, fn) {
    try { fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + (e && e.message)); }
}
function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + b + ", got " + a);
}
function assertTrue(v, msg) { if (!v) throw new Error((msg || "") + " expected truthy"); }
function code(f) { try { f(); return null; } catch (e) { return e.code || e.message; } }
function threw(f) { try { f(); return false; } catch (_) { return true; } }

// ── memory store: overwrite larger than the budget (DA-L3) ─────────────

test("memstore overwrite never evicts its own key", () => {
    // entry bytes = k.length + v.length + 48
    const h = cache.open({ backend: "memory", namespace: "t5-own", maxBytes: 1000 });
    h.set("a", "x".repeat(600));
    assertEq(code(() => h.set("a", "y".repeat(1100))), "capacity_exceeded");
    assertEq(h.get("a"), "x".repeat(600));
    let st = h._store.stats();
    assertEq(st.items, 1); assertEq(st.bytes, 1 + 600 + 48);
    h.set("b", "b".repeat(200));
    h.set("a", "z".repeat(900));
    assertEq(h.get("a"), "z".repeat(900));
    assertEq(h.get("b"), null, "b evicted");
    st = h._store.stats();
    assertEq(st.items, 1); assertEq(st.bytes, 1 + 900 + 48);
});

test("memstore LRU evicts by access order", () => {
    const h = cache.open({ backend: "memory", namespace: "t5-lru", maxItems: 3 });
    h.set("a", "1"); h.set("b", "2"); h.set("c", "3");
    assertEq(h.get("a"), "1");
    h.set("d", "4");
    assertEq(h.get("b"), null);
    assertEq(h.get("a"), "1"); assertEq(h.get("c"), "3"); assertEq(h.get("d"), "4");
});

test("kv memory (no eviction) refuses when full", () => {
    const h = kv.open({ backend: "memory", namespace: "t5-kvfull", maxItems: 1 });
    h.set("a", "1");
    assertEq(code(() => h.set("b", "2")), "capacity_exceeded");
    h.set("a", "22");
    assertEq(h.get("a"), "22");
});

test("cache.new LRU", () => {
    const c = cache.new({ maxEntries: 2 });
    c.set("a", 1); c.set("b", 2);
    c.get("a");
    c.set("c", 3);
    assertEq(c.get("b"), null);
    assertEq(c.get("a"), 1);
});

// ── SQL cache (DA-L5) ──────────────────────────────────────────────────

test("sql cache purges expired rows before evicting live ones", () => {
    const h = cache.open({ backend: "sqlite", database: db, namespace: "t5-sql", maxItems: 5 });
    for (let i = 1; i <= 5; i++) h.set("live" + i, "v");
    for (let i = 1; i <= 6; i++) h.set("dead" + i, "v", { ttl: 0 });
    for (let i = 1; i <= 5; i++) assertEq(h.get("live" + i), "v", "live" + i);
    assertEq(h._store.stats().items, 5);
});

test("sql cache stays bounded", () => {
    const h = cache.open({ backend: "sqlite", database: db, namespace: "t5-sql2", maxItems: 10 });
    for (let i = 1; i <= 40; i++) h.set("k" + i, String(i));
    assertTrue(h._store.stats().items <= 10, "bounded");
    assertEq(h.get("k40"), "40");
});

test("sql cache counts keys cas and incr create (audit 6 L4)", () => {
    const h = cache.open({ backend: "sqlite", database: db, namespace: "t6-sql3", maxItems: 10 });
    for (let i = 1; i <= 40; i++) h._store.incr("c" + i, 1);
    for (let i = 1; i <= 40; i++) h._store.cas("l" + i, null, "x");
    const n = h._store.stats().items;
    assertTrue(n <= 10, "bounded: " + n);
});

// ── bounds: NaN / infinity / negative refused (audit 12) ──────────────

test("cache.new refuses a bad maxEntries / defaultTtl / ttl", () => {
    for (const m of [NaN, Infinity, -1, 0, 1.5, "10"])
        assertTrue(threw(() => cache.new({ maxEntries: m })), "maxEntries " + m);
    for (const t of [NaN, Infinity, -1, "5"])
        assertTrue(threw(() => cache.new({ defaultTtl: t })), "defaultTtl " + t);
    const c = cache.new({ maxEntries: 2, defaultTtl: 60 });
    for (const t of [NaN, Infinity, -5])
        assertTrue(threw(() => c.set("k", 1, t)), "ttl " + t);
    assertEq(c.size(), 0, "nothing stored by a refused set");
    c.set("k", 1, 0); assertEq(c.get("k"), null, "ttl 0 expires at once");
    c.set("k", 2); assertEq(c.get("k"), 2);
});

test("kv / cache.open refuse a bad ttl, defaultTtl, maxItems, maxBytes", () => {
    const h = kv.open({ backend: "memory", namespace: "t12-ttl" });
    for (const t of [NaN, Infinity, -1])
        assertEq(code(() => h.set("k", "v", { ttl: t })), "invalid_argument", "ttl " + t);
    assertEq(h.get("k"), null);
    h.set("k", "v", { ttl: false }); assertEq(h.get("k"), "v", "false = no expiry");
    let i = 0;
    for (const o of [{ defaultTtl: NaN }, { defaultTtl: -1 }, { defaultTtl: Infinity },
                     { maxItems: Infinity }, { maxBytes: Infinity }, { maxItems: NaN },
                     { maxItems: -1 }]) {
        o.backend = "memory"; o.namespace = "t12-bad-" + (i++);
        assertEq(code(() => cache.open(o)), "invalid_argument", JSON.stringify(o));
    }
    assertEq(code(() => cache.open({ backend: "sqlite", database: db, namespace: "t12-sql",
                                     defaultTtl: NaN })), "invalid_argument");
});

// ── rbac names (DA-L6 parity) ──────────────────────────────────────────

test("rbac refuses bad names, lookups answer false", () => {
    rbac.init();
    assertTrue(threw(() => rbac.assign("u1", null)));
    assertTrue(threw(() => rbac.assign("u1", 1)));
    assertTrue(threw(() => rbac.defineRole("r".repeat(256))));
    assertTrue(!threw(() => rbac.defineRole("r".repeat(255))));
    // Counted by codepoint, as Lua counts (audit 6 L6): 255 emoji are 510 units.
    assertTrue(!threw(() => rbac.defineRole("\u{1F600}".repeat(255))));
    assertTrue(threw(() => rbac.defineRole("\u{1F600}".repeat(256))));
    rbac.assign("u1", "editor");
    assertTrue(rbac.hasRole("u1", "editor"));
    assertEq(rbac.hasRole("u1", null), false);
    assertEq(rbac.hasPermission("u1", 1), false);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
