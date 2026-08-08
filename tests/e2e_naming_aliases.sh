#!/bin/sh
# e2e_naming_aliases.sh: the stdlib naming reconciliation keeps both the
# canonical option name AND its back-compat alias working, in both runtimes.
#
# Reconciled (canonical <- alias):
#   auth.session_middleware/login/logout  name          <- cookie_name
#   oauth.init / auth-flows.init           secret        <- state_secret
#   csrf.middleware / csrf.verify          ttl           <- max_age
#
# For the init-based secrets, `secret` is validated FIRST, so a valid value
# shifts the thrown error away from the ">= 32 bytes" message - that shift is
# the proof the alias was accepted (no DB / callbacks needed).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

MODS_LUA='"hull/web/middleware/csrf@1", "hull/web/middleware/auth@1", "hull/web/middleware/oauth@1", "hull/web/auth-flows@1"'
MODS_JS='"hull/web/middleware/csrf@1", "hull/web/middleware/auth@1", "hull/web/middleware/oauth@1", "hull/web/auth-flows@1"'

cat > "$WD/n.lua" <<LUA
local csrf = require("hull.web.middleware.csrf")
local auth = require("hull.web.middleware.auth")
local oauth = require("hull.web.middleware.oauth")
local authflows = require("hull.web.auth-flows")
app.manifest({ modules = { $MODS_LUA } })
local S = string.rep("x", 32)
-- secret accepted iff the throw is NOT the ">= 32 bytes" floor error.
local function sec_ok(fn, key)
    local ok, err = pcall(fn, { [key] = S })
    if ok then return true end
    return not tostring(err):find(">= 32", 1, true)
end
app.main(function(ctx)
    local o = {}
    local t = csrf.generate("sid", "sec")
    o[#o+1] = csrf.verify(t, "sid", "sec", 3600) and "csrf_rt" or "F1"
    o[#o+1] = type(csrf.middleware({ secret = "s", ttl = 60 })) == "function" and "csrf_ttl" or "F2"
    o[#o+1] = type(csrf.middleware({ secret = "s", max_age = 60 })) == "function" and "csrf_maxage" or "F3"
    o[#o+1] = type(auth.session_middleware({ name = "c" })) == "function" and "auth_name" or "F4"
    o[#o+1] = type(auth.session_middleware({ cookie_name = "c" })) == "function" and "auth_cookiename" or "F5"
    o[#o+1] = sec_ok(oauth.init, "secret") and "oauth_secret" or "F6"
    o[#o+1] = sec_ok(oauth.init, "state_secret") and "oauth_statesecret" or "F7"
    o[#o+1] = sec_ok(authflows.init, "secret") and "af_secret" or "F8"
    o[#o+1] = sec_ok(authflows.init, "state_secret") and "af_statesecret" or "F9"
    local ok_s, err_s = pcall(oauth.init, { secret = "short" })
    o[#o+1] = (not ok_s and tostring(err_s):find(">= 32", 1, true)) and "short_rejected" or "F10"
    ctx.stdout:write(table.concat(o, "|") .. "\n"); return 0
end)
LUA
cat > "$WD/n.js" <<JS
import { app } from "hull:app";
import { csrf } from "hull:web:middleware:csrf";
import { auth } from "hull:web:middleware:auth";
import { oauth } from "hull:web:middleware:oauth";
import { authFlows } from "hull:web:auth-flows";
app.manifest({ modules: [ $MODS_JS ] });
const S = "x".repeat(32);
function secOk(fn, key) {
    try { fn({ [key]: S }); return true; }
    catch (e) { return String(e).indexOf(">= 32") === -1; }
}
app.main((ctx) => {
    const o = [];
    const t = csrf.generate("sid", "sec");
    o.push(csrf.verify(t, "sid", "sec", 3600) ? "csrf_rt" : "F1");
    o.push(typeof csrf.middleware({ secret: "s", ttl: 60 }) === "function" ? "csrf_ttl" : "F2");
    o.push(typeof csrf.middleware({ secret: "s", maxAge: 60 }) === "function" ? "csrf_maxage" : "F3");
    o.push(typeof auth.sessionMiddleware({ name: "c" }) === "function" ? "auth_name" : "F4");
    o.push(typeof auth.sessionMiddleware({ cookieName: "c" }) === "function" ? "auth_cookiename" : "F5");
    o.push(secOk(oauth.init, "secret") ? "oauth_secret" : "F6");
    o.push(secOk(oauth.init, "stateSecret") ? "oauth_statesecret" : "F7");
    o.push(secOk(authFlows.init, "secret") ? "af_secret" : "F8");
    o.push(secOk(authFlows.init, "stateSecret") ? "af_statesecret" : "F9");
    let shortRejected = "F10";
    try { oauth.init({ secret: "short" }); } catch (e) { if (String(e).indexOf(">= 32") !== -1) shortRejected = "short_rejected"; }
    o.push(shortRejected);
    ctx.stdout.write(o.join("|") + "\n"); return 0;
});
JS

expect="csrf_rt|csrf_ttl|csrf_maxage|auth_name|auth_cookiename|oauth_secret|oauth_statesecret|af_secret|af_statesecret|short_rejected"
lua_out="$("$HULL" "$WD/n.lua" 2>/dev/null | tail -1)"
js_out="$( "$HULL" "$WD/n.js"  2>/dev/null | tail -1)"

if [ "$lua_out" = "$expect" ] && [ "$lua_out" = "$js_out" ]; then
    echo "PASS: canonical + alias option names both work across Lua and JS"
else
    echo "::error naming-alias DRIFT:"
    echo "  expect=$expect"
    echo "  lua   =$lua_out"
    echo "  js    =$js_out"
    exit 1
fi
