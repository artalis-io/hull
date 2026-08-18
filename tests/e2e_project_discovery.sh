#!/bin/sh
# E2E tests — `hull agent inspect` (standalone project source discovery)
#
# Usage: sh tests/e2e_project_discovery.sh
# Requires: build/hull already built; python3 (for JSON assertions, as the oauth e2e uses).
#
# Exercises the REAL tool VM end-to-end (not the unit harness's stubbed fs): the canonical
# analyzer produces deterministic output; Lua annotations are discovered WITHOUT executing
# app source; unknown annotations survive; malformed Lua -> valid=false + diagnostics;
# frontend capabilities are accurate; static/ browser assets are pruned while application
# .js is honestly unsupported (never parsed as Lua) -> complete=false; a missing root is
# an invalid+incomplete discovery; and the public JSON leaks NO generation-internal state
# (handle / _by_source / _handles / by_id).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
PASS=0
FAIL=0

if [ ! -x "$HULL" ]; then
    echo "e2e-project-discovery: hull binary not found at $HULL — run 'make' first"
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "e2e-project-discovery: python3 not found — skipping (JSON assertions need it)"
    exit 0
fi

fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }

TMP=$(mktemp -d)
trap 'chmod -R u+rwx "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT

# assert_py "<name>" "<json>" "<python expr over d>": pass iff the expr is truthy.
assert_py() {
    _name="$1"; _json="$2"; _expr="$3"
    if printf '%s' "$_json" | python3 -c '
import json,sys
d=json.load(sys.stdin)
sys.exit(0 if ('"$_expr"') else 1)
' 2>/dev/null; then pass "$_name"; else fail "$_name"; fi
}

echo ""
echo "=== E2E: hull agent inspect (project source discovery) ==="

# ── a mixed modular app: annotated Lua, nested source, browser asset, app JS ──
APP="$TMP/app"
mkdir -p "$APP/routes" "$APP/static"
cat > "$APP/app.lua" <<'EOF'
---@route GET /
---@custom freeform tag
local function home() end
local a, b = 1, 2
return home, a, b
EOF
cat > "$APP/routes/users.lua" <<'EOF'
---@resource users
local function list() end
return list
EOF
printf 'window.x = 1;\n' > "$APP/static/vendor.js"   # browser asset -> pruned
printf 'export const q = 1;\n' > "$APP/client.js"    # application JS -> unsupported

OUT=$("$HULL" agent inspect "$APP" 2>/dev/null); RC=$?
[ "$RC" = "0" ] && pass "exit 0 (a discovery was produced)" || fail "exit code ($RC)"

assert_py "schema_version + standalone provenance" "$OUT" 'd["schema_version"]==1 and d["source"]=="standalone" and d["generation"]==0'
assert_py "valid (no malformed source)" "$OUT" 'd["valid"] is True'
assert_py "complete=false (an application .js is unsupported)" "$OUT" 'd["complete"] is False'
assert_py "static/*.js browser asset is pruned (not a source)" "$OUT" \
    'not any(s["path"]=="static/vendor.js" for s in d["sources"])'
assert_py "application client.js honestly unsupported" "$OUT" \
    'any(s["path"]=="client.js" and s["status"]=="unsupported" and s["language"]=="javascript" for s in d["sources"])'
assert_py "app.lua + routes/users.lua analyzed" "$OUT" \
    'sum(1 for s in d["sources"] if s["status"]=="analyzed")==2'
assert_py "annotations discovered WITHOUT executing source" "$OUT" \
    'set(d["indexes"]["by_annotation"].keys())=={"route","custom","resource"}'
assert_py "UNKNOWN @custom annotation survives" "$OUT" \
    'any(a["name"]=="custom" for x in d["declarations"] for a in x["annotations"])'
assert_py "annotated-only public declarations (home + list)" "$OUT" \
    'sorted(x["name"] for x in d["declarations"])==["home","list"]'
assert_py "totals retain all 4 declarations; 2 annotated" "$OUT" \
    'd["summary"]["declarations_total"]==4 and d["summary"]["declarations_annotated"]==2'
assert_py "JS frontend honest: analyzable=false, 0 capabilities" "$OUT" \
    'any(f["language"]=="javascript" and f["analyzable"] is False and len(f["capabilities"])==0 for f in d["frontends"])'
assert_py "Lua frontend reports 4 shipped capabilities" "$OUT" \
    'any(f["language"]=="lua" and f["analyzable"] is True and len(f["capabilities"])==4 for f in d["frontends"])'
assert_py "unsupported-frontend diagnostic present (warning)" "$OUT" \
    'any(x["code"]=="project.frontend.unsupported" and x["severity"]=="warning" for x in d["diagnostics"])'

# multi-name group: both @route/@custom annotations target the SAME declaration group
assert_py "annotation target_group_id points at the declaration group" "$OUT" \
    'all(a["target_group_id"]==x["group_id"] for x in d["declarations"] for a in x["annotations"])'

# NO generation-internal state on the wire
assert_py "no handle / _by_source / _handles / by_id leaked" "$OUT" \
    '"handle" not in json.dumps(d) and "_by_source" not in json.dumps(d) and "_handles" not in json.dumps(d) and "by_id" not in d["indexes"]'

# determinism
OUT2=$("$HULL" agent inspect "$APP" 2>/dev/null)
[ "$OUT" = "$OUT2" ] && pass "deterministic (two runs byte-identical)" || fail "non-deterministic output"

# ── malformed Lua -> valid=false, structured diagnostics, status error ──
BAD="$TMP/bad"
mkdir -p "$BAD"
printf 'local function ok() end\nreturn ok\n' > "$BAD/good.lua"
printf 'local = = )\n' > "$BAD/broken.lua"
OUTB=$("$HULL" agent inspect "$BAD" 2>/dev/null)
assert_py "malformed Lua -> valid=false" "$OUTB" 'd["valid"] is False'
assert_py "malformed source status=error + error diagnostic" "$OUTB" \
    'any(s["path"]=="broken.lua" and s["status"]=="error" for s in d["sources"]) and any(x["severity"]=="error" for x in d["diagnostics"])'
assert_py "the clean sibling still analyzed" "$OUTB" \
    'any(s["path"]=="good.lua" and s["status"]=="analyzed" for s in d["sources"])'

# ── a genuinely clean all-Lua project -> valid + complete ──
OK="$TMP/okproj"
mkdir -p "$OK/lib"
cat > "$OK/app.lua" <<'EOF'
---@main
local function main() end
return main
EOF
cat > "$OK/lib/util.lua" <<'EOF'
---@util
local function u() end
return u
EOF
OUTC=$("$HULL" agent inspect "$OK" 2>/dev/null)
assert_py "clean all-Lua project -> valid AND complete" "$OUTC" 'd["valid"] is True and d["complete"] is True'
assert_py "both annotated Lua declarations public" "$OUTC" 'sorted(x["name"] for x in d["declarations"])==["main","u"]'

# ── missing root -> invalid + incomplete, project.discovery_failed, still exit 0 ──
OUTM=$("$HULL" agent inspect "$TMP/does-not-exist" 2>/dev/null); RCM=$?
assert_py "missing root -> invalid + incomplete + no sources" "$OUTM" \
    'd["valid"] is False and d["complete"] is False and len(d["sources"])==0'
assert_py "missing root emits project.discovery_failed" "$OUTM" \
    'any(x["code"]=="project.discovery_failed" for x in d["diagnostics"])'
[ "$RCM" = "0" ] && pass "missing root still exits 0 (validity is in the JSON)" || fail "missing-root exit ($RCM)"

echo ""
echo "=== hull agent inspect E2E: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
