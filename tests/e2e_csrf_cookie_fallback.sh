#!/bin/sh
# e2e_csrf_cookie_fallback.sh: csrf middleware session-id resolution parity.
#
# The JS csrf resolved the session id as req.ctx[sessionKey] ELSE parse the
# session cookie, and had cookieName + requireSession opts; the Lua csrf did
# only req.ctx[session_key] (no cookie fallback, no opts) - a behavioral
# divergence (Lua under-applied CSRF when the session id lived only in the
# cookie) AND a module-resolution wrinkle (csrf.js statically imports
# hull:web:cookie, so a JS app declaring only csrf failed to resolve until
# hull/web/cookie was added to csrf's registry deps). Both fixed. This drives
# the middleware with stub req/res through the shared branches and asserts an
# identical result vector across runtimes.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

cat > "$WD/c.lua" <<'LUA'
local csrf = require("hull.web.middleware.csrf")
app.manifest({ modules = { "hull/web/middleware/csrf@1" } })   -- NOTE: cookie NOT declared; auto-admitted via csrf deps
local SEC = "test-secret-key"
local function res() return setmetatable({}, { __index = {
    status = function(self) return self end, json = function() end } }) end
app.main(function(ctx)
    local mw  = csrf.middleware({ secret = SEC })
    local mwr = csrf.middleware({ secret = SEC, require_session = true })
    local out = {}
    -- 1. GET, session only in the cookie -> token generated from the cookie sid
    local g = { method = "GET", ctx = {}, headers = { cookie = "hull_session=abc" } }
    mw(g, res()); out[#out+1] = g.ctx.csrf_token and "gen" or "nogen"
    local tok = csrf.generate("abc", SEC)
    -- 2. POST, cookie-only session + valid token -> accept (0)
    out[#out+1] = tostring(mw({ method = "POST", ctx = {},
        headers = { cookie = "hull_session=abc", ["x-csrf-token"] = tok } }, res()))
    -- 3. POST, cookie-only session + bad token -> reject (1)
    out[#out+1] = tostring(mw({ method = "POST", ctx = {},
        headers = { cookie = "hull_session=abc", ["x-csrf-token"] = "bad.tok" } }, res()))
    -- 4. POST, no session, require_session off -> pass (0)
    out[#out+1] = tostring(mw({ method = "POST", ctx = {}, headers = {} }, res()))
    -- 5. POST, no session, require_session on -> reject (1)
    out[#out+1] = tostring(mwr({ method = "POST", ctx = {}, headers = {} }, res()))
    ctx.stdout:write(table.concat(out, "|") .. "\n"); return 0
end)
LUA
cat > "$WD/c.js" <<'JS'
import { app } from "hull:app"; import { csrf } from "hull:web:middleware:csrf";
app.manifest({ modules: ["hull/web/middleware/csrf@1"] });
const SEC = "test-secret-key";
const res = () => ({ status() { return this; }, json() {} });
// req.header(name) is case-insensitive over a lowercase map.
const mkReq = (method, map) => ({ method, ctx: {}, header(n) { return map[n.toLowerCase()] || null; } });
app.main((ctx) => {
    const mw  = csrf.middleware({ secret: SEC });
    const mwr = csrf.middleware({ secret: SEC, requireSession: true });
    const out = [];
    const g = mkReq("GET", { cookie: "hull_session=abc" });
    mw(g, res()); out.push(g.ctx.csrf_token ? "gen" : "nogen");
    const tok = csrf.generate("abc", SEC);
    out.push(String(mw(mkReq("POST", { cookie: "hull_session=abc", "x-csrf-token": tok }), res())));
    out.push(String(mw(mkReq("POST", { cookie: "hull_session=abc", "x-csrf-token": "bad.tok" }), res())));
    out.push(String(mw(mkReq("POST", {}), res())));
    out.push(String(mwr(mkReq("POST", {}), res())));
    ctx.stdout.write(out.join("|") + "\n"); return 0;
});
JS

# token-from-cookie / accept valid / reject bad / pass no-session / reject (require)
expect="gen|0|1|0|1"
lua_out="$("$HULL" "$WD/c.lua" 2>/dev/null | tail -1)"
js_out="$( "$HULL" "$WD/c.js"  2>/dev/null | tail -1)"

if [ "$lua_out" = "$expect" ] && [ "$lua_out" = "$js_out" ]; then
    echo "PASS: csrf session-id resolution (cookie fallback + require_session) is identical across Lua and JS"
else
    echo "::error csrf cookie-fallback DRIFT:"
    echo "  expect=$expect"
    echo "  lua   =$lua_out"
    echo "  js    =$js_out"
    exit 1
fi
