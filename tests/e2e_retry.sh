#!/bin/sh
# e2e_retry.sh: hull.retry generic retry-with-backoff, Lua/JS parity.
#
# Drives retry.run through: a fn that errors N times then succeeds (retry on
# error), a fn that always errors (exhaustion re-raises), a fn retried on a
# value predicate (retry_on), and the deterministic backoff curve. Uses
# base_ms=1 so the real hull.sleep between attempts is negligible. Asserts the
# result vector is byte-identical across runtimes.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

cat > "$WD/r.lua" <<'LUA'
local retry = require("hull.retry")
app.manifest({ modules = { "hull/retry@1" } })
app.main(function(ctx)
    local calls = 0
    local res = retry.run(function()
        calls = calls + 1
        if calls < 3 then error("boom") end
        return "ok:" .. calls
    end, { max_attempts = 5, base_ms = 1 })

    local ex = 0
    local ok = pcall(retry.run, function() ex = ex + 1; error("always") end,
                     { max_attempts = 3, base_ms = 1 })

    local vc = 0
    local vres = retry.run(function() vc = vc + 1; return vc end,
                           { max_attempts = 4, base_ms = 1,
                             retry_on = function(v) return v < 3 end })

    local b1 = retry.backoff(1, { base_ms = 100, cap_ms = 100000 })
    local b3 = retry.backoff(3, { base_ms = 100, cap_ms = 100000 })
    local bc = retry.backoff(9, { base_ms = 100, cap_ms = 500 })

    ctx.stdout:write(table.concat({ res, calls, ok and "N" or "T", ex, vres, vc, b1, b3, bc }, "|") .. "\n")
    return 0
end)
LUA
cat > "$WD/r.js" <<'JS'
import { app } from "hull:app"; import { retry } from "hull:retry";
app.manifest({ modules: ["hull/retry@1"] });
app.main(async (ctx) => {
    let calls = 0;
    const res = await retry.run(async () => {
        calls++;
        if (calls < 3) throw new Error("boom");
        return "ok:" + calls;
    }, { maxAttempts: 5, baseMs: 1 });

    let ex = 0, ok = "N";
    try { await retry.run(async () => { ex++; throw new Error("always"); },
                          { maxAttempts: 3, baseMs: 1 }); }
    catch (e) { ok = "T"; }

    let vc = 0;
    const vres = await retry.run(async () => { vc++; return vc; },
                                 { maxAttempts: 4, baseMs: 1, retryOn: (v) => v < 3 });

    const b1 = retry.backoff(1, { baseMs: 100, capMs: 100000 });
    const b3 = retry.backoff(3, { baseMs: 100, capMs: 100000 });
    const bc = retry.backoff(9, { baseMs: 100, capMs: 500 });

    ctx.stdout.write([res, calls, ok === "T" ? "T" : "N", ex, vres, vc, b1, b3, bc].join("|") + "\n");
    return 0;
});
JS

# success-after-2-errors / 3 calls / exhaustion re-raises / 3 err calls /
# retry_on stops at 3 / 3 val calls / backoff 100 / 400 / capped 500
expect="ok:3|3|T|3|3|3|100|400|500"
lua_out="$("$HULL" "$WD/r.lua" 2>/dev/null | tail -1)"
js_out="$( "$HULL" "$WD/r.js"  2>/dev/null | tail -1)"

if [ "$lua_out" = "$expect" ] && [ "$lua_out" = "$js_out" ]; then
    echo "PASS: hull.retry is byte-identical across Lua and JS"
else
    echo "::error hull.retry DRIFT:"
    echo "  expect=$expect"
    echo "  lua   =$lua_out"
    echo "  js    =$js_out"
    exit 1
fi
