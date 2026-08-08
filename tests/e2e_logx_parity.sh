#!/bin/sh
# e2e_logx_parity.sh: hull.logx contextual-logging formatting, Lua/JS parity.
#
# logx.with(fields).info(msg) must append the SAME logfmt fragment in both
# runtimes: sorted keys, quoting of values with spaces/quotes/'=', boolean
# rendering, and child-logger field merging. Captures the log line (stderr),
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
    return 0;
});
JS

# Strip ANSI, keep only the marker-onward text of each marked line, join.
extract() {
    "$HULL" "$1" 2>&1 \
        | sed 's/\x1b\[[0-9;]*m//g' \
        | grep -oE '(AAAA|BBBB|CCCC).*' \
        | sed 's/[[:space:]]*$//' \
        | tr '\n' '~'
}

lua_out="$(extract "$WD/l.lua")"
js_out="$(extract "$WD/l.js")"

# AAAA: sorted keys (a,b,z); "x y" quoted; boolean true.
# BBBB: child merge a+c.
# CCCC: backslash doubled + unquoted; quote escaped + quoted; '=' quoted.
expect='AAAA a=1 b="x y" z=true~BBBB a=1 c=d~CCCC bs=a\\b eq="k=v" qt="x\"y"~'

if [ "$lua_out" = "$expect" ] && [ "$lua_out" = "$js_out" ]; then
    echo "PASS: hull.logx logfmt formatting is byte-identical across Lua and JS"
else
    echo "::error hull.logx DRIFT:"
    echo "  expect=[$expect]"
    echo "  lua   =[$lua_out]"
    echo "  js    =[$js_out]"
    exit 1
fi
