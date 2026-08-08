#!/bin/sh
# e2e_ratelimit_parity.sh: ratelimit.check (now backed by hull.cache) behavior
# + Lua/JS parity.
#
# Core rate-limiting is unchanged (count-per-window, block-when-over, window
# reset, reset timestamp). The one behavior change from the hull.cache refactor
# is the overflow policy: at the bucket cap, a NEW key is now ADMITTED by
# evicting the least-recently-used bucket, instead of being rejected - so a
# unique-key flood can't 429 a genuine new client. This asserts all of that,
# byte-identical across runtimes. check takes the caller's `now`, so it's
# deterministic (no wall-clock waits).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

cat > "$WD/r.lua" <<'LUA'
local rl = require("hull.web.middleware.ratelimit")
local cache = require("hull.cache")
app.manifest({ modules = { "hull/web/middleware/ratelimit@1", "hull/cache@1" } })
app.main(function(ctx)
    local o = {}
    -- allow first + reset timestamp
    local c1 = cache.new()
    local r1 = rl.check(c1, "ip", 5, 60, 1000)
    o[#o+1] = (r1.allowed == true and r1.remaining == 4 and r1.reset == 1060) and "allow4_ts" or "F1"
    -- block after limit
    local c2 = cache.new()
    for _ = 1, 5 do rl.check(c2, "ip", 5, 60, 1000) end
    local r2 = rl.check(c2, "ip", 5, 60, 1000)
    o[#o+1] = (r2.allowed == false and r2.remaining == 0) and "block" or "F2"
    -- reset after window
    local c3 = cache.new()
    for _ = 1, 5 do rl.check(c3, "ip", 5, 60, 1000) end
    local r3 = rl.check(c3, "ip", 5, 60, 1061)
    o[#o+1] = (r3.allowed == true and r3.remaining == 4) and "reset" or "F3"
    -- LRU overflow: at cap, a NEW key is admitted (evict LRU), not rejected
    local c4 = cache.new({ max_entries = 2 })
    rl.check(c4, "a", 1, 60, 1000)
    rl.check(c4, "b", 1, 60, 1000)
    rl.check(c4, "a", 1, 60, 1000)                       -- touch a -> b is LRU
    local rc = rl.check(c4, "c", 1, 60, 1000)            -- new key at cap
    o[#o+1] = (rc.allowed == true and c4.size() == 2) and "lru_admit" or "F5"
    ctx.stdout:write(table.concat(o, "|") .. "\n"); return 0
end)
LUA
cat > "$WD/r.js" <<'JS'
import { app } from "hull:app";
import { ratelimit as rl } from "hull:web:middleware:ratelimit";
import { cache } from "hull:cache";
app.manifest({ modules: ["hull/web/middleware/ratelimit@1", "hull/cache@1"] });
app.main((ctx) => {
    const o = [];
    const c1 = cache.new();
    const r1 = rl.check(c1, "ip", 5, 60, 1000);
    o.push((r1.allowed === true && r1.remaining === 4 && r1.reset === 1060) ? "allow4_ts" : "F1");
    const c2 = cache.new();
    for (let i = 0; i < 5; i++) rl.check(c2, "ip", 5, 60, 1000);
    const r2 = rl.check(c2, "ip", 5, 60, 1000);
    o.push((r2.allowed === false && r2.remaining === 0) ? "block" : "F2");
    const c3 = cache.new();
    for (let i = 0; i < 5; i++) rl.check(c3, "ip", 5, 60, 1000);
    const r3 = rl.check(c3, "ip", 5, 60, 1061);
    o.push((r3.allowed === true && r3.remaining === 4) ? "reset" : "F3");
    const c4 = cache.new({ maxEntries: 2 });
    rl.check(c4, "a", 1, 60, 1000);
    rl.check(c4, "b", 1, 60, 1000);
    rl.check(c4, "a", 1, 60, 1000);
    const rc = rl.check(c4, "c", 1, 60, 1000);
    o.push((rc.allowed === true && c4.size() === 2) ? "lru_admit" : "F5");
    ctx.stdout.write(o.join("|") + "\n"); return 0;
});
JS

expect="allow4_ts|block|reset|lru_admit"
lua_out="$("$HULL" "$WD/r.lua" 2>/dev/null | tail -1)"
js_out="$( "$HULL" "$WD/r.js"  2>/dev/null | tail -1)"

if [ "$lua_out" = "$expect" ] && [ "$lua_out" = "$js_out" ]; then
    echo "PASS: ratelimit.check (hull.cache-backed) is byte-identical across Lua and JS"
else
    echo "::error ratelimit DRIFT:"
    echo "  expect=$expect"
    echo "  lua   =$lua_out"
    echo "  js    =$js_out"
    exit 1
fi
