#!/bin/sh
# e2e_cache_module.sh: hull.cache (the app-facing key/value cache) behavior +
# Lua/JS parity. (Distinct from e2e_cache.sh, which tests the `hull cache` CLI.)
#
# Exercises the full surface deterministically (no wall-clock waits): miss,
# set/get, has, fetch memoization (fn runs once), ttl=0 immediate expiry,
# delete idempotence, and LRU eviction at a max_entries cap. Asserts an
# identical result vector across runtimes.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

cat > "$WD/c.lua" <<'LUA'
local cache = require("hull.cache")
app.manifest({ modules = { "hull/cache@1" } })
app.main(function(ctx)
    local o = {}
    cache.clear()
    o[#o+1] = cache.get("miss") == nil and "miss_nil" or "F1"
    cache.set("k", "v")
    o[#o+1] = cache.get("k") == "v" and "set_get" or "F2"
    o[#o+1] = (cache.has("k") and cache.has("nope") == false) and "has" or "F3"
    local calls = 0
    local a = cache.fetch("f", 60, function() calls = calls + 1; return "c" end)
    local b = cache.fetch("f", 60, function() calls = calls + 1; return "c" end)
    o[#o+1] = (a == "c" and b == "c" and calls == 1) and "fetch_memo" or "F4"
    cache.set("z", "v", 0)
    o[#o+1] = cache.get("z") == nil and "ttl0_expired" or "F5"
    cache.set("d", "x")
    local d1 = cache.delete("d")
    local d2 = cache.delete("d")
    o[#o+1] = (d1 == true and d2 == false and cache.get("d") == nil) and "delete" or "F6"
    local c2 = cache.new({ max_entries = 2 })
    c2.set("a", 1); c2.set("b", 2); c2.get("a"); c2.set("c", 3)  -- b is LRU -> evicted
    o[#o+1] = (c2.has("a") and not c2.has("b") and c2.has("c") and c2.size() == 2) and "lru" or "F7"
    ctx.stdout:write(table.concat(o, "|") .. "\n"); return 0
end)
LUA
cat > "$WD/c.js" <<'JS'
import { app } from "hull:app"; import { cache } from "hull:cache";
app.manifest({ modules: ["hull/cache@1"] });
app.main((ctx) => {
    const o = [];
    cache.clear();
    o.push(cache.get("miss") === null ? "miss_nil" : "F1");
    cache.set("k", "v");
    o.push(cache.get("k") === "v" ? "set_get" : "F2");
    o.push((cache.has("k") && cache.has("nope") === false) ? "has" : "F3");
    let calls = 0;
    const a = cache.fetch("f", 60, () => { calls++; return "c"; });
    const b = cache.fetch("f", 60, () => { calls++; return "c"; });
    o.push((a === "c" && b === "c" && calls === 1) ? "fetch_memo" : "F4");
    cache.set("z", "v", 0);
    o.push(cache.get("z") === null ? "ttl0_expired" : "F5");
    cache.set("d", "x");
    const d1 = cache.delete("d"); const d2 = cache.delete("d");
    o.push((d1 === true && d2 === false && cache.get("d") === null) ? "delete" : "F6");
    const c2 = cache.new({ maxEntries: 2 });
    c2.set("a", 1); c2.set("b", 2); c2.get("a"); c2.set("c", 3);
    o.push((c2.has("a") && !c2.has("b") && c2.has("c") && c2.size() === 2) ? "lru" : "F7");
    ctx.stdout.write(o.join("|") + "\n"); return 0;
});
JS

expect="miss_nil|set_get|has|fetch_memo|ttl0_expired|delete|lru"
lua_out="$("$HULL" "$WD/c.lua" 2>/dev/null | tail -1)"
js_out="$( "$HULL" "$WD/c.js"  2>/dev/null | tail -1)"

if [ "$lua_out" = "$expect" ] && [ "$lua_out" = "$js_out" ]; then
    echo "PASS: hull.cache behavior is byte-identical across Lua and JS"
else
    echo "::error hull.cache DRIFT:"
    echo "  expect=$expect"
    echo "  lua   =$lua_out"
    echo "  js    =$js_out"
    exit 1
fi
