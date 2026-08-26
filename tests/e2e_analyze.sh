#!/bin/sh
# E2E tests - `hull analyze` (static Lua source syntax analysis)
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
    echo "e2e-analyze: hull binary not found at $HULL - run 'make' first"
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
printf 'local M = {}\nfunction M.register(app) app.get("/u", function(req, res) res:json({ path = req.path }) end) end\nreturn M\n' > "$APP/routes/users.lua"
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
   && grep -q '"schema_version":2' "$TMP/j.txt" && grep -q '"root":' "$TMP/j.txt" \
   && grep -q '"code":"lua.syntax"' "$TMP/j.txt" && grep -q '"files_scanned":' "$TMP/j.txt" \
   && grep -q '"summary":' "$TMP/j.txt"; then
    pass "JSON stdout pure (1 line, {…}) + schema_version 2 + required keys"
else fail "JSON purity/schema (lines=$lines first=$first last=$last)"; fi

# ── Test 5: explicit target errors - missing + non-Lua (outside-root is
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
if [ -n "$jq" ] && [ "$(printf '%s' "$jq" | head -c1)" = "{" ] && echo "$jq" | grep -q '"schema_version":2'; then
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

# ── Test 16: unreadable NON-excluded discovered subdir → fail closed (exit 2) ─
if [ "$(id -u)" = "0" ]; then
    pass "unreadable subdir fail-closed (skipped: running as root)"
else
    UNR="$TMP/unreadable"
    mkdir -p "$UNR/sub"
    printf 'app.main(function() return 0 end)\n' > "$UNR/app.lua"
    printf 'x = 1\n' > "$UNR/sub/ok.lua"
    chmod 000 "$UNR/sub"
    rc=0; out=$("$HULL" analyze "$UNR" 2>"$TMP/unr_err.txt") || rc=$?
    chmod 755 "$UNR/sub"
    if [ "$rc" = "2" ] && [ -z "$out" ] && grep -q "discovery failed" "$TMP/unr_err.txt"; then
        pass "unreadable discovered subdir → exit 2 + discovery failed (fail closed)"
    else fail "unreadable subdir (rc=$rc, out=[$out], err=$(cat "$TMP/unr_err.txt"))"; fi
fi

# ── lint (v2) fixture: a duplicate table key, an empty block, a TODO ──
LINT="$TMP/lint"
mkdir -p "$LINT"
printf -- '-- TODO: finish this\napp.main(function()\n  local t = { a = 1, a = 2 }\n  if t.a then end\n  return 0\nend)\n' > "$LINT/app.lua"

# ── Test 17: lint warnings are advisory (exit 0), findings still printed ─
rc=0; out=$("$HULL" analyze "$LINT" 2>/dev/null) || rc=$?
if [ "$rc" = "0" ] && echo "$out" | grep -q "lua.lint.duplicate-table-key" \
   && echo "$out" | grep -q "lua.lint.empty-block" && echo "$out" | grep -q "lua.lint.todo-comment"; then
    pass "lint findings advisory (exit 0) with dup-key + empty-block + todo"
else fail "lint advisory (rc=$rc, out=$out)"; fi

# ── Test 18: --strict makes warnings fail (exit 1) ────────────────────
rc=0; "$HULL" analyze "$LINT" --strict >/dev/null 2>&1 || rc=$?
if [ "$rc" = "1" ]; then pass "--strict: warnings fail (exit 1)"; else fail "--strict (rc=$rc)"; fi

# ── Test 19: --list-rules enumerates the registry ─────────────────────
lr=$("$HULL" analyze --list-rules 2>/dev/null || true)
if echo "$lr" | grep -q "duplicate-table-key" && echo "$lr" | grep -q "empty-block" \
   && echo "$lr" | grep -q "todo-comment"; then
    pass "--list-rules lists the rules"
else fail "--list-rules ($lr)"; fi

# ── Test 20: --disable / --rules selection ────────────────────────────
d=$("$HULL" analyze "$LINT" --disable=empty-block,todo-comment 2>/dev/null || true)
r=$("$HULL" analyze "$LINT" --rules=todo-comment 2>/dev/null || true)
if ! echo "$d" | grep -q "empty-block" && echo "$d" | grep -q "duplicate-table-key" \
   && echo "$r" | grep -q "todo-comment" && ! echo "$r" | grep -q "empty-block"; then
    pass "--disable / --rules select the active rules"
else fail "rule selection (d=$d)(r=$r)"; fi

# ── Test 21: unknown rule → exit 2 ────────────────────────────────────
rc=0; out=$("$HULL" analyze "$LINT" --rules=nope 2>"$TMP/lr.txt") || rc=$?
if [ "$rc" = "2" ] && [ -z "$out" ] && grep -q "unknown lint rule" "$TMP/lr.txt"; then
    pass "unknown lint rule → exit 2, stderr message"
else fail "unknown rule (rc=$rc)"; fi

# ── Test 22: JSON v2 schema - lint codes + severities + summary counts ─
"$HULL" analyze "$LINT" --json 2>/dev/null > "$TMP/lint.json" || true
if grep -q '"schema_version":2' "$TMP/lint.json" \
   && grep -q '"code":"lua.lint.duplicate-table-key"' "$TMP/lint.json" \
   && grep -q '"severity":"warning"' "$TMP/lint.json" && grep -q '"severity":"info"' "$TMP/lint.json" \
   && grep -q '"warnings":' "$TMP/lint.json" && grep -q '"by_rule":' "$TMP/lint.json"; then
    pass "JSON v2: lua.lint.* codes + warning/info severities + summary.warnings/by_rule"
else fail "lint JSON schema"; fi

# ── Test 23: a syntax-broken file is NOT linted (no spurious lint) ────
printf 'local t = { a = ) }\n' > "$LINT/oops.lua"
n=$("$HULL" analyze "$LINT" oops.lua --json 2>/dev/null | grep -c 'lua.lint' || true)
rc=0; "$HULL" analyze "$LINT" oops.lua >/dev/null 2>&1 || rc=$?
if [ "$n" = "0" ] && [ "$rc" = "1" ]; then
    pass "syntax-broken file: no spurious lint, exit 1 from the syntax error"
else fail "broken-not-linted (n=$n, rc=$rc)"; fi
rm -f "$LINT/oops.lua"

# ── slice-3 scope-backed rules fixture ────────────────────────────────
SCOPEAPP="$TMP/scopeapp"
mkdir -p "$SCOPEAPP"
printf 'local unused_var = 1\nlocal shadowed = 2\nlocal function helper(a, b)\n  return a + shadowed\nend\ndo\n  local shadowed = 3\n  helper(shadowed, 0)\nend\nreturn os.time()\n' > "$SCOPEAPP/app.lua"

# ── Test 24: unused-local / unused-param / shadowed-local fire ─────────
out=$("$HULL" analyze "$SCOPEAPP" 2>/dev/null || true)
if echo "$out" | grep -q "lua.lint.unused-local" && echo "$out" | grep -q "lua.lint.unused-param" \
   && echo "$out" | grep -q "lua.lint.shadowed-local"; then
    pass "scope rules: unused-local + unused-param + shadowed-local fire"
else fail "scope rules ($out)"; fi

# ── Test 25: undefined-global OFF by default, --enable fires (os), allowed silent ─
off=$("$HULL" analyze "$SCOPEAPP" 2>/dev/null || true)
on=$("$HULL" analyze "$SCOPEAPP" --enable=undefined-global 2>/dev/null || true)
if ! echo "$off" | grep -q "undefined-global" && echo "$on" | grep -q "undefined global 'os'" \
   && ! echo "$on" | grep -q "undefined global 'string'"; then
    pass "undefined-global: OFF by default, --enable fires os, allowlisted globals silent"
else fail "undefined-global (on=$on)"; fi

# ── summary ───────────────────────────────────────────────────────────
echo ""
echo "=== hull analyze E2E: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
