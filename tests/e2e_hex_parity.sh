#!/bin/sh
# e2e_hex_parity.sh: Lua/JS parity for hull.encoding (hex, base64, base64url,
# base32).
#
# Every HMAC key, token and stored kv row in the stdlib depends on both
# runtimes encoding the same bytes to the same text. A C codec cannot promise
# that: a JS string is UTF-8-encoded at the C boundary, so "\xff" would hex
# as "c3bf" in JS but "ff" in Lua. hull.encoding is the one home for
# byte<->text codecs precisely so this cannot happen; this asserts it over
# the full 0-255 range in every codec, both directions, so any drift fails CI.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

cat > "$WD/enc.lua" <<'LUA'
local enc = require("hull.encoding")
app.manifest({ modules = { "hull/encoding@1" } })
app.main(function(ctx)
    local b = {}; for i = 0, 255 do b[#b+1] = string.char(i) end
    local all = table.concat(b)
    local out = { enc.hex.encode(all), enc.base64.encode(all),
                  enc.base64.encode(all, { url = true }), enc.base32.encode(all) }
    local back = enc.hex.decode(out[1]) == all and enc.base64.decode(out[2]) == all
        and enc.base64.decode(out[3], { url = true }) == all
        and enc.base32.decode(out[4]) == all
    out[#out + 1] = back and "roundtrip" or "F_roundtrip"
    ctx.stdout:write(table.concat(out, "|") .. "\n"); return 0
end)
LUA
cat > "$WD/enc.js" <<'JS'
import { app } from "hull:app"; import { encoding as enc } from "hull:encoding";
app.manifest({ modules: ["hull/encoding@1"] });
app.main((ctx) => {
    let all = ""; for (let i = 0; i < 256; i++) all += String.fromCharCode(i);
    const out = [enc.hex.encode(all), enc.base64.encode(all),
                 enc.base64.encode(all, { url: true }), enc.base32.encode(all)];
    const back = enc.hex.decode(out[0]) === all && enc.base64.decode(out[1]) === all
        && enc.base64.decode(out[2], { url: true }) === all
        && enc.base32.decode(out[3]) === all;
    out.push(back ? "roundtrip" : "F_roundtrip");
    ctx.stdout.write(out.join("|") + "\n"); return 0;
});
JS

lua_out="$("$HULL" "$WD/enc.lua" 2>/dev/null | tail -1)"
js_out="$( "$HULL" "$WD/enc.js"  2>/dev/null | tail -1)"

# 256 bytes -> 512 hex chars leads the line; guard against an empty (errored)
# render as well as a mismatch.
case "$lua_out" in
    *"|roundtrip") ok=1 ;;
    *) ok=0 ;;
esac
if [ "$ok" -eq 1 ] && [ "${lua_out%%|*}" != "" ] && [ "${#lua_out}" -gt 512 ] \
   && [ "$lua_out" = "$js_out" ]; then
    echo "PASS: hull.encoding is byte-identical across Lua and JS (hex, base64, base64url, base32; 0-255)"
else
    echo "::error hull.encoding DRIFT (Lua != JS):"
    echo "  lua=$lua_out"
    echo "  js =$js_out"
    exit 1
fi
