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
# It also guards the `pattern` rule's shared input-size contract: pattern
# validation caps the input at 8192 UTF-8 BYTES, rejects over-cap values BEFORE
# matching, and tests the FULL value (never a truncated prefix). JS previously
# matched only value.substring(0, 8192) - an anchored allowlist rule (^...$)
# could be bypassed by appending a payload past 8192 chars, and it diverged from
# Lua (which matched the whole value). The pattern matrix below proves: the
# 8192-byte boundary accepts, 8193 rejects, a clean 8192-byte prefix + tail
# rejects (no truncated-prefix test), anchored and unanchored patterns both
# honor the cap, the cap is BYTE-based not char-based (multibyte UTF-8 boundary),
# and existing short-value behavior is unchanged - byte-identically across both
# runtimes.
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
local function chk(value, pat)
    return b(validate.check({ s = value }, { s = { pattern = pat } }))
end
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

    -- pattern input-size contract (8192 UTF-8 bytes, full-value match)
    out[#out + 1] = chk(string.rep("a", 8192), "^[a-z]+$")        -- P1 at limit, matches -> T
    out[#out + 1] = chk(string.rep("a", 8193), "^[a-z]+$")        -- P2 over limit -> F
    out[#out + 1] = chk(string.rep("a", 8192) .. "1", "^[a-z]+$") -- P3 clean prefix + tail -> F
    out[#out + 1] = chk(string.rep("a", 8193), "a")               -- P4 unanchored, over limit -> F
    out[#out + 1] = chk("cat", "a")                               -- P5 short unanchored match -> T
    out[#out + 1] = chk("hello", "^[a-z]+$")                      -- P6 short anchored match -> T
    out[#out + 1] = chk("Hello", "^[a-z]+$")                      -- P7 short anchored non-match -> F
    out[#out + 1] = chk(string.rep("a", 8190) .. "é", "^a")       -- P8 8192 bytes (multibyte) -> T
    out[#out + 1] = chk(string.rep("a", 8191) .. "é", "^a")       -- P9 8193 bytes (multibyte) -> F

    ctx.stdout:write(table.concat(out, "") .. "\n"); return 0
end)
LUA

cat > "$WD/v.js" <<'JS'
import { app } from "hull:app";
import { validate } from "hull:validate";
app.manifest({ modules: ["hull/validate@1"] });
const b = (v) => v ? "T" : "F";
const chk = (value, pat) => b(validate.check({ s: value }, { s: { pattern: pat } })[0]);
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

    // pattern input-size contract (8192 UTF-8 bytes, full-value match)
    out += chk("a".repeat(8192), "^[a-z]+$");        // P1 at limit, matches -> T
    out += chk("a".repeat(8193), "^[a-z]+$");        // P2 over limit -> F
    out += chk("a".repeat(8192) + "1", "^[a-z]+$");  // P3 clean prefix + tail -> F
    out += chk("a".repeat(8193), "a");               // P4 unanchored, over limit -> F
    out += chk("cat", "a");                          // P5 short unanchored match -> T
    out += chk("hello", "^[a-z]+$");                 // P6 short anchored match -> T
    out += chk("Hello", "^[a-z]+$");                 // P7 short anchored non-match -> F
    out += chk("a".repeat(8190) + "é", "^a");        // P8 8192 bytes (multibyte) -> T
    out += chk("a".repeat(8191) + "é", "^a");        // P9 8193 bytes (multibyte) -> F

    ctx.stdout.write(out + "\n"); return 0;
});
JS

lua_out="$("$HULL" "$WD/v.lua" 2>/dev/null | tail -1)"
js_out="$("$HULL" "$WD/v.js"  2>/dev/null | tail -1)"

# email:  valid / invalid / dbl-dot / leading-dot / dot-at / digit-TLD /
#         short-TLD / uppercase-valid / too-long ; then int-in-range / int-out /
#         oneof-hit / oneof-miss
# pattern: P1 at-limit-match / P2 over / P3 clean-prefix+tail / P4 unanchored-over
#          / P5 short-match / P6 short-anchored-match / P7 short-non-match /
#          P8 8192B-multibyte / P9 8193B-multibyte
expect="TFFFFFFTFTFTF""TFFFTTFTF"

if [ "$lua_out" = "$expect" ] && [ "$lua_out" = "$js_out" ]; then
    echo "PASS: hull.validate is byte-identical across Lua and JS"
else
    echo "::error hull.validate DRIFT:"
    echo "  expect=$expect"
    echo "  lua   =$lua_out"
    echo "  js    =$js_out"
    exit 1
fi
