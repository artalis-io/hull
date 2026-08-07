#!/bin/sh
# e2e_hex_parity.sh: Lua/JS parity for the shared raw-byte hex helper.
#
# hull.crypto._hex is the ONE correct home for byte-string hex, precisely
# because crypto.hex_encode / crypto.hexEncode DIVERGE across runtimes: Lua
# strings are byte strings (raw hex), but a JS string is UTF-8-encoded at the C
# boundary, so crypto.hexEncode inflates any code unit >= 0x80 (0xFF -> "c3bf",
# not "ff"). Every HMAC key / token wire format in the auth stdlib depends on
# both runtimes hexing the same bytes to the same string. This asserts _hex is
# byte-identical across the full 0-255 range in both runtimes, so a future
# "simplification" back to crypto.hexEncode (or any drift) fails CI.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

cat > "$WD/hex.lua" <<'LUA'
local _hex = require("hull.crypto._hex")
app.manifest({ modules = { "hull/crypto@1" } })
app.main(function(ctx)
    local b = {}; for i = 0, 255 do b[#b+1] = string.char(i) end
    ctx.stdout:write(_hex.to_hex(table.concat(b)) .. "\n"); return 0
end)
LUA
cat > "$WD/hex.js" <<'JS'
import { app } from "hull:app"; import { _hex } from "hull:crypto:_hex";
app.manifest({ modules: ["hull/crypto@1"] });
app.main((ctx) => {
    let s = ""; for (let i = 0; i < 256; i++) s += String.fromCharCode(i);
    ctx.stdout.write(_hex.toHex(s) + "\n"); return 0;
});
JS

lua_hex="$("$HULL" "$WD/hex.lua" 2>/dev/null | tail -1)"
js_hex="$("$HULL" "$WD/hex.js"  2>/dev/null | tail -1)"

# 256 bytes -> 512 hex chars; guard against an empty (errored) render too.
if [ "${#lua_hex}" -eq 512 ] && [ "$lua_hex" = "$js_hex" ]; then
    echo "PASS: hull.crypto._hex is byte-identical across Lua and JS (full 0-255 range)"
else
    echo "::error _hex DRIFT (Lua != JS byte-string hex):"
    echo "  lua=$lua_hex"
    echo "  js =$js_hex"
    exit 1
fi
