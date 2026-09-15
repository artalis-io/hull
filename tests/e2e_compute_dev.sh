#!/bin/sh
# e2e_compute_dev.sh - E2E tests for the WASM compute developer workflow.
#
# Tests the `hull compute new / build / test / check` lifecycle plus
# integration into `hull build` (auto-rebuild) and `hull agent deploy`
# (per-module enumeration).
#
# Requires: build/hull, clang with wasm32 target support.
# If clang is unavailable the whole suite skips cleanly so CI runs
# without a wasm toolchain do not fail.
#
# Sibling to:
#   tests/e2e_compute.sh       - runtime semantics (compute.call from Lua/JS)
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PASS=0
FAIL=0
TOTAL=0

pass() { PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)); echo "  FAIL: $1"; }

# ── Toolchain detection ──────────────────────────────────────────────
#
# `hull compute build` shells out to clang via the same lookup as the
# shared compute_build.lua helper. If no suitable clang is present
# (Homebrew llvm@18 or system clang with wasm-ld in PATH) we cannot
# exercise the developer workflow, so we skip the entire suite.

# The probe lives in tests/lib/hull_clang.sh - three other suites need the same
# one, and two copies of a host probe is how #501's bug hid.
. "$(dirname "$0")/lib/hull_clang.sh"

if ! CLANG_PATH="$(hull_find_clang)"; then
    echo "=== E2E: compute developer workflow ==="
    echo "  SKIP: no clang with wasm32 + wasm-ld available"
    hull_clang_skip_note
    exit 0
fi

echo "=== E2E: compute developer workflow ==="
echo "  Using clang: $CLANG_PATH"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Resolve hull to an absolute path so cd into TMPDIR still finds it.
case "$HULL" in
    /*) HULL_ABS="$HULL" ;;
    *)  HULL_ABS="$(pwd)/$HULL" ;;
esac

# The two "should error" checks below read an APE's exit status, which a POSIX
# shell on Windows sees as 0 for every outcome (jart/cosmopolitan#1521) - so
# `hull compute new` correctly REFUSING an existing module, and an invalid
# name, both read as success.
. "$(dirname "$0")/lib/hull_rc.sh"
hull_rc_init "$HULL_ABS"

# ── hull compute new ────────────────────────────────────────────────

echo ""
echo "--- hull compute new ---"

cd "$TMPDIR"

# Subject-under-test: scaffold a fresh module.
"$HULL_ABS" compute new score 2>&1 > /tmp/new.log || true

if [ -d compute/score ]; then
    pass "compute/score/ directory created"
else
    fail "compute/score/ directory missing"
fi

if [ -f compute/score/score.c ]; then
    pass "compute/score/score.c written"
else
    fail "compute/score/score.c missing"
fi

if [ -f compute/score/hull_compute.h ]; then
    pass "compute/score/hull_compute.h written"
else
    fail "compute/score/hull_compute.h missing"
fi

if [ -f compute/score/test_fixtures.json ]; then
    pass "compute/score/test_fixtures.json written"
else
    fail "compute/score/test_fixtures.json missing"
fi

# Idempotency / safety: re-running new on an existing module errors.
if [ "$(hull_rc "$HULL_ABS" compute new score)" = 0 ]; then
    fail "hull compute new on existing module should error"
else
    pass "hull compute new on existing module errors (no clobber)"
fi

# Bad names rejected.
if [ "$(hull_rc "$HULL_ABS" compute new 'bad name')" = 0 ]; then
    fail "hull compute new with invalid name should error"
else
    pass "hull compute new rejects invalid name 'bad name'"
fi

# ── hull compute build ──────────────────────────────────────────────

echo ""
echo "--- hull compute build ---"

if "$HULL_ABS" compute build score > /tmp/build.log 2>&1; then
    pass "hull compute build score succeeds"
else
    fail "hull compute build score failed; output:"
    sed 's/^/    /' /tmp/build.log
fi

if [ -f compute/score.wasm ]; then
    pass "compute/score.wasm artifact produced"
    # Check WASM magic: 0x00 0x61 0x73 0x6d "\0asm"
    magic=$(head -c 4 compute/score.wasm | od -An -tx1 | tr -d ' \n')
    if [ "$magic" = "0061736d" ]; then
        pass "compute/score.wasm has valid WASM magic"
    else
        fail "compute/score.wasm magic is '$magic', expected '0061736d'"
    fi
else
    fail "compute/score.wasm artifact missing"
fi

# Build with no args builds all modules; re-running is a no-op (the
# logic doesn't track staleness in `hull compute build` - it always
# rebuilds - but it must still succeed).
if "$HULL_ABS" compute build > /tmp/build_all.log 2>&1; then
    pass "hull compute build (all) succeeds"
else
    fail "hull compute build (all) failed"
fi

# ── hull compute check ──────────────────────────────────────────────

echo ""
echo "--- hull compute check ---"

# `hull compute check` spawns hull test on a tempdir app. Under Cosmo's
# Landlock self-exec restrictions this can fail; under native builds
# it should pass. Either way we treat absence of clean exit on macOS/
# Linux native as a hard fail.
if uname -o 2>/dev/null | grep -q -i cosmo; then
    echo "  SKIP: hull compute check on Cosmo (APE self-exec under Landlock)"
elif "$HULL_ABS" compute check score > /tmp/check.log 2>&1; then
    pass "hull compute check score succeeds"
else
    fail "hull compute check score failed; output:"
    sed 's/^/    /' /tmp/check.log | head -20
fi

# ── hull compute test ───────────────────────────────────────────────

echo ""
echo "--- hull compute test ---"

if uname -o 2>/dev/null | grep -q -i cosmo; then
    echo "  SKIP: hull compute test on Cosmo (APE self-exec under Landlock)"
elif "$HULL_ABS" compute test score > /tmp/test.log 2>&1; then
    pass "hull compute test score succeeds with default fixtures"
else
    fail "hull compute test score failed; output:"
    sed 's/^/    /' /tmp/test.log | head -20
fi

# ── hull build auto-rebuild from source ─────────────────────────────
#
# This is the integration point the compute-dev flow added. The flow:
#
#   1. Establish a known mtime ordering: .wasm older than .c.
#   2. Run `hull build .` (or just `hull build` for the local app).
#      hull build is permitted to abort partway through (missing
#      platform archive, no entry point, etc.) - what matters is
#      that the source-rebuild step ran BEFORE any other artifact
#      step and refreshed compute/score.wasm.
#   3. Verify the .wasm mtime moved forward.

echo ""
echo "--- hull build auto-rebuild ---"

# Write a tiny app so hull build has something to process. We don't
# care if the full build pipeline completes (it usually does not on
# CI without the platform archive); we only care that the compute
# auto-rebuild step runs.
cat > app.lua << 'EOF'
app.get("/", function(req, res) res:json({ ok = true }) end)
EOF

sleep 1
touch compute/score/score.c
# file_mtime FILE -> the mtime in epoch seconds, or non-zero if it cannot be had.
#
# NOT `stat -f %m ... || stat -c %Y ...`. GNU stat's -f means --file-system, so
# given the BSD format string it FAILS (rc=1) *and still prints filesystem info
# to stdout* - the `||` then appends the real answer to six lines of garbage:
#
#     [  File: "score.wasm"
#        ID: 60ed436f00000000 Namelen: 255 ...
#      1789466644]
#
# which is how this suite reported "mtime did not advance (before=  File: ...".
# macOS happens to satisfy the BSD form on the first try, and no CI job runs
# this suite, so it went unnoticed on every GNU-stat host.
#
# GNU form first, and the result is VALIDATED as digits, so neither stat's
# quirks can yield a non-numeric mtime.
file_mtime() {
    _m=$(stat -c %Y "$1" 2>/dev/null) || _m=""
    case "$_m" in ''|*[!0-9]*) _m=$(stat -f %m "$1" 2>/dev/null) || _m="" ;; esac
    case "$_m" in ''|*[!0-9]*) return 1 ;; esac
    printf '%s' "$_m"
}

src_mtime=$(file_mtime compute/score/score.c)
wasm_mtime_before=$(file_mtime compute/score.wasm)

# Run hull build; tolerate non-zero exit (platform-archive errors are
# fine here, the rebuild step runs first).
"$HULL_ABS" build --no-verify-platform -o app . > /tmp/buildlog.txt 2>&1 || true

wasm_mtime_after=$(file_mtime compute/score.wasm)

if grep -q "compiled 1 compute source" /tmp/buildlog.txt; then
    pass "hull build reports 'compiled N compute source(s)' for stale source"
else
    fail "hull build did not report compute auto-rebuild; log:"
    sed 's/^/    /' /tmp/buildlog.txt | head -15
fi

if [ "$wasm_mtime_after" -gt "$wasm_mtime_before" ]; then
    pass "compute/score.wasm mtime advanced after hull build"
else
    fail "compute/score.wasm mtime did not advance (before=$wasm_mtime_before after=$wasm_mtime_after)"
fi

# --no-build-compute should suppress the auto-rebuild even when stale.
sleep 1
touch compute/score/score.c
wasm_mtime_before=$(file_mtime compute/score.wasm)

"$HULL_ABS" build --no-verify-platform --no-build-compute -o app . > /tmp/buildlog2.txt 2>&1 || true

wasm_mtime_after=$(file_mtime compute/score.wasm)

if grep -q "compiled .* compute source" /tmp/buildlog2.txt; then
    fail "--no-build-compute should not rebuild compute"
else
    pass "--no-build-compute suppresses compute auto-rebuild"
fi

if [ "$wasm_mtime_after" = "$wasm_mtime_before" ]; then
    pass "compute/score.wasm mtime unchanged under --no-build-compute"
else
    fail "compute/score.wasm mtime changed despite --no-build-compute"
fi

# ── hull agent deploy enumeration ───────────────────────────────────

echo ""
echo "--- hull agent deploy ---"

# Make source stale again so we exercise the source_stale + recommendation
# path in one go.
sleep 1
touch compute/score/score.c

"$HULL_ABS" agent deploy . > /tmp/deploy.json 2>&1 || true

if grep -q '"compute_modules"' /tmp/deploy.json; then
    pass "hull agent deploy emits compute_modules array"
else
    fail "hull agent deploy missing compute_modules array; output:"
    sed 's/^/    /' /tmp/deploy.json | head -5
fi

if grep -q '"name":"score"' /tmp/deploy.json; then
    pass "compute_modules includes the scaffolded 'score' module"
else
    fail "compute_modules missing 'score' entry"
fi

if grep -q '"has_source":true' /tmp/deploy.json; then
    pass "has_source=true reported for source-backed module"
else
    fail "has_source flag not surfaced"
fi

if grep -q '"source_stale":true' /tmp/deploy.json; then
    pass "source_stale=true reported when .c is newer than .wasm"
else
    fail "source_stale flag not detected"
fi

if grep -q "Compute source newer than" /tmp/deploy.json; then
    pass "recommendations include stale-source advisory"
else
    fail "recommendations missing stale-source advisory"
fi

# ── hull compute refresh-header ─────────────────────────────────────

echo ""
echo "--- hull compute refresh-header ---"

# Corrupt the per-module copy and verify refresh-header restores it
# from the embedded canonical version.
echo "// CORRUPTED - should be overwritten by refresh-header" > compute/score/hull_compute.h

if "$HULL_ABS" compute refresh-header score > /tmp/refresh.log 2>&1; then
    pass "hull compute refresh-header score succeeds"
else
    fail "hull compute refresh-header score failed"
fi

if grep -q "HULL_COMPUTE_H" compute/score/hull_compute.h; then
    pass "compute/score/hull_compute.h restored to canonical version"
else
    fail "hull_compute.h was not overwritten; still contains corrupted content"
fi

# Refresh-header with no name should refresh every module - scaffold a
# second module to exercise the multi-module path.
"$HULL_ABS" compute new other > /dev/null 2>&1 || true
echo "// CORRUPTED" > compute/score/hull_compute.h
echo "// CORRUPTED" > compute/other/hull_compute.h

if "$HULL_ABS" compute refresh-header > /tmp/refresh_all.log 2>&1; then
    pass "hull compute refresh-header (all) succeeds"
else
    fail "hull compute refresh-header (all) failed"
fi

if grep -q "HULL_COMPUTE_H" compute/score/hull_compute.h && \
   grep -q "HULL_COMPUTE_H" compute/other/hull_compute.h; then
    pass "refresh-header (all) restored every module's header"
else
    fail "refresh-header (all) did not restore all headers"
fi

# ── Result summary ──────────────────────────────────────────────────

echo ""
echo "=== Results ==="
echo "  Total:  $TOTAL"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
