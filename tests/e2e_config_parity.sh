#!/bin/sh
# e2e_config_parity.sh: Lua/JS parity for hull.config (typed env config + .env).
#
# Phase 1 (env-only): typed coercion, defaults, required-throws, and a coercion
# failure through both runtimes with an identical environment; asserts the
# result vector is byte-identical.
#
# Phase 2 (.env): config.load_dotenv must clear BOTH gates - fs.read for the
# file AND the manifest env allowlist per key. Asserts: an allowlisted key
# present only in .env is served; a key in .env but NOT in manifest.env is
# ignored; the real process env overrides a .env value; the applied-count
# reflects only allowlisted keys. Identical across runtimes.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
case "$HULL" in /*) : ;; *) HULL="$(pwd)/$HULL" ;; esac
[ -x "$HULL" ] || HULL="$(pwd)/build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

# ── Phase 1: env-only ───────────────────────────────────────────────────────
cat > "$WD/c.lua" <<'LUA'
local config = require("hull.config")
app.manifest({ modules = { "hull/config@1" }, env = { "T_PORT", "T_DEBUG", "T_MISSING" } })
app.main(function(ctx)
    local out = {}
    out[#out + 1] = tostring(config.get("T_PORT", { type = "integer", default = 1 }))
    out[#out + 1] = tostring(config.get("T_DEBUG", { type = "boolean", default = false }))
    out[#out + 1] = tostring(config.get("T_MISSING", { default = 42 }))
    out[#out + 1] = (pcall(config.require, "T_MISSING")) and "N" or "T"
    out[#out + 1] = (pcall(config.get, "T_PORT", { type = "boolean" })) and "N" or "T"
    ctx.stdout:write(table.concat(out, "|") .. "\n"); return 0
end)
LUA
cat > "$WD/c.js" <<'JS'
import { app } from "hull:app"; import { config } from "hull:config";
app.manifest({ modules: ["hull/config@1"], env: ["T_PORT", "T_DEBUG", "T_MISSING"] });
function threw(fn) { try { fn(); return "N"; } catch (e) { return "T"; } }
app.main((ctx) => {
    const out = [];
    out.push(String(config.get("T_PORT", { type: "integer", default: 1 })));
    out.push(String(config.get("T_DEBUG", { type: "boolean", default: false })));
    out.push(String(config.get("T_MISSING", { default: 42 })));
    out.push(threw(() => config.require("T_MISSING")));
    out.push(threw(() => config.get("T_PORT", { type: "boolean" })));
    ctx.stdout.write(out.join("|") + "\n"); return 0;
});
JS

lua1="$(T_PORT=8080 T_DEBUG=yes "$HULL" "$WD/c.lua" 2>/dev/null | tail -1)"
js1="$( T_PORT=8080 T_DEBUG=yes "$HULL" "$WD/c.js"  2>/dev/null | tail -1)"
expect1="8080|true|42|T|T"

# ── Phase 2: .env (two gates) ───────────────────────────────────────────────
# T_DSN + T_PORT are allowlisted; T_SECRET + NOT_ALLOWED are NOT.
cat > "$WD/.env" <<'ENV'
# a comment
T_DSN=postgres://localhost/db
export T_SECRET="shhh"
NOT_ALLOWED=leak
T_PORT=1111
ENV
cat > "$WD/d.lua" <<'LUA'
local config = require("hull.config")
app.manifest({ modules = { "hull/config@1" },
               env = { "T_DSN", "T_PORT" },
               fs = { read = { ".env" } } })
app.main(function(ctx)
    local n = config.load_dotenv()
    local out = {
        tostring(n),
        config.get("T_DSN") or "nil",            -- allowlisted, only in .env
        config.get("T_SECRET") or "nil",         -- in .env, NOT allowlisted -> nil
        tostring(config.get("T_PORT", { type = "integer" })),  -- real env wins
    }
    ctx.stdout:write(table.concat(out, "|") .. "\n"); return 0
end)
LUA
cat > "$WD/d.js" <<'JS'
import { app } from "hull:app"; import { config } from "hull:config";
app.manifest({ modules: ["hull/config@1"],
               env: ["T_DSN", "T_PORT"],
               fs: { read: [".env"] } });
app.main((ctx) => {
    const n = config.loadDotenv();
    const out = [
        String(n),
        config.get("T_DSN") || "nil",
        config.get("T_SECRET") || "nil",
        String(config.get("T_PORT", { type: "integer" })),
    ];
    ctx.stdout.write(out.join("|") + "\n"); return 0;
});
JS

# 2 allowlisted keys applied (T_DSN, T_PORT); T_DSN from .env; T_SECRET ignored
# (not allowlisted) -> nil; T_PORT real env 9090 overrides .env 1111.
expect2="2|postgres://localhost/db|nil|9090"
lua2="$(cd "$WD" && T_PORT=9090 "$HULL" ./d.lua 2>/dev/null | tail -1)"
js2="$( cd "$WD" && T_PORT=9090 "$HULL" ./d.js  2>/dev/null | tail -1)"

ok=1
[ "$lua1" = "$expect1" ] && [ "$lua1" = "$js1" ] || { ok=0; echo "::error phase1: expect=$expect1 lua=$lua1 js=$js1"; }
[ "$lua2" = "$expect2" ] && [ "$lua2" = "$js2" ] || { ok=0; echo "::error phase2(.env): expect=$expect2 lua=$lua2 js=$js2"; }

if [ "$ok" -eq 1 ]; then
    echo "PASS: hull.config (typed env + two-gate .env) is byte-identical across Lua and JS"
else
    exit 1
fi
