#!/bin/sh
# E2E tests — `hull analyze` (static Lua source syntax analysis)
#
# Usage: sh tests/e2e_analyze.sh
# Requires: build/hull already built
#
# Fixtures are built in a TMPDIR (self-contained; keeps intentionally-broken .lua out
# of the repo / the conformance corpus). Covers: deterministic ordering, syntax error
# in a NON-entry module, explicit-file target errors, directory exclusions, JSON
# purity + schema, --quiet, and all three exit codes.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
PASS=0
FAIL=0

if [ ! -x "$HULL" ]; then
    echo "e2e-analyze: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo ""
echo "=== E2E: hull analyze ==="

# ── a clean modular app ───────────────────────────────────────────────
APP="$TMP/clean"
mkdir -p "$APP/routes" "$APP/lib"
printf 'app.manifest({ modules = {} })\napp.main(function() return 0 end)\n' > "$APP/app.lua"
printf 'local M = {}\nfunction M.register(app) app.get("/u", function(req, res) res:json({ ok = true }) end) end\nreturn M\n' > "$APP/routes/users.lua"
printf 'local M = {}\nfunction M.add(a, b) return a + b end\nreturn M\n' > "$APP/lib/util.lua"

# ── a broken app: a syntax error in a NON-entry module ────────────────
BROKEN="$TMP/broken"
cp -r "$APP" "$BROKEN"
printf 'local M = {}\nfunction M.oops() return { ok = ) end\nreturn M\n' > "$BROKEN/routes/bad.lua"

# ── Test 1: clean app → exit 0, "no issues", stdout only ──────────────
rc=0; out=$("$HULL" analyze "$APP" 2>/dev/null) || rc=$?
if [ "$rc" = "0" ] && echo "$out" | grep -q "no issues"; then
    pass "clean app → exit 0 + no issues"
else
    fail "clean app (rc=$rc, out=$out)"
fi

# ── Test 2: syntax error in non-entry module → exit 1 + located ───────
rc=0; out=$("$HULL" analyze "$BROKEN" 2>/dev/null) || rc=$?
if [ "$rc" = "1" ] && echo "$out" | grep -q "routes/bad.lua:2:" && echo "$out" | grep -q "lua.syntax"; then
    pass "broken non-entry module → exit 1 + path:line + lua.syntax"
else
    fail "broken app (rc=$rc, out=$out)"
fi

# ── Test 3: deterministic ordering (two --json runs are byte-identical) ─
# (a broken app exits 1; guard the substitutions so `set -e` does not abort)
a=$("$HULL" analyze "$BROKEN" --json 2>/dev/null) || true
b=$("$HULL" analyze "$BROKEN" --json 2>/dev/null) || true
if [ "$a" = "$b" ] && [ -n "$a" ]; then pass "deterministic --json output"; else fail "non-deterministic --json"; fi

# ── Test 4: JSON purity + schema ──────────────────────────────────────
"$HULL" analyze "$BROKEN" --json 2>/dev/null > "$TMP/j.txt" || true
lines=$(wc -l < "$TMP/j.txt" | tr -d ' ')
first=$(head -c1 "$TMP/j.txt")
last=$(tail -c2 "$TMP/j.txt" | head -c1)
if [ "$lines" = "1" ] && [ "$first" = "{" ] && [ "$last" = "}" ] \
   && grep -q '"schema_version":1' "$TMP/j.txt" \
   && grep -q '"root":' "$TMP/j.txt" \
   && grep -q '"code":"lua.syntax"' "$TMP/j.txt" \
   && grep -q '"files_scanned":' "$TMP/j.txt" \
   && grep -q '"summary":' "$TMP/j.txt"; then
    pass "JSON stdout pure (1 line, {…}) + schema_version 1 + required keys"
else
    fail "JSON purity/schema (lines=$lines first=$first last=$last)"
fi

# ── Test 5: explicit target errors (never silent skips) ───────────────
printf 'not lua\n' > "$BROKEN/note.txt"
rc=0; out=$("$HULL" analyze "$BROKEN" nope.lua note.txt ../outside.lua 2>/dev/null) || rc=$?
if [ "$rc" = "1" ] \
   && echo "$out" | grep -q "analyze.not_found" \
   && echo "$out" | grep -q "analyze.not_lua" \
   && echo "$out" | grep -q "analyze.outside_root"; then
    pass "explicit targets → not_found / not_lua / outside_root, exit 1"
else
    fail "explicit target errors (rc=$rc, out=$out)"
fi

# ── Test 6: exclusions (build/, site/build/, vendor/, .git/ not scanned) ─
EXC="$TMP/exc"
mkdir -p "$EXC/build" "$EXC/site/build" "$EXC/vendor" "$EXC/.git"
printf 'app.main(function() return 0 end)\n' > "$EXC/app.lua"
for d in build site/build vendor .git; do
    printf 'this is ) not valid lua\n' > "$EXC/$d/gen.lua"
done
rc=0; out=$("$HULL" analyze "$EXC" 2>/dev/null) || rc=$?
if [ "$rc" = "0" ] && echo "$out" | grep -q "no issues"; then
    pass "excluded dirs (build/site-build/vendor/.git) not scanned → exit 0"
else
    fail "exclusions (rc=$rc, out=$out)"
fi

# ── Test 7: --quiet (clean → empty stdout; broken → diagnostics only) ──
qc=$("$HULL" analyze "$APP" --quiet 2>/dev/null || true)
qb=$("$HULL" analyze "$BROKEN" --quiet 2>/dev/null || true)
if [ -z "$qc" ] && echo "$qb" | grep -q "lua.syntax" && ! echo "$qb" | grep -q "hull analyze:"; then
    pass "--quiet: clean silent, broken shows diagnostics without the summary"
else
    fail "--quiet (qc=[$qc])"
fi

# ── Test 8: exit code 2 (usage error) → empty stdout, message on stderr ─
rc=0; out=$("$HULL" analyze --bogus 2>"$TMP/err.txt") || rc=$?
if [ "$rc" = "2" ] && [ -z "$out" ] && grep -q "unknown flag" "$TMP/err.txt"; then
    pass "unknown flag → exit 2, empty stdout, stderr message"
else
    fail "usage error (rc=$rc, out=[$out])"
fi

# ── summary ───────────────────────────────────────────────────────────
echo ""
echo "=== hull analyze E2E: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
