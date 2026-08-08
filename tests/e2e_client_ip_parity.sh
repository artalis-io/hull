#!/bin/sh
# e2e_client_ip_parity.sh: Lua/JS parity for the shared client-IP helper.
#
# hull.web._request.client_ip / clientIp is the ONE home for deriving a
# request's source IP under a trust_proxy policy (XFF-first when trusted,
# remote_addr fallback, 64-char cap). Four middleware (session, audit-log,
# totp, auth-flows) delegate to it; before it existed each hand-rolled the
# extraction and they had already drifted (some capped the length, some did
# not; some named the flag trust_proxy, one trust_xff). This asserts the two
# runtimes agree byte-for-byte across the branch matrix, so a future re-roll
# or drift fails CI.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

cat > "$WD/ip.lua" <<'LUA'
local _request = require("hull.web._request")
app.manifest({ modules = {} })
local function s(v) return v == nil and "(nil)" or v end
app.main(function(ctx)
    local cases = {
        { { headers = {}, remote_addr = "10.0.0.1" }, false },
        { { headers = { ["x-forwarded-for"] = "1.1.1.1" }, remote_addr = "10.0.0.1" }, false },
        { { headers = { ["x-forwarded-for"] = "a, b, c" }, remote_addr = "10.0.0.1" }, true },
        { { headers = { ["x-forwarded-for"] = " 1.2.3.4 , x" }, remote_addr = "10.0.0.1" }, true },
        { { headers = {}, remote_addr = "10.0.0.1" }, true },
        { { headers = { ["x-forwarded-for"] = "" }, remote_addr = "10.0.0.1" }, true },
        { { headers = {} }, false },
        { { headers = {}, remote_addr = string.rep("a", 100) }, false },
    }
    local out = {}
    for _, c in ipairs(cases) do out[#out + 1] = s(_request.client_ip(c[1], c[2])) end
    out[#out + 1] = s(_request.client_ip(nil, true))
    ctx.stdout:write(table.concat(out, "|") .. "\n"); return 0
end)
LUA

cat > "$WD/ip.js" <<'JS'
import { app } from "hull:app";
import { _request } from "hull:web:_request";
app.manifest({ modules: [] });
const s = (v) => (v === null || v === undefined) ? "(nil)" : v;
app.main((ctx) => {
    const cases = [
        [{ headers: {}, remote_addr: "10.0.0.1" }, false],
        [{ headers: { "x-forwarded-for": "1.1.1.1" }, remote_addr: "10.0.0.1" }, false],
        [{ headers: { "x-forwarded-for": "a, b, c" }, remote_addr: "10.0.0.1" }, true],
        [{ headers: { "x-forwarded-for": " 1.2.3.4 , x" }, remote_addr: "10.0.0.1" }, true],
        [{ headers: {}, remote_addr: "10.0.0.1" }, true],
        [{ headers: { "x-forwarded-for": "" }, remote_addr: "10.0.0.1" }, true],
        [{ headers: {} }, false],
        [{ headers: {}, remote_addr: "a".repeat(100) }, false],
    ];
    const out = cases.map((c) => s(_request.clientIp(c[0], c[1])));
    out.push(s(_request.clientIp(null, true)));
    ctx.stdout.write(out.join("|") + "\n"); return 0;
});
JS

lua_ip="$("$HULL" "$WD/ip.lua" 2>/dev/null | tail -1)"
js_ip="$("$HULL" "$WD/ip.js"  2>/dev/null | tail -1)"

# Expected: remote_addr / remote_addr (xff untrusted) / first-of-chain /
# trimmed-first / remote_addr fallback / remote_addr (empty xff) / (nil) /
# 64-char cap / (nil) for a nil req.
cap64="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
expect="10.0.0.1|10.0.0.1|a|1.2.3.4|10.0.0.1|10.0.0.1|(nil)|${cap64}|(nil)"

if [ "$lua_ip" = "$expect" ] && [ "$lua_ip" = "$js_ip" ]; then
    echo "PASS: hull.web._request client-IP helper is byte-identical across Lua and JS"
else
    echo "::error client_ip DRIFT:"
    echo "  expect=$expect"
    echo "  lua   =$lua_ip"
    echo "  js    =$js_ip"
    exit 1
fi
