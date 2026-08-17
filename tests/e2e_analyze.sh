#!/bin/sh
# E2E tests — `hull analyze` (static Lua source syntax analysis)
#
# Usage: sh tests/e2e_analyze.sh
# Requires: build/hull already built
#
# Fixtures are built in a TMPDIR (self-contained; keeps intentionally-broken .lua out
# of the repo / the conformance corpus). Covers the locked contract: deterministic
# ordering, syntax error in a NON-entry module, explicit-file target errors (missing /
# unreadable / non-regular / non-Lua / outside-root), symlink containment (canonical),
# symlinked app root, duplicate targets, directory exclusions, JSON purity + schema,
# --quiet, --json overriding --quiet, an incomplete analysis state, and all exit codes.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
HULL_ABS=$(pwd)/build/hull      # absolute (subtests cd into the TMPDIR)
PASS=0
FAIL=0

if [ ! -x "$HULL" ]; then
    echo "e2e-analyze: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }

TMP=$(mktemp -d)
trap 'chmod -R u+rwx "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT

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
else fail "clean app (rc=$rc, out=$out)"; fi

# ── Test 2: syntax error in non-entry module → exit 1 + located ───────
rc=0; out=$("$HULL" analyze "$BROKEN" 2>/dev/null) || rc=$?
if [ "$rc" = "1" ] && echo "$out" | grep -q "routes/bad.lua:2:" && echo "$out" | grep -q "lua.syntax"; then
    pass "broken non-entry module → exit 1 + path:line + lua.syntax"
else fail "broken app (rc=$rc, out=$out)"; fi

# ── Test 3: deterministic ordering (two --json runs are byte-identical) ─
a=$("$HULL" analyze "$BROKEN" --json 2>/dev/null) || true
b=$("$HULL" analyze "$BROKEN" --json 2>/dev/null) || true
if [ "$a" = "$b" ] && [ -n "$a" ]; then pass "deterministic --json output"; else fail "non-deterministic --json"; fi

# ── Test 4: JSON purity + schema ──────────────────────────────────────
"$HULL" analyze "$BROKEN" --json 2>/dev/null > "$TMP/j.txt" || true
lines=$(wc -l < "$TMP/j.txt" | tr -d ' ')
first=$(head -c1 "$TMP/j.txt"); last=$(tail -c2 "$TMP/j.txt" | head -c1)
if [ "$lines" = "1" ] && [ "$first" = "{" ] && [ "$last" = "}" ] \
   && grep -q '"schema_version":1' "$TMP/j.txt" && grep -q '"root":' "$TMP/j.txt" \
   && grep -q '"code":"lua.syntax"' "$TMP/j.txt" && grep -q '"files_scanned":' "$TMP/j.txt" \
   && grep -q '"summary":' "$TMP/j.txt"; then
    pass "JSON stdout pure (1 line, {…}) + schema_version 1 + required keys"
else fail "JSON purity/schema (lines=$lines first=$first last=$last)"; fi

# ── Test 5: explicit target errors — missing + non-Lua (outside-root is
# covered by the symlink test 9, since canonical containment reports a
# non-existent ../path as not_found, honestly, before any containment check) ─
printf 'not lua\n' > "$BROKEN/note.txt"
rc=0; out=$("$HULL" analyze "$BROKEN" nope.lua note.txt 2>/dev/null) || rc=$?
if [ "$rc" = "1" ] && echo "$out" | grep -q "analyze.not_found" \
   && echo "$out" | grep -q "analyze.not_lua"; then
    pass "explicit targets → not_found / not_lua, exit 1"
else fail "explicit target errors (rc=$rc, out=$out)"; fi

# ── Test 6: non-regular explicit target (a directory) → not_regular ───
mkdir -p "$APP/adir.lua"
rc=0; out=$("$HULL" analyze "$APP" adir.lua 2>/dev/null) || rc=$?
if [ "$rc" = "1" ] && echo "$out" | grep -q "analyze.not_regular"; then
    pass "non-regular explicit target → analyze.not_regular"
else fail "non-regular target (rc=$rc, out=$out)"; fi
rmdir "$APP/adir.lua"

# ── Test 7: unreadable vs missing (skipped as root: perms are bypassed) ─
if [ "$(id -u)" = "0" ]; then
    pass "unreadable-vs-missing (skipped: running as root)"
else
    printf 'x = 1\n' > "$APP/noread.lua"; chmod 000 "$APP/noread.lua"
    rc=0; out=$("$HULL" analyze "$APP" noread.lua gone.lua 2>/dev/null) || rc=$?
    if [ "$rc" = "1" ] && echo "$out" | grep -q "noread.lua.*analyze.unreadable" \
       && echo "$out" | grep -q "gone.lua.*analyze.not_found"; then
        pass "unreadable (exists, chmod 000) vs missing → distinct codes"
    else fail "unreadable-vs-missing (rc=$rc, out=$out)"; fi
    chmod 644 "$APP/noread.lua"; rm -f "$APP/noread.lua"
fi

# ── Test 8: duplicate explicit targets analyzed once ──────────────────
n=$("$HULL" analyze "$APP" app.lua app.lua --json 2>/dev/null | grep -o '"files_scanned":[0-9]*') || true
if [ "$n" = '"files_scanned":1' ]; then pass "duplicate explicit targets analyzed once"; else fail "dedup ($n)"; fi

# ── Test 9: symlink INSIDE the app pointing OUTSIDE is not followed ────
# outside.lua is VALID lua, so if the symlink were followed the run would be clean;
# canonical containment must instead reject link.lua (exit 1, an analyze.* error, no
# lua.syntax leaking from the outside file).
mkdir -p "$TMP/sym/app"
printf 'x = 1\n' > "$TMP/sym/outside.lua"
printf 'app.main(function() return 0 end)\n' > "$TMP/sym/app/app.lua"
ln -s "$TMP/sym/outside.lua" "$TMP/sym/app/link.lua"
rc=0; out=$(cd "$TMP/sym" && "$HULL_ABS" analyze app link.lua 2>/dev/null) || rc=$?
if [ "$rc" = "1" ] && echo "$out" | grep -q "link.lua.*analyze\." && ! echo "$out" | grep -q "link.lua.*lua.syntax"; then
    pass "symlink inside app → outside is rejected (containment holds), exit 1"
else fail "symlink containment (rc=$rc, out=$out)"; fi

# ── Test 10: symlinked app ROOT is supported (resolved via realpath) ──
ln -s "$TMP/sym/app" "$TMP/sym/rootlink"
rc=0; out=$(cd "$TMP/sym" && "$HULL_ABS" analyze rootlink 2>/dev/null) || rc=$?
if [ "$rc" = "0" ] && echo "$out" | grep -q "no issues"; then
    pass "symlinked app root supported → analyzed"
else fail "symlinked root (rc=$rc, out=$out)"; fi

# ── Test 11: exclusions pruned during traversal (build/site-build/vendor/.git/.hull/node_modules) ─
EXC="$TMP/exc"
mkdir -p "$EXC/build" "$EXC/site/build" "$EXC/vendor" "$EXC/.git" "$EXC/.hull" "$EXC/node_modules"
printf 'app.main(function() return 0 end)\n' > "$EXC/app.lua"
for d in build site/build vendor .git .hull node_modules; do
    printf 'this is ) not valid lua\n' > "$EXC/$d/gen.lua"
done
rc=0; out=$("$HULL" analyze "$EXC" 2>/dev/null) || rc=$?
if [ "$rc" = "0" ] && echo "$out" | grep -q "no issues"; then
    pass "excluded dirs (build/site-build/vendor/.git/.hull/node_modules) not scanned → exit 0"
else fail "exclusions (rc=$rc, out=$out)"; fi

# ── Test 12: --quiet (clean → empty stdout; broken → diagnostics only) ─
qc=$("$HULL" analyze "$APP" --quiet 2>/dev/null || true)
qb=$("$HULL" analyze "$BROKEN" --quiet 2>/dev/null || true)
if [ -z "$qc" ] && echo "$qb" | grep -q "lua.syntax" && ! echo "$qb" | grep -q "hull analyze:"; then
    pass "--quiet: clean silent, broken shows diagnostics without the summary"
else fail "--quiet (qc=[$qc])"; fi

# ── Test 13: --json overrides --quiet (stdout stays pure JSON) ─────────
jq=$("$HULL" analyze "$APP" --json --quiet 2>/dev/null || true)
if [ -n "$jq" ] && [ "$(printf '%s' "$jq" | head -c1)" = "{" ] && echo "$jq" | grep -q '"schema_version":1'; then
    pass "--json --quiet → JSON overrides quiet, pure JSON stdout"
else fail "--json --quiet override (jq=$jq)"; fi

# ── Test 14: incomplete analysis state (a limit trip) in JSON, exit 1 ──
i=1; parens=""; cparens=""
while [ $i -le 30 ]; do parens="(${parens}"; cparens="${cparens})"; i=$((i + 1)); done
printf 'local x = %s1%s\n' "$parens" "$cparens" > "$APP/deep.lua"
"$HULL" analyze "$APP" deep.lua --max-depth=5 --json 2>/dev/null > "$TMP/deep.json" || true
rc=0; "$HULL" analyze "$APP" deep.lua --max-depth=5 >/dev/null 2>&1 || rc=$?
if [ "$rc" = "1" ] && grep -q '"state":"incomplete"' "$TMP/deep.json" \
   && grep -q '"code":"lua.limit.max_depth"' "$TMP/deep.json" && grep -q '"clean":false' "$TMP/deep.json"; then
    pass "incomplete state (limit trip) → JSON state=incomplete + exit 1"
else fail "incomplete state (rc=$rc)"; fi
rm -f "$APP/deep.lua"

# ── Test 15: exit code 2 (usage error) → empty stdout, message on stderr ─
rc=0; out=$("$HULL" analyze --bogus 2>"$TMP/err.txt") || rc=$?
if [ "$rc" = "2" ] && [ -z "$out" ] && grep -q "unknown flag" "$TMP/err.txt"; then
    pass "unknown flag → exit 2, empty stdout, stderr message"
else fail "usage error (rc=$rc, out=[$out])"; fi

# ── summary ───────────────────────────────────────────────────────────
echo ""
echo "=== hull analyze E2E: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
