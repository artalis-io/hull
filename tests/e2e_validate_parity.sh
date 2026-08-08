#!/bin/sh
# e2e_validate_parity.sh: Lua/JS parity for hull.validate.
#
# The email validator had DRIFTED: the JS side was a bare
# /^[^\s@]+@[^\s@]+\.[^\s@]+$/ while Lua enforced length <= 254, rejected "..",
# leading/edge dots, and required a >=2-LETTER TLD. Same address validated
# differently across runtimes - a real trust/anti-abuse gap. This runs an
# identical case matrix (email edge cases + a few type/min/oneof cases) through
# BOTH runtimes and asserts the pass/fail vector is exact and identical, so the
# validators can't silently diverge again.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

cat > "$WD/v.lua" <<'LUA'
local validate = require("hull.validate")
app.manifest({ modules = { "hull/validate@1" } })
local function b(v) return v and "T" or "F" end
app.main(function(ctx)
    local emails = {
        "a@b.co", "bad", "a..b@x.co", ".x@y.co", "x@.y.co",
        "user@host.123", "foo@bar.c", "A@B.CO",
        string.rep("a", 250) .. "@x.co",
    }
    local out = {}
    for _, e in ipairs(emails) do
        out[#out + 1] = b(validate.check({ e = e }, { e = { required = true, email = true } }))
    end
    out[#out + 1] = b(validate.check({ n = 5 },  { n = { type = "integer", min = 1, max = 10 } }))
    out[#out + 1] = b(validate.check({ n = 20 }, { n = { type = "integer", min = 1, max = 10 } }))
    out[#out + 1] = b(validate.check({ s = "hi" }, { s = { oneof = { "hi", "yo" } } }))
    out[#out + 1] = b(validate.check({ s = "no" }, { s = { oneof = { "hi", "yo" } } }))
    ctx.stdout:write(table.concat(out, "") .. "\n"); return 0
end)
LUA

cat > "$WD/v.js" <<'JS'
import { app } from "hull:app";
import { validate } from "hull:validate";
app.manifest({ modules: ["hull/validate@1"] });
const b = (v) => v ? "T" : "F";
app.main((ctx) => {
    const emails = [
        "a@b.co", "bad", "a..b@x.co", ".x@y.co", "x@.y.co",
        "user@host.123", "foo@bar.c", "A@B.CO",
        "a".repeat(250) + "@x.co",
    ];
    let out = "";
    for (const e of emails) {
        out += b(validate.check({ e }, { e: { required: true, email: true } })[0]);
    }
    out += b(validate.check({ n: 5 },  { n: { type: "integer", min: 1, max: 10 } })[0]);
    out += b(validate.check({ n: 20 }, { n: { type: "integer", min: 1, max: 10 } })[0]);
    out += b(validate.check({ s: "hi" }, { s: { oneof: ["hi", "yo"] } })[0]);
    out += b(validate.check({ s: "no" }, { s: { oneof: ["hi", "yo"] } })[0]);
    ctx.stdout.write(out + "\n"); return 0;
});
JS

lua_out="$("$HULL" "$WD/v.lua" 2>/dev/null | tail -1)"
js_out="$("$HULL" "$WD/v.js"  2>/dev/null | tail -1)"

# valid / invalid / dbl-dot / leading-dot / dot-at / digit-TLD / short-TLD /
# uppercase-valid / too-long ; then int-in-range / int-out / oneof-hit / oneof-miss
expect="TFFFFFFTFTFTF"

if [ "$lua_out" = "$expect" ] && [ "$lua_out" = "$js_out" ]; then
    echo "PASS: hull.validate is byte-identical across Lua and JS"
else
    echo "::error hull.validate DRIFT:"
    echo "  expect=$expect"
    echo "  lua   =$lua_out"
    echo "  js    =$js_out"
    exit 1
fi
