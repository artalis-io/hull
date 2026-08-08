#!/bin/sh
# e2e_token_interop.sh: cross-runtime wire-format interop for the shared token
# schemes (jwt, crypto.envelope, csrf) + jwt precondition-throw parity.
#
# These wire formats are dual-implemented (Lua + JS) and are explicitly meant to
# interoperate - e.g. a JS-Hull deployment must verify tokens a Lua-Hull minted
# from the same DB/secret, and vice-versa. Nothing tested that. This mints a
# token in one runtime and verifies it in the OTHER (both directions) for each
# scheme, so a signing/framing drift fails CI instead of silently rejecting
# every cross-runtime token.
#
# It also asserts the recently-fixed jwt precondition convention holds in BOTH
# runtimes: jwt.sign throws on a missing arg; jwt.verify throws on a missing key
# but returns a value for a bad token.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

# Fixed shared inputs (no shell/JS/Lua-string-special chars).
SECRET="hull-parity-secret-0123456789abcdef"
HEXSECRET="00112233445566778899aabbccddeeff"
SID="session-abc-123"

pass=0; fail=0
check() { # label got want
    if [ "$2" = "$3" ]; then pass=$((pass+1));
    else fail=$((fail+1)); echo "  FAIL: $1: got [$2] want [$3]"; fi
}
run() { "$HULL" "$1" 2>/dev/null | tail -1; }

# ── mint apps (quoted heredocs; secret hardcoded) ───────────────────────────
cat > "$WD/jwt_mint.lua" <<'LUA'
local jwt = require("hull.jwt")
app.manifest({ modules = { "hull/jwt@1" } })
app.main(function(ctx)
    ctx.stdout:write(jwt.sign({ sub = "u1" }, "hull-parity-secret-0123456789abcdef") .. "\n"); return 0
end)
LUA
cat > "$WD/jwt_mint.js" <<'JS'
import { app } from "hull:app"; import { jwt } from "hull:jwt";
app.manifest({ modules: ["hull/jwt@1"] });
app.main((ctx) => { ctx.stdout.write(jwt.sign({ sub: "u1" }, "hull-parity-secret-0123456789abcdef") + "\n"); return 0; });
JS
cat > "$WD/env_mint.lua" <<'LUA'
local env = require("hull.crypto.envelope")
app.manifest({ modules = { "hull/crypto/envelope@1" } })
app.main(function(ctx)
    ctx.stdout:write(env.sign({ sub = "u1" }, "00112233445566778899aabbccddeeff") .. "\n"); return 0
end)
LUA
cat > "$WD/env_mint.js" <<'JS'
import { app } from "hull:app"; import { envelope } from "hull:crypto:envelope";
app.manifest({ modules: ["hull/crypto/envelope@1"] });
app.main((ctx) => { ctx.stdout.write(envelope.sign({ sub: "u1" }, "00112233445566778899aabbccddeeff") + "\n"); return 0; });
JS
cat > "$WD/csrf_mint.lua" <<'LUA'
local csrf = require("hull.web.middleware.csrf")
app.manifest({ modules = { "hull/web/middleware/csrf@1", "hull/web/cookie@1" } })
app.main(function(ctx)
    ctx.stdout:write(csrf.generate("session-abc-123", "hull-parity-secret-0123456789abcdef") .. "\n"); return 0
end)
LUA
cat > "$WD/csrf_mint.js" <<'JS'
import { app } from "hull:app"; import { csrf } from "hull:web:middleware:csrf";
app.manifest({ modules: ["hull/web/middleware/csrf@1", "hull/web/cookie@1"] });
app.main((ctx) => { ctx.stdout.write(csrf.generate("session-abc-123", "hull-parity-secret-0123456789abcdef") + "\n"); return 0; });
JS

# ── verify apps take the minted token via unquoted-heredoc interpolation ─────
verify_jwt_lua() { cat > "$WD/vjwt.lua" <<LUA
local jwt = require("hull.jwt")
app.manifest({ modules = { "hull/jwt@1" } })
app.main(function(ctx)
    local p, err = jwt.verify("$1", "$SECRET", { algs = { "HS256" } })
    ctx.stdout:write((p and ("OK:" .. p.sub) or ("FAIL:" .. tostring(err))) .. "\n"); return 0
end)
LUA
run "$WD/vjwt.lua"; }
verify_jwt_js() { cat > "$WD/vjwt.js" <<JS
import { app } from "hull:app"; import { jwt } from "hull:jwt";
app.manifest({ modules: ["hull/jwt@1"] });
app.main((ctx) => {
    const r = jwt.verify("$1", "$SECRET", { algs: ["HS256"] });
    ctx.stdout.write((r[0] ? "OK:" + r[0].sub : "FAIL:" + r[1]) + "\n"); return 0;
});
JS
run "$WD/vjwt.js"; }
verify_env_lua() { cat > "$WD/venv.lua" <<LUA
local env = require("hull.crypto.envelope")
app.manifest({ modules = { "hull/crypto/envelope@1" } })
app.main(function(ctx)
    local p, err = env.verify("$1", "$HEXSECRET")
    ctx.stdout:write((p and ("OK:" .. p.sub) or ("FAIL:" .. tostring(err))) .. "\n"); return 0
end)
LUA
run "$WD/venv.lua"; }
verify_env_js() { cat > "$WD/venv.js" <<JS
import { app } from "hull:app"; import { envelope } from "hull:crypto:envelope";
app.manifest({ modules: ["hull/crypto/envelope@1"] });
app.main((ctx) => {
    const r = envelope.verify("$1", "$HEXSECRET");
    ctx.stdout.write((r[0] ? "OK:" + r[0].sub : "FAIL:" + r[1]) + "\n"); return 0;
});
JS
run "$WD/venv.js"; }
verify_csrf_lua() { cat > "$WD/vcsrf.lua" <<LUA
local csrf = require("hull.web.middleware.csrf")
app.manifest({ modules = { "hull/web/middleware/csrf@1", "hull/web/cookie@1" } })
app.main(function(ctx)
    ctx.stdout:write((csrf.verify("$1", "$SID", "$SECRET", 3600) and "OK" or "FAIL") .. "\n"); return 0
end)
LUA
run "$WD/vcsrf.lua"; }
verify_csrf_js() { cat > "$WD/vcsrf.js" <<JS
import { app } from "hull:app"; import { csrf } from "hull:web:middleware:csrf";
app.manifest({ modules: ["hull/web/middleware/csrf@1", "hull/web/cookie@1"] });
app.main((ctx) => { ctx.stdout.write((csrf.verify("$1", "$SID", "$SECRET", 3600) ? "OK" : "FAIL") + "\n"); return 0; });
JS
run "$WD/vcsrf.js"; }

# ── interop matrix ──────────────────────────────────────────────────────────
check "jwt lua->js"      "$(verify_jwt_js  "$(run "$WD/jwt_mint.lua")")"  "OK:u1"
check "jwt js->lua"      "$(verify_jwt_lua "$(run "$WD/jwt_mint.js")")"   "OK:u1"
check "envelope lua->js" "$(verify_env_js  "$(run "$WD/env_mint.lua")")"  "OK:u1"
check "envelope js->lua" "$(verify_env_lua "$(run "$WD/env_mint.js")")"   "OK:u1"
check "csrf lua->js"     "$(verify_csrf_js  "$(run "$WD/csrf_mint.lua")")" "OK"
check "csrf js->lua"     "$(verify_csrf_lua "$(run "$WD/csrf_mint.js")")"  "OK"

# ── jwt precondition-throw parity ───────────────────────────────────────────
cat > "$WD/throw.lua" <<'LUA'
local jwt = require("hull.jwt")
app.manifest({ modules = { "hull/jwt@1" } })
app.main(function(ctx)
    local s = pcall(jwt.sign, { sub = "x" }, nil) and "N" or "T"        -- sign missing secret -> throw
    local v = pcall(jwt.verify, "a.b.c", nil) and "N" or "T"            -- verify missing key -> throw
    -- bad token WITH a key must NOT throw (returns nil): pcall ok=true
    local btok_ok = pcall(jwt.verify, "not-a-jwt", "k", { algs = { "HS256" } })
    ctx.stdout:write(s .. v .. (btok_ok and "R" or "X") .. "\n"); return 0
end)
LUA
cat > "$WD/throw.js" <<'JS'
import { app } from "hull:app"; import { jwt } from "hull:jwt";
app.manifest({ modules: ["hull/jwt@1"] });
function threw(fn) { try { fn(); return "N"; } catch (e) { return "T"; } }
app.main((ctx) => {
    const s = threw(() => jwt.sign({ sub: "x" }, null));
    const v = threw(() => jwt.verify("a.b.c", null));
    let btokOk = true; try { jwt.verify("not-a-jwt", "k", { algs: ["HS256"] }); } catch (e) { btokOk = false; }
    ctx.stdout.write(s + v + (btokOk ? "R" : "X") + "\n"); return 0;
});
JS
# T (sign throws) T (verify-missing-key throws) R (bad-token returns, no throw)
check "jwt throw parity lua" "$(run "$WD/throw.lua")" "TTR"
check "jwt throw parity js"  "$(run "$WD/throw.js")"  "TTR"

echo "----"
if [ "$fail" -eq 0 ]; then
    echo "PASS: token wire formats interoperate across Lua<->JS ($pass checks)"
else
    echo "::error token interop: $fail failing check(s)"; exit 1
fi
