#!/bin/sh
# e2e_uuid.sh: hull.uuid produces spec-valid v4/v7 UUIDs in BOTH runtimes.
#
# UUIDs are random so they can't be byte-compared cross-runtime; instead this
# asserts each runtime emits canonical RFC 9562 strings with the correct version
# nibble (4 / 7) and variant (8-b), that two v4s differ (randomness), and that
# v7's leading 48 bits are a plausible current-time-ms prefix (sortable).
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

HULL="${HULL:-./build/hull}"
[ -x "$HULL" ] || HULL="build/hull"
WD=$(mktemp -d)
trap 'rm -rf "$WD"' EXIT

cat > "$WD/u.lua" <<'LUA'
local uuid = require("hull.uuid")
app.manifest({ modules = { "hull/uuid@1" } })
app.main(function(ctx)
    ctx.stdout:write(uuid.v4() .. " " .. uuid.v7() .. " " .. uuid.v4() .. " " .. uuid.v7() .. "\n"); return 0
end)
LUA
cat > "$WD/u.js" <<'JS'
import { app } from "hull:app"; import { uuid } from "hull:uuid";
app.manifest({ modules: ["hull/uuid@1"] });
app.main((ctx) => { ctx.stdout.write(uuid.v4() + " " + uuid.v7() + " " + uuid.v4() + " " + uuid.v7() + "\n"); return 0; });
JS

V4RE='^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$'
V7RE='^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$'

fails=0
check_rt() { # runtime label, output line
    lbl="$1"; line="$2"
    set -- $line
    a="$1"; b="$2"; c="$3"; d="$4"
    echo "$a" | grep -Eq "$V4RE" || { echo "  FAIL: $lbl v4 malformed: $a"; fails=$((fails+1)); }
    echo "$c" | grep -Eq "$V4RE" || { echo "  FAIL: $lbl v4b malformed: $c"; fails=$((fails+1)); }
    echo "$b" | grep -Eq "$V7RE" || { echo "  FAIL: $lbl v7 malformed: $b"; fails=$((fails+1)); }
    echo "$d" | grep -Eq "$V7RE" || { echo "  FAIL: $lbl v7b malformed: $d"; fails=$((fails+1)); }
    [ "$a" != "$c" ] || { echo "  FAIL: $lbl two v4s identical (not random): $a"; fails=$((fails+1)); }
    # v7 is time-ordered: the 48-bit timestamp prefix (first 13 chars, incl. the
    # dash) is non-decreasing. Within one millisecond it ties (the random tail is
    # NOT ordered), across a millisecond it increases.
    pb=$(printf %s "$b" | cut -c1-13); pd=$(printf %s "$d" | cut -c1-13)
    [ "$pb" \< "$pd" ] || [ "$pb" = "$pd" ] || { echo "  FAIL: $lbl v7 ts prefix regressed: $pb > $pd"; fails=$((fails+1)); }
}

check_rt "lua" "$("$HULL" "$WD/u.lua" 2>/dev/null | tail -1)"
check_rt "js"  "$("$HULL" "$WD/u.js"  2>/dev/null | tail -1)"

if [ "$fails" -eq 0 ]; then
    echo "PASS: hull.uuid emits spec-valid v4/v7 in both runtimes"
else
    echo "::error hull.uuid: $fails failing check(s)"; exit 1
fi
