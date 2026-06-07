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

echo ""
echo "$PASS/$((PASS + FAIL)) e2e cache tests passed"
[ "$FAIL" -eq 0 ]
