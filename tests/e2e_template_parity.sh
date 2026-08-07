#!/bin/sh
# e2e_template_parity.sh: Lua/JS golden-parity for the (security-relevant)
# escaping + template-filter behavior.
#
# Hull's stdlib is dual-runtime: template.lua/template.js and the htmx widgets
# are hand-ported, so a hardening or a fix can land in one runtime and silently
# miss the other. That already happened (JS escaped backtick and the json filter's
# "<", Lua did not). This test renders an IDENTICAL battery of cases through the
# Lua and the JS runtime and asserts the two outputs are byte-for-byte identical,
# so any future escaping/filter drift fails CI instead of shipping.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"

WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

# The two apps below MUST emit the same KEY=value lines for the same inputs.
# Cases target the exact drift class: HTML escaping (incl. backtick), the json
# filter's "<" escape, length on object/array/string, and for-loop coercion of a
# non-table source. Plus the shared htmx.escape and a widget that uses it, so the
# 16-copies-collapsed-to-one escape stays identical across runtimes.

cat > "$WD/parity.lua" <<'LUA'
local t     = require("hull.template")
local htmx  = require("hull.web.htmx")
local confirm = require("hull.web.htmx.confirm")
app.manifest({ modules = {
    "hull/template@1", "hull/web/htmx@1", "hull/web/htmx/confirm@1",
} })
app.main(function(ctx)
    local function w(k, v) ctx.stdout:write(k .. "=" .. v .. "\n") end
    w("esc_all",  t.render_string("{{ x }}", { x = "&<>\"'`z" }))
    w("json_lt",  t.render_string("{{{ d | json }}}", { d = { a = "</script>" } }))
    w("len_obj",  t.render_string("{{ o | length }}", { o = { a = 1, b = 2, c = 3 } }))
    w("len_arr",  t.render_string("{{ a | length }}", { a = { 10, 20 } }))
    w("len_str",  t.render_string("{{ s | length }}", { s = "hello" }))
    w("for_str",  "[" .. t.render_string("{% for i in s %}{{ i }}{% end %}", { s = "nope" }) .. "]")
    w("forkv_str","[" .. t.render_string("{% for k, v in s %}x{% end %}", { s = "nope" }) .. "]")
    w("default",  t.render_string("{{ m | default: \"fb\" }}", {}))
    w("upper",    t.render_string("{{ p | upper }}", { p = "aB" }))
    w("trim",     t.render_string("{{ p | trim }}", { p = "  hi  " }))
    w("htmx_esc", htmx.escape("a\"<b>&'`"))
    w("htmx_nil", "[" .. htmx.escape(nil) .. "]")
    w("widget",   confirm.attrs("a\"<b`"))
    return 0
end)
LUA

cat > "$WD/parity.js" <<'JS'
import { app } from "hull:app";
import { template as t } from "hull:template";
import { htmx } from "hull:web:htmx";
import { confirm } from "hull:web:htmx:confirm";
app.manifest({ modules: [
    "hull/template@1", "hull/web/htmx@1", "hull/web/htmx/confirm@1",
] });
app.main((ctx) => {
    const w = (k, v) => ctx.stdout.write(k + "=" + v + "\n");
    w("esc_all",  t.renderString("{{ x }}", { x: "&<>\"'`z" }));
    w("json_lt",  t.renderString("{{{ d | json }}}", { d: { a: "</script>" } }));
    w("len_obj",  t.renderString("{{ o | length }}", { o: { a: 1, b: 2, c: 3 } }));
    w("len_arr",  t.renderString("{{ a | length }}", { a: [10, 20] }));
    w("len_str",  t.renderString("{{ s | length }}", { s: "hello" }));
    w("for_str",  "[" + t.renderString("{% for i in s %}{{ i }}{% end %}", { s: "nope" }) + "]");
    w("forkv_str","[" + t.renderString("{% for k, v in s %}x{% end %}", { s: "nope" }) + "]");
    w("default",  t.renderString('{{ m | default: "fb" }}', {}));
    w("upper",    t.renderString("{{ p | upper }}", { p: "aB" }));
    w("trim",     t.renderString("{{ p | trim }}", { p: "  hi  " }));
    w("htmx_esc", htmx.escape("a\"<b>&'`"));
    w("htmx_nil", "[" + htmx.escape(null) + "]");
    w("widget",   confirm.attrs("a\"<b`"));
    return 0;
});
JS

echo "=== rendering the parity battery through both runtimes ==="
"$HULL" "$WD/parity.lua" 2>/dev/null | grep '=' | sort > "$WD/lua.out"
"$HULL" "$WD/parity.js"  2>/dev/null | grep '=' | sort > "$WD/js.out"

lua_n=$(grep -c '=' "$WD/lua.out" || true)
js_n=$(grep -c '=' "$WD/js.out"  || true)
echo "  lua emitted $lua_n cases, js emitted $js_n cases"

if [ "$lua_n" -lt 13 ] || [ "$js_n" -lt 13 ]; then
    echo "::error one runtime produced too few cases (lua=$lua_n js=$js_n) - a render errored"
    echo "--- lua ---"; cat "$WD/lua.out"
    echo "--- js ---";  cat "$WD/js.out"
    exit 1
fi

if diff -u "$WD/lua.out" "$WD/js.out" > "$WD/diff.txt" 2>&1; then
    echo "PASS: Lua and JS escaping/template output is byte-identical ($lua_n cases)"
else
    echo "::error Lua/JS template parity DRIFT - escaping/filter output differs:"
    cat "$WD/diff.txt"
    exit 1
fi
