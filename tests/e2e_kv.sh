#!/bin/sh
# e2e_kv.sh: hull.kv + hull.cache.open behavior, cross-BACKEND conformance
# (memory vs sqlite behave identically) and cross-RUNTIME parity (Lua == JS).
#
# Part A - a deterministic conformance vector run against BOTH the memory and
#          the sqlite backend inside one process; the app asserts they match,
#          then prints the vector. The script asserts Lua's vector == JS's.
# Part B - durability: a writer process stores durable sqlite KV; a fresh reader
#          process reads it back (text, binary, a counter) - both runtimes.
# Part C - hull.cache.open eviction: LRU at max_items + a max_bytes budget.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

# ---------------------------------------------------------------------------
# Part A - conformance vector (identical across memory + sqlite, Lua + JS)
# ---------------------------------------------------------------------------
cat > "$WD/conf.lua" <<'LUA'
local kv = require("hull.kv")
app.manifest({ modules = { "hull/kv@1", "hull/db@1" } })

local function run(s)
    s:clear()
    local o = {}
    o[#o+1] = s:get("miss") == nil and "miss_nil" or "F_miss"
    s:set("k", "v")
    o[#o+1] = s:get("k") == "v" and "set_get" or "F_set"
    s:set("bv", "\0\1\2\255")
    o[#o+1] = s:get("bv") == "\0\1\2\255" and "binval" or "F_binval"
    s:set("\0bk\255", "x")
    o[#o+1] = s:get("\0bk\255") == "x" and "binkey" or "F_binkey"
    s:set("ow", "1"); s:set("ow", "2")
    o[#o+1] = s:get("ow") == "2" and "overwrite" or "F_ow"
    s:set("d", "x")
    o[#o+1] = (s:delete("d") == true and s:delete("d") == false and s:get("d") == nil)
              and "delete" or "F_del"
    o[#o+1] = (s:exists("k") and not s:exists("nope")) and "exists" or "F_exists"
    o[#o+1] = (s:incr("n", 5) == 5 and s:incr("n", 3) == 8) and "incr" or "F_incr"
    o[#o+1] = (s:cas("c", nil, "a") and not s:cas("c", "x", "b")
               and s:cas("c", "a", "b") and s:get("c") == "b") and "cas" or "F_cas"
    s:set("p:1", "x"); s:set("p:2", "y"); s:set("q:1", "z")
    o[#o+1] = #s:scan("p:") == 2 and "scan2" or "F_scan"
    s:set("e", "v", { ttl = 0 })
    o[#o+1] = s:get("e") == nil and "ttl0" or "F_ttl0"
    return table.concat(o, ",")
end

app.main(function(ctx)
    local db = require("hull.db").default()
    local mem = run(kv.open{ backend = "memory", namespace = "cm" })
    local sql = run(kv.open{ backend = "sqlite", database = db, namespace = "cs" })
    assert(mem == sql, "cross-backend drift:\n mem=" .. mem .. "\n sql=" .. sql)
    -- namespace isolation: distinct namespaces never see each other
    local a = kv.open{ backend = "memory", namespace = "ns_a" }
    local b = kv.open{ backend = "memory", namespace = "ns_b" }
    a:set("z", "A"); b:set("z", "B")
    local iso = (a:get("z") == "A" and b:get("z") == "B") and "nsiso" or "F_nsiso"
    ctx.stdout:write(mem .. "|" .. iso .. "\n")
    return 0
end)
LUA

cat > "$WD/conf.js" <<'JS'
import { app } from "hull:app";
import { kv } from "hull:kv";
import { db as dbModule } from "hull:db";
app.manifest({ modules: ["hull/kv@1", "hull/db@1"] });

function run(s) {
    s.clear();
    const o = [];
    o.push(s.get("miss") === null ? "miss_nil" : "F_miss");
    s.set("k", "v");
    o.push(s.get("k") === "v" ? "set_get" : "F_set");
    s.set("bv", "\x00\x01\x02\xff");
    o.push(s.get("bv") === "\x00\x01\x02\xff" ? "binval" : "F_binval");
    s.set("\x00bk\xff", "x");
    o.push(s.get("\x00bk\xff") === "x" ? "binkey" : "F_binkey");
    s.set("ow", "1"); s.set("ow", "2");
    o.push(s.get("ow") === "2" ? "overwrite" : "F_ow");
    s.set("d", "x");
    o.push((s.delete("d") === true && s.delete("d") === false && s.get("d") === null)
           ? "delete" : "F_del");
    o.push((s.exists("k") && !s.exists("nope")) ? "exists" : "F_exists");
    o.push((s.incr("n", 5) === 5 && s.incr("n", 3) === 8) ? "incr" : "F_incr");
    o.push((s.cas("c", null, "a") && !s.cas("c", "x", "b")
            && s.cas("c", "a", "b") && s.get("c") === "b") ? "cas" : "F_cas");
    s.set("p:1", "x"); s.set("p:2", "y"); s.set("q:1", "z");
    o.push(s.scan("p:").length === 2 ? "scan2" : "F_scan");
    s.set("e", "v", { ttl: 0 });
    o.push(s.get("e") === null ? "ttl0" : "F_ttl0");
    return o.join(",");
}

app.main((ctx) => {
    const db = dbModule.default();
    const mem = run(kv.open({ backend: "memory", namespace: "cm" }));
    const sql = run(kv.open({ backend: "sqlite", database: db, namespace: "cs" }));
    if (mem !== sql) throw new Error("cross-backend drift:\n mem=" + mem + "\n sql=" + sql);
    const a = kv.open({ backend: "memory", namespace: "ns_a" });
    const b = kv.open({ backend: "memory", namespace: "ns_b" });
    a.set("z", "A"); b.set("z", "B");
    const iso = (a.get("z") === "A" && b.get("z") === "B") ? "nsiso" : "F_nsiso";
    ctx.stdout.write(mem + "|" + iso + "\n");
    return 0;
});
JS

expect="miss_nil,set_get,binval,binkey,overwrite,delete,exists,incr,cas,scan2,ttl0|nsiso"
lua_out="$("$HULL" "$WD/conf.lua" -d "$WD/conf_lua.db" 2>/dev/null | tail -1)"
js_out="$( "$HULL" "$WD/conf.js"  -d "$WD/conf_js.db"  2>/dev/null | tail -1)"

if [ "$lua_out" = "$expect" ] && [ "$lua_out" = "$js_out" ]; then
    echo "PASS A: kv conformance identical across memory+sqlite and Lua+JS"
else
    echo "::error kv conformance DRIFT"
    echo "  expect=$expect"
    echo "  lua   =$lua_out"
    echo "  js    =$js_out"
    exit 1
fi

# ---------------------------------------------------------------------------
# Part B - durability across a process restart (write then reopen + read)
# ---------------------------------------------------------------------------
cat > "$WD/write.lua" <<'LUA'
local kv = require("hull.kv")
app.manifest({ modules = { "hull/kv@1", "hull/db@1" } })
app.main(function()
    local s = kv.open{ backend = "sqlite", database = require("hull.db").default(), namespace = "dur" }
    s:set("greeting", "hello"); s:set("bytes", "\0\255\0"); s:incr("counter", 42)
    return 0
end)
LUA
cat > "$WD/read.lua" <<'LUA'
local kv = require("hull.kv")
app.manifest({ modules = { "hull/kv@1", "hull/db@1" } })
app.main(function(ctx)
    local s = kv.open{ backend = "sqlite", database = require("hull.db").default(), namespace = "dur" }
    local ok = s:get("greeting") == "hello" and s:get("bytes") == "\0\255\0" and s:get("counter") == "42"
    ctx.stdout:write(ok and "durable_ok\n" or "durable_FAIL\n"); return 0
end)
LUA
DB="$WD/dur.db"
"$HULL" "$WD/write.lua" -d "$DB" >/dev/null 2>&1
dur_out="$("$HULL" "$WD/read.lua" -d "$DB" 2>/dev/null | tail -1)"
if [ "$dur_out" = "durable_ok" ]; then
    echo "PASS B: sqlite KV persists across process restart (reopen)"
else
    echo "::error kv durability FAIL: $dur_out"; exit 1
fi

# ---------------------------------------------------------------------------
# Part C - cache.open eviction (LRU at max_items + max_bytes budget)
# ---------------------------------------------------------------------------
cat > "$WD/cache.lua" <<'LUA'
local cache = require("hull.cache")
app.manifest({ modules = { "hull/cache@1" } })
app.main(function(ctx)
    local o = {}
    local c = cache.open{ backend = "memory", namespace = "lru", max_items = 2 }
    c:set("a", "1"); c:set("b", "2"); c:get("a"); c:set("c", "3")   -- b is LRU
    o[#o+1] = (c:get("b") == nil and c:get("a") == "1" and c:get("c") == "3"
               and c:stats().evictions >= 1) and "lru" or "F_lru"
    local cb = cache.open{ backend = "memory", namespace = "bud", max_bytes = 200 }
    for i = 1, 20 do cb:set("k" .. i, string.rep("x", 30)) end
    o[#o+1] = cb:stats().bytes <= 200 and "budget" or "F_budget"
    ctx.stdout:write(table.concat(o, ",") .. "\n"); return 0
end)
LUA
cat > "$WD/cache.js" <<'JS'
import { app } from "hull:app"; import { cache } from "hull:cache";
app.manifest({ modules: ["hull/cache@1"] });
app.main((ctx) => {
    const o = [];
    const c = cache.open({ backend: "memory", namespace: "lru", maxItems: 2 });
    c.set("a", "1"); c.set("b", "2"); c.get("a"); c.set("c", "3");
    o.push((c.get("b") === null && c.get("a") === "1" && c.get("c") === "3"
            && c.stats().evictions >= 1) ? "lru" : "F_lru");
    const cb = cache.open({ backend: "memory", namespace: "bud", maxBytes: 200 });
    for (let i = 1; i <= 20; i++) cb.set("k" + i, "x".repeat(30));
    o.push(cb.stats().bytes <= 200 ? "budget" : "F_budget");
    ctx.stdout.write(o.join(",") + "\n");
});
JS
cexp="lru,budget"
cl="$("$HULL" "$WD/cache.lua" 2>/dev/null | tail -1)"
cj="$("$HULL" "$WD/cache.js"  2>/dev/null | tail -1)"
if [ "$cl" = "$cexp" ] && [ "$cl" = "$cj" ]; then
    echo "PASS C: cache.open eviction (LRU + byte budget) identical Lua+JS"
else
    echo "::error cache.open eviction DRIFT: lua=$cl js=$cj expect=$cexp"; exit 1
fi

echo "e2e_kv: ALL PASS"
