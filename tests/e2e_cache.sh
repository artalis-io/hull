#!/bin/sh
# e2e_cache.sh — End-to-end tests for `hull cache list|prune|clear`
# plus the HULL_CACHE_DIR per-app isolation override.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
case "$HULL" in
    /*) ;;
    *)  HULL="$(pwd)/$HULL" ;;
esac

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1${2:+ — $2}"; }

# Hermetic HOME so we never touch the developer's real cache pool.
TMPHOME=$(mktemp -d)
export HOME="$TMPHOME"
trap 'rm -rf "$TMPHOME"' EXIT

echo "=== E2E: hull cache (HOME=$TMPHOME) ==="

# ── 1. Empty state ──────────────────────────────────────────────
OUT=$("$HULL" cache list 2>&1)
case "$OUT" in
    *"lua-bytecode"*) pass "list shows lua-bytecode kind" ;;
    *) fail "list shows lua-bytecode" "got: $OUT" ;;
esac
case "$OUT" in
    *"templates"*) pass "list shows templates kind" ;;
    *) fail "list shows templates" ;;
esac
case "$OUT" in
    *"tools"*) pass "list shows tools kind" ;;
    *) fail "list shows tools" ;;
esac
case "$OUT" in
    *"system"*) pass "list flags tools as system" ;;
    *) fail "list flags tools as system" ;;
esac
case "$OUT" in
    *"Runtime: 0 entries"*) pass "empty runtime totals" ;;
    *) fail "empty runtime totals" "got: $OUT" ;;
esac

# ── 2. JSON output ──────────────────────────────────────────────
JSON=$("$HULL" cache list --json 2>&1)
case "$JSON" in
    *'"caches":['*) pass "json has caches array" ;;
    *) fail "json shape" "got: $JSON" ;;
esac
case "$JSON" in
    *'"name":"lua-bytecode"'*) pass "json includes lua-bytecode entry" ;;
    *) fail "json lua-bytecode entry" ;;
esac
case "$JSON" in
    *'"is_runtime":true'*) pass "json flags runtime" ;;
    *) fail "json runtime flag" ;;
esac
case "$JSON" in
    *'"hull_cache_dir":""'*) pass "json reports empty HULL_CACHE_DIR" ;;
    *) fail "json HULL_CACHE_DIR field" ;;
esac

# ── 3. Populate the bytecode cache by booting any app ──────────
# Use the hello example — it loads the Lua stdlib which exercises the
# bytecode cache. Bind to a high port + --no-sandbox so it stays
# hermetic.
TMPDB=$(mktemp -d)
HOME="$TMPHOME" "$HULL" examples/hello/app.lua \
    -p 19851 --no-sandbox --no-migrate \
    >/dev/null 2>&1 &
PID=$!
sleep 1
curl -s --max-time 2 "http://127.0.0.1:19851/health" >/dev/null 2>&1 || true
kill -INT $PID 2>/dev/null || true
sleep 1
kill -KILL $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
rm -rf "$TMPDB"

# Now there should be lua-bytecode entries
COUNT=$(find "$TMPHOME/.hull/blobs/runtime/lua-bytecode" -type f 2>/dev/null \
        | wc -l | tr -d ' ')
if [ "$COUNT" -gt 0 ]; then
    pass "stdlib boot populated lua-bytecode cache ($COUNT entries)"
else
    fail "stdlib boot did not populate cache" "got $COUNT"
fi

# ── 4. list reflects the populated state ─────────────────────────
OUT=$("$HULL" cache list 2>&1)
case "$OUT" in
    *"lua-bytecode"*[1-9]*) pass "list reports nonzero count" ;;
    *) fail "list nonzero count" "got: $OUT" ;;
esac

# ── 5. prune without bounds is rejected ──────────────────────────
RC=0
OUT=$("$HULL" cache prune 2>&1) || RC=$?
case "$OUT" in
    *"need --max-size"*) pass "prune refuses without bounds" ;;
    *) fail "prune bounds check" "got: $OUT" ;;
esac

# ── 6. prune --dry-run reports correctly without unlinking ──────
PRE=$(find "$TMPHOME/.hull/blobs/runtime/lua-bytecode" -type f 2>/dev/null \
      | wc -l | tr -d ' ')
OUT=$("$HULL" cache prune --max-size=1 --dry-run 2>&1)
POST=$(find "$TMPHOME/.hull/blobs/runtime/lua-bytecode" -type f 2>/dev/null \
       | wc -l | tr -d ' ')
case "$OUT" in
    *"would remove"*) pass "dry-run uses 'would remove' verb" ;;
    *) fail "dry-run verb" "got: $OUT" ;;
esac
[ "$PRE" = "$POST" ] && pass "dry-run does not unlink" \
                     || fail "dry-run unlinked" "$PRE -> $POST"

# ── 7. prune --max-size=1 actually evicts everything ────────────
"$HULL" cache prune --max-size=1 >/dev/null 2>&1
COUNT=$(find "$TMPHOME/.hull/blobs/runtime/lua-bytecode" -type f 2>/dev/null \
        | wc -l | tr -d ' ')
[ "$COUNT" = "0" ] && pass "prune --max-size=1 cleared bytecode cache" \
                   || fail "prune did not evict all" "remaining: $COUNT"

# ── 8. clear without --yes refuses ───────────────────────────────
RC=0
OUT=$("$HULL" cache clear --kind=lua-bytecode 2>&1) || RC=$?
case "$OUT" in
    *"refusing to wipe"*) pass "clear refuses without --yes" ;;
    *) fail "clear refusal" "got: $OUT" ;;
esac

# ── 9. clear --kind=tools requires explicit kind (no default sweep)
# Re-populate by booting again so we have something to test.
HOME="$TMPHOME" "$HULL" examples/hello/app.lua \
    -p 19851 --no-sandbox --no-migrate >/dev/null 2>&1 &
PID=$!
sleep 1
kill -INT $PID 2>/dev/null || true
sleep 1
kill -KILL $PID 2>/dev/null || true

# Plant a fake "tools" file to prove default clear leaves it alone.
# Filename must be 64 lowercase hex chars to pass blob_store's
# validate_id — otherwise cleanup walks past it and the "default
# clear preserves tools" check is meaningless.
mkdir -p "$TMPHOME/.hull/blobs/tools/blobs/aa"
FAKE_TOOL="$TMPHOME/.hull/blobs/tools/blobs/aa/aa1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcd"
printf 'fake' > "$FAKE_TOOL"
"$HULL" cache clear --yes >/dev/null 2>&1
[ -f "$FAKE_TOOL" ] && pass "clear (no --kind) preserves tools store" \
                    || fail "clear wiped tools by default"

# Confirm runtime caches WERE wiped.
RT_COUNT=$(find "$TMPHOME/.hull/blobs/runtime" -type f 2>/dev/null \
           | wc -l | tr -d ' ')
[ "$RT_COUNT" = "0" ] && pass "clear (no --kind) wiped runtime caches" \
                      || fail "runtime not wiped" "remaining: $RT_COUNT"

# Now explicit --kind=tools removes it.
"$HULL" cache clear --kind=tools --yes >/dev/null 2>&1
[ ! -f "$FAKE_TOOL" ] && pass "clear --kind=tools --yes removes tools" \
                     || fail "clear --kind=tools did not remove"

# ── 10. HULL_CACHE_DIR per-app isolation ─────────────────────────
ISOLATED=$(mktemp -d)
# Populate isolated cache via app boot
HULL_CACHE_DIR="$ISOLATED" HOME="$TMPHOME" "$HULL" examples/hello/app.lua \
    -p 19852 --no-sandbox --no-migrate >/dev/null 2>&1 &
PID=$!
sleep 1
kill -INT $PID 2>/dev/null || true
sleep 1
kill -KILL $PID 2>/dev/null || true

ISO_COUNT=$(find "$ISOLATED" -type f 2>/dev/null | wc -l | tr -d ' ')
HOME_RT=$(find "$TMPHOME/.hull/blobs/runtime" -type f 2>/dev/null | wc -l | tr -d ' ')

if [ "$ISO_COUNT" -gt 0 ]; then
    pass "HULL_CACHE_DIR receives the cache ($ISO_COUNT entries)"
else
    fail "HULL_CACHE_DIR not populated"
fi
if [ "$HOME_RT" = "0" ]; then
    pass "HOME runtime caches untouched when HULL_CACHE_DIR set"
else
    fail "HOME runtime caches written when override active" "got: $HOME_RT"
fi

OUT=$(HULL_CACHE_DIR="$ISOLATED" "$HULL" cache list 2>&1)
case "$OUT" in
    *"$ISOLATED"*) pass "list reports the override path" ;;
    *) fail "list override path" "got: $OUT" ;;
esac
case "$OUT" in
    *"HULL_CACHE_DIR override active"*)
        pass "list footer flags the override" ;;
    *) fail "list footer override flag" ;;
esac

# ── 11. HULL_CACHE_DIR rejects relative paths ────────────────────
RC=0
HULL_CACHE_DIR="relative/path" "$HULL" cache list >/dev/null 2>&1 || RC=$?
# We don't have a tight assertion on exit code (list might still
# render with the relative path falling back to HOME), but the cache
# subdir helper should refuse to mkdir under a relative root. The
# best behavioural check is that no files leak into ./relative/path.
[ ! -d "relative/path" ] && pass "relative HULL_CACHE_DIR not auto-mkdir'd" \
                         || { rm -rf "relative"; fail "relative path created cache"; }

rm -rf "$ISOLATED"

# ── 12. `hull doctor` surfaces the cache section ─────────────────
OUT=$("$HULL" doctor 2>&1)
case "$OUT" in
    *"Caches  (runtime + tools storage)"*)
        pass "doctor shows Caches section" ;;
    *) fail "doctor Caches section" ;;
esac
case "$OUT" in
    *"lua-bytecode"*"entries"*) pass "doctor lists lua-bytecode entries" ;;
    *) fail "doctor lua-bytecode line" ;;
esac
case "$OUT" in
    *"system store"*) pass "doctor flags tools as system store" ;;
    *) fail "doctor system-store annotation" ;;
esac
case "$OUT" in
    *"manage via \`hull cache list|prune|clear\`"*)
        pass "doctor points at cache subcommand" ;;
    *) fail "doctor cache subcommand pointer" ;;
esac

# 13. doctor --json includes caches array + hull_cache_dir field
JSON=$("$HULL" doctor --json 2>&1)
case "$JSON" in
    *'"caches":'*) pass "doctor --json includes caches array" ;;
    *) fail "doctor --json caches" ;;
esac
case "$JSON" in
    *'"hull_cache_dir":'*)
        pass "doctor --json includes hull_cache_dir" ;;
    *) fail "doctor --json hull_cache_dir" ;;
esac

# 14. HULL_CACHE_DIR surfaces in doctor output too
ISO2=$(mktemp -d)
OUT=$(HULL_CACHE_DIR="$ISO2" "$HULL" doctor 2>&1)
case "$OUT" in
    *"HULL_CACHE_DIR active"*) pass "doctor surfaces active override" ;;
    *) fail "doctor override surface" ;;
esac
case "$OUT" in
    *"$ISO2"*) pass "doctor reports override path" ;;
    *) fail "doctor override path" ;;
esac
rm -rf "$ISO2"

# ── 15. `hull inspect` surfaces the runtime-cache disclosure ─────
INSP_DIR=$(mktemp -d)
cat > "$INSP_DIR/package.sig" <<'SIG'
{"version":1,"manifest":{"fs":{"read":["data/in"]}},"files":{"./app.lua":"abc"}}
SIG
OUT=$("$HULL" inspect "$INSP_DIR" 2>&1)
case "$OUT" in
    *"Runtime caches (auto-allowed, not in manifest)"*)
        pass "inspect shows runtime-caches section" ;;
    *) fail "inspect cache section" ;;
esac
case "$OUT" in
    *"lua-bytecode [runtime]"*)
        pass "inspect lists lua-bytecode kind" ;;
    *) fail "inspect lua-bytecode line" ;;
esac
case "$OUT" in
    *"tools        [system]"*)
        pass "inspect flags tools as system" ;;
    *) fail "inspect system flag" ;;
esac
case "$OUT" in
    *"hull cache list|prune|clear"*)
        pass "inspect points at cache subcommand" ;;
    *) fail "inspect cache-subcommand pointer" ;;
esac
case "$OUT" in
    *"HULL_NO_CACHE=1"*) pass "inspect mentions opt-out" ;;
    *) fail "inspect opt-out mention" ;;
esac

# 16. inspect under HULL_CACHE_DIR surfaces override line
ISO3=$(mktemp -d)
OUT=$(HULL_CACHE_DIR="$ISO3" "$HULL" inspect "$INSP_DIR" 2>&1)
case "$OUT" in
    *"HULL_CACHE_DIR override active"*"$ISO3"*)
        pass "inspect surfaces HULL_CACHE_DIR override" ;;
    *) fail "inspect override surface" "got: $OUT" ;;
esac
case "$OUT" in
    *"$ISO3/lua-bytecode"*)
        pass "inspect paths reflect override" ;;
    *) fail "inspect override paths" ;;
esac
rm -rf "$ISO3" "$INSP_DIR"

# ── 17. Status column reflects per-cache opt-out ─────────────────
# Baseline: nothing set → every kind shows "ok" (or "n/a" for system)
OUT=$("$HULL" cache list 2>&1)
case "$OUT" in
    *"lua-bytecode runtime  ok"*) pass "list: lua-bytecode is ok by default" ;;
    *) fail "list: lua-bytecode default status" "got: $OUT" ;;
esac
case "$OUT" in
    *"tools        system   n/a"*) pass "list: tools shows n/a (no opt-out)" ;;
    *) fail "list: tools n/a status" ;;
esac

# Per-cache opt-out flips lua-bytecode to "off (env)", others stay ok.
OUT=$(HULL_NO_LUA_BYTECODE_CACHE=1 "$HULL" cache list 2>&1)
case "$OUT" in
    *"lua-bytecode runtime  off (env)"*)
        pass "list: per-cache env disable shows 'off (env)'" ;;
    *) fail "list: lua-bytecode off (env)" ;;
esac
case "$OUT" in
    *"js-bytecode  runtime  ok"*)
        pass "list: per-cache disable doesn't affect other kinds" ;;
    *) fail "list: js-bytecode still ok" ;;
esac
case "$OUT" in
    *"HULL_NO_LUA_BYTECODE_CACHE=1 active"*)
        pass "list footer names the active opt-out var" ;;
    *) fail "list footer per-cache opt-out" ;;
esac

# Global kill-switch flips ALL runtime kinds to "off (all)".
OUT=$(HULL_NO_CACHE=1 "$HULL" cache list 2>&1)
case "$OUT" in
    *"lua-bytecode runtime  off (all)"*)
        pass "list: HULL_NO_CACHE shows 'off (all)' on lua" ;;
    *) fail "list: HULL_NO_CACHE on lua" ;;
esac
case "$OUT" in
    *"js-bytecode  runtime  off (all)"*)
        pass "list: HULL_NO_CACHE shows 'off (all)' on js too" ;;
    *) fail "list: HULL_NO_CACHE on js" ;;
esac

# JSON output exposes env_var + disabled fields.
JSON=$("$HULL" cache list --json 2>&1)
case "$JSON" in
    *'"env_var":"HULL_NO_LUA_BYTECODE_CACHE"'*)
        pass "json includes env_var per kind" ;;
    *) fail "json env_var field" ;;
esac
case "$JSON" in
    *'"disabled":false'*) pass "json includes disabled flag" ;;
    *) fail "json disabled field" ;;
esac

# JSON under disable: lua-bytecode disabled=true, others stay false.
JSON=$(HULL_NO_LUA_BYTECODE_CACHE=1 "$HULL" cache list --json 2>&1)
case "$JSON" in
    *'"name":"lua-bytecode"'*'"disabled":true'*)
        pass "json reflects per-cache disable for lua-bytecode" ;;
    *) fail "json disable reflection" ;;
esac

# ── 18. The OLD HULL_NO_BYTECODE_CACHE no longer disables anything
OUT=$(HULL_NO_BYTECODE_CACHE=1 "$HULL" cache list 2>&1)
case "$OUT" in
    *"lua-bytecode runtime  ok"*)
        pass "old HULL_NO_BYTECODE_CACHE is inert (renamed)" ;;
    *) fail "old HULL_NO_BYTECODE_CACHE should be ignored now" ;;
esac

# ── 19. Time-string + size-string parsers for `cache prune` ───────
# --max-age accepts unit suffixes (s/m/h/d/w/y).
RC=0
OUT=$("$HULL" cache prune --max-age=30d --dry-run 2>&1) || RC=$?
[ "$RC" -eq 0 ] && pass "prune --max-age=30d parses cleanly" \
                || fail "prune --max-age=30d" "rc=$RC out=$OUT"
RC=0
OUT=$("$HULL" cache prune --max-age=24h --dry-run 2>&1) || RC=$?
[ "$RC" -eq 0 ] && pass "prune --max-age=24h parses cleanly" \
                || fail "prune --max-age=24h"
# Bare seconds still work (back-compat).
RC=0
OUT=$("$HULL" cache prune --max-age=2592000 --dry-run 2>&1) || RC=$?
[ "$RC" -eq 0 ] && pass "prune --max-age=2592000 (bare seconds) still accepted" \
                || fail "bare-seconds max-age regressed"
# Bad unit rejected with hint.
RC=0
OUT=$("$HULL" cache prune --max-age=30days --dry-run 2>&1) || RC=$?
[ "$RC" -ne 0 ] && pass "prune rejects unknown duration unit" \
                || fail "prune accepted bad unit"
case "$OUT" in
    *"examples: --max-age=30d"*)
        pass "prune error includes example" ;;
    *) fail "prune error example missing" ;;
esac

# --max-size accepts K/M/G suffixes.
RC=0
OUT=$("$HULL" cache prune --max-size=100M --dry-run 2>&1) || RC=$?
[ "$RC" -eq 0 ] && pass "prune --max-size=100M parses cleanly" \
                || fail "prune --max-size=100M"
RC=0
OUT=$("$HULL" cache prune --max-size=100MB --dry-run 2>&1) || RC=$?
[ "$RC" -eq 0 ] && pass "prune --max-size=100MB (trailing B) parses" \
                || fail "prune --max-size=100MB"
RC=0
OUT=$("$HULL" cache prune --max-size=104857600 --dry-run 2>&1) || RC=$?
[ "$RC" -eq 0 ] && pass "prune --max-size=104857600 (bare bytes) still accepted" \
                || fail "bare-bytes max-size regressed"

# ── 20. doctor surfaces a "large cache" warning past the threshold
LARGE_HOME=$(mktemp -d)
mkdir -p "$LARGE_HOME/.hull/blobs/runtime/lua-bytecode/blobs/aa"
# 300 MB of zeros — comfortably past the 250 MB per-kind threshold.
# bs=1m is a macOS-ism (Linux uses 1M); use 1024*1024 bytes for both.
dd if=/dev/zero of="$LARGE_HOME/.hull/blobs/runtime/lua-bytecode/blobs/aa/aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899" \
    bs=1048576 count=300 >/dev/null 2>&1
OUT=$(HOME="$LARGE_HOME" "$HULL" doctor 2>&1)
case "$OUT" in
    *"lua-bytecode"*"large"*) pass "doctor flags large per-kind cache" ;;
    *) fail "doctor large-kind warning" ;;
esac
case "$OUT" in
    *"hull cache prune --max-age=30d"*)
        pass "doctor surfaces actionable prune hint" ;;
    *) fail "doctor prune hint" ;;
esac
rm -rf "$LARGE_HOME"

# ── 21. `prune --json` shape ─────────────────────────────────────
JSON=$("$HULL" cache prune --max-age=30d --dry-run --json 2>&1)
case "$JSON" in
    *'"results":['*'"total_removed":'*'"total_freed_bytes":'*'"dry_run":true'*)
        pass "prune --json includes results + totals + dry_run" ;;
    *) fail "prune --json shape" "got: $JSON" ;;
esac
# Echo into python json.tool for a stricter validity check (no
# trailing-comma or whitespace bugs).
if echo "$JSON" | python3 -m json.tool >/dev/null 2>&1; then
    pass "prune --json is valid JSON"
else
    fail "prune --json invalid JSON" "got: $JSON"
fi

# ── 22. `clear --json` shape ─────────────────────────────────────
# Clear refuses without --yes even in JSON mode (refusal is a
# process error, not a structured result).
RC=0
OUT=$("$HULL" cache clear --json 2>&1) || RC=$?
[ "$RC" -ne 0 ] && pass "clear --json without --yes still refuses" \
                || fail "clear --json bypassed --yes"

# Populate the cache so clear has something to do, then JSON-wipe.
INSP_HOME=$(mktemp -d)
HOME="$INSP_HOME" "$HULL" examples/hello/app.lua -p 19961 \
    --no-sandbox --no-migrate >/dev/null 2>&1 &
PID=$!
sleep 2
kill -INT $PID 2>/dev/null || true
sleep 1
kill -KILL $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

JSON=$(HOME="$INSP_HOME" "$HULL" cache clear --yes --json 2>&1)
case "$JSON" in
    *'"results":['*'"total_removed":'*'"total_freed_bytes":'*)
        pass "clear --json includes results + totals" ;;
    *) fail "clear --json shape" "got: $JSON" ;;
esac
if echo "$JSON" | python3 -m json.tool >/dev/null 2>&1; then
    pass "clear --json is valid JSON"
else
    fail "clear --json invalid JSON" "got: $JSON"
fi
# After clear --yes --json, runtime caches should actually be empty.
remain=$(find "$INSP_HOME/.hull/blobs/runtime" -type f 2>/dev/null \
              | wc -l | tr -d ' ')
[ "$remain" = "0" ] \
    && pass "clear --yes --json actually wiped runtime caches" \
    || fail "clear --yes --json left $remain files behind"
rm -rf "$INSP_HOME"

# ── 23. `verify` finds corruption + `verify --repair` fixes ──
VERIFY_HOME=$(mktemp -d)
HOME="$VERIFY_HOME" "$HULL" examples/hello/app.lua -p 19975 \
    --no-sandbox --no-migrate >/dev/null 2>&1 &
PID=$!
sleep 2
kill -INT $PID 2>/dev/null || true; sleep 1; kill -KILL $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

# Clean state should verify clean.
OUT=$(HOME="$VERIFY_HOME" "$HULL" cache verify 2>&1)
RC=$?
case "$OUT" in
    *"0 corrupt"*) pass "verify on clean cache reports 0 corrupt" ;;
    *) fail "verify clean output" "got: $OUT" ;;
esac
[ "$RC" -eq 0 ] && pass "verify on clean cache exits 0" \
                || fail "verify clean rc" "rc=$RC"

# Corrupt one file by truncating it to zero bytes.
SOMEFILE=$(find "$VERIFY_HOME/.hull/blobs/runtime/lua-bytecode/blobs" \
                -type f | head -1)
if [ -n "$SOMEFILE" ]; then
    truncate -s 0 "$SOMEFILE"
    RC=0
    OUT=$(HOME="$VERIFY_HOME" "$HULL" cache verify 2>&1) || RC=$?
    case "$OUT" in
        *"zero-size"*) pass "verify detects zero-size corruption" ;;
        *) fail "verify zero-size detection" "got: $OUT" ;;
    esac
    [ "$RC" -ne 0 ] && pass "verify exits non-zero on corruption" \
                    || fail "verify rc on corruption" "got rc=$RC"
    case "$OUT" in
        *"re-run with --repair"*) pass "verify suggests --repair" ;;
        *) fail "verify --repair suggestion missing" ;;
    esac

    # --repair unlinks the corrupt entry.
    RC=0
    OUT=$(HOME="$VERIFY_HOME" "$HULL" cache verify --repair 2>&1) || RC=$?
    case "$OUT" in
        *"unlinked"*) pass "verify --repair reports unlink" ;;
        *) fail "verify --repair output" "got: $OUT" ;;
    esac
    [ "$RC" -eq 0 ] && pass "verify --repair exits 0 after fix" \
                    || fail "verify --repair rc" "got rc=$RC"
    [ ! -f "$SOMEFILE" ] && pass "verify --repair actually unlinked the file" \
                         || fail "verify --repair left file behind"

    # JSON output shape.
    JSON=$(HOME="$VERIFY_HOME" "$HULL" cache verify --json 2>&1)
    case "$JSON" in
        *'"results":['*'"total_checked":'*'"total_corrupt":'*)
            pass "verify --json shape" ;;
        *) fail "verify --json shape" "got: $JSON" ;;
    esac
    if echo "$JSON" | python3 -m json.tool >/dev/null 2>&1; then
        pass "verify --json is valid JSON"
    else
        fail "verify --json invalid JSON"
    fi
fi
rm -rf "$VERIFY_HOME"

echo ""
echo "$PASS/$((PASS + FAIL)) e2e cache tests passed"
[ "$FAIL" -eq 0 ]
