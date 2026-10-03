#!/bin/sh
# e2e_manifest_isolation.sh: reading a Lua app's manifest cannot reach the
# build-tool API.
#
# The manifest is a call, so `hull build` / `hull manifest` / `hull modules`
# run the app's top level. They used to run it inside the tool VM, with the
# `tool` global removed for the window and put back after - and a metatable
# the app set on _G saw `tool` come back and kept it (tool.write_file,
# tool.spawn). The app now runs in a Lua runtime of its own. This app tries
# both ways in; neither may leave its marker file, and its manifest must still
# come back.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
case "$HULL" in /*) ;; *) HULL="$(pwd)/$HULL" ;; esac
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
mkdir -p "$TMPDIR/app"

cat > "$TMPDIR/app/app.lua" <<'EOF'
setmetatable(_G, { __newindex = function(t, k, v)
    rawset(t, k, v)
    if k == "tool" and type(v) == "table" and v.write_file then
        v.write_file("PWNED", "the tool API came back to the app")
    end
end })
if rawget(_G, "tool") then tool.write_file("PWNED", "tool was visible at top level") end
app.manifest({ modules = { "hull/json@1" } })
app.main(function() return 0 end)
EOF

echo "=== manifest extraction isolation ==="
for cmd in "manifest" "modules list"; do
    # shellcheck disable=SC2086
    out=$(cd "$TMPDIR" && "$HULL" $cmd app 2>&1 || true)
    if [ -e "$TMPDIR/PWNED" ] || [ -e "$TMPDIR/app/PWNED" ]; then
        fail "hull $cmd: the app could not reach the tool API" "marker written"
        rm -f "$TMPDIR/PWNED" "$TMPDIR/app/PWNED"
    else
        pass "hull $cmd: the app could not reach the tool API"
    fi
    case "$out" in
        *hull/json*) pass "hull $cmd: the manifest still comes back" ;;
        *) fail "hull $cmd: the manifest still comes back" "$out" ;;
    esac
done

echo ""
echo "e2e_manifest_isolation: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
