#!/bin/sh
# e2e_logx_parity.sh: hull.logx contextual-logging formatting, Lua/JS parity.
#
# logx.with(fields).info(msg) must append the SAME logfmt fragment in both
# runtimes: sorted keys, quoting of values with spaces/quotes/'=', boolean
# rendering, and child-logger field merging; the message is the leading
# msg="..." field (audit 11), and C1 controls / U+2028 / U+2029 are escaped
# as \uXXXX in both. Captures the log line (stderr),
# strips any ANSI, extracts the message onward, and asserts byte-identical
# output across runtimes.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

cat > "$WD/l.lua" <<'LUA'
local logx = require("hull.logx")
app.manifest({ modules = { "hull/logx@1", "hull/log@1" } })
app.main(function(ctx)
    logx.with({ b = "x y", a = 1, z = true }).info("AAAA")
    logx.with({ a = 1 }).with({ c = "d" }).warn("BBBB")
    -- Escaping: backslash (escaped, unquoted), a quote (escaped + quoted),
    -- an '=' (quoted). sorted keys bs, eq, qt.
    logx.with({ bs = "a\\b", qt = 'x"y', eq = "k=v" }).info("CCCC")
    -- The message cannot forge a field; NEL / U+2028 / U+2029 are escaped.
    logx.with({ u = "a\u{85}b\u{2028}c\u{2029}" }).info("DDDD user=admin \"x")
    return 0
end)
LUA
cat > "$WD/l.js" <<'JS'
import { app } from "hull:app"; import { logx } from "hull:logx";
app.manifest({ modules: ["hull/logx@1", "hull/log@1"] });
app.main((ctx) => {
    logx.with({ b: "x y", a: 1, z: true }).info("AAAA");
    logx.with({ a: 1 }).with({ c: "d" }).warn("BBBB");
    logx.with({ bs: "a\\b", qt: 'x"y', eq: "k=v" }).info("CCCC");
    logx.with({ u: "a\u0085b\u2028c\u2029" }).info("DDDD user=admin \"x");
    return 0;
});
JS

# Strip ANSI, keep only the marker-onward text of each marked line, join.
extract() {
    "$HULL" "$1" 2>&1 \
        | sed 's/\x1b\[[0-9;]*m//g' \
        | grep -oE 'msg="(AAAA|BBBB|CCCC|DDDD).*' \
        | sed 's/[[:space:]]*$//' \
        | tr '\n' '~'
}

lua_out="$(extract "$WD/l.lua")"
js_out="$(extract "$WD/l.js")"

# AAAA: sorted keys (a,b,z); "x y" quoted; boolean true.
# BBBB: child merge a+c.
# CCCC: backslash doubled + unquoted; quote escaped + quoted; '=' quoted.
# DDDD: the message quoted + escaped (its "user=admin" is not a field); the
#   value's U+0085 / U+2028 / U+2029 as \uXXXX, quoted.
expect='msg="AAAA" a=1 b="x y" z=true~msg="BBBB" a=1 c=d~msg="CCCC" bs=a\\b eq="k=v" qt="x\"y"~msg="DDDD user=admin \"x" u="a\u0085b\u2028c\u2029"~'

if [ "$lua_out" = "$expect" ] && [ "$lua_out" = "$js_out" ]; then
    echo "PASS: hull.logx logfmt formatting is byte-identical across Lua and JS"
else
    echo "::error hull.logx DRIFT:"
    echo "  expect=[$expect]"
    echo "  lua   =[$lua_out]"
    echo "  js    =[$js_out]"
    exit 1
fi
