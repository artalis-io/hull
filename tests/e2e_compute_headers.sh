#!/bin/sh
# tests/e2e_compute_headers.sh — `hull compute new` / `refresh-header` install and
# refresh BOTH Hull-owned headers (hull_compute.h + hull_span.h) atomically,
# reusing the existing embedded-string + write_file delivery (mapped-spans 3b,
# slice 1). Covers: initial scaffolding, refresh of both, legacy one-header
# projects gaining hull_span.h, idempotence, byte-exactness vs the canonical
# templates, and rollback / no-partial-update on failure (never a mismatched pair).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HULL="${HULL:-$ROOT/build/hull}"
CANON_SPAN="$ROOT/templates/hull_span.h"    # canonical source of truth for hull_span.h
PASS=0; FAIL=0
pass() { PASS=$((PASS + 1)); printf "  \033[32mPASS\033[0m: %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  \033[31mFAIL\033[0m: %s\n" "$1"; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cd "$TMP" || exit 1

sha() { shasum "$1" 2>/dev/null | awk '{print $1}'; }

# ── 1. Scaffolding: `new` writes BOTH headers; hull_span.h byte-exact ─────────
"$HULL" compute new foo >/dev/null 2>&1
if [ -f compute/foo/hull_compute.h ] && [ -f compute/foo/hull_span.h ]; then
    pass "new: installs both hull_compute.h and hull_span.h"
else
    fail "new: both headers installed"
fi
if cmp -s compute/foo/hull_span.h "$CANON_SPAN"; then
    pass "new: installed hull_span.h is byte-exact to templates/hull_span.h"
else
    fail "new: hull_span.h byte-exact"
fi
# Reference bytes for the canonical pair (from a clean install).
CANON_COMPUTE_SHA="$(sha compute/foo/hull_compute.h)"
CANON_SPAN_SHA="$(sha compute/foo/hull_span.h)"

# ── 2. Refresh both: corrupt both, refresh restores both to canonical ────────
printf 'garbage' > compute/foo/hull_compute.h
printf 'garbage' > compute/foo/hull_span.h
"$HULL" compute refresh-header foo >/dev/null 2>&1
if [ "$(sha compute/foo/hull_compute.h)" = "$CANON_COMPUTE_SHA" ] \
   && cmp -s compute/foo/hull_span.h "$CANON_SPAN"; then
    pass "refresh: restores BOTH headers to canonical bytes"
else
    fail "refresh: both headers restored"
fi

# ── 3. Legacy one-header project: only hull_compute.h → refresh adds hull_span.h ─
rm -f compute/foo/hull_span.h
"$HULL" compute refresh-header foo >/dev/null 2>&1
if [ -f compute/foo/hull_span.h ] && cmp -s compute/foo/hull_span.h "$CANON_SPAN" \
   && [ "$(sha compute/foo/hull_compute.h)" = "$CANON_COMPUTE_SHA" ]; then
    pass "refresh: legacy hull_compute.h-only project gains hull_span.h (compute.h intact)"
else
    fail "refresh: legacy project upgrade"
fi

# ── 4. Idempotence: refresh twice → identical bytes ──────────────────────────
"$HULL" compute refresh-header foo >/dev/null 2>&1
c1="$(sha compute/foo/hull_compute.h)"; s1="$(sha compute/foo/hull_span.h)"
"$HULL" compute refresh-header foo >/dev/null 2>&1
c2="$(sha compute/foo/hull_compute.h)"; s2="$(sha compute/foo/hull_span.h)"
if [ "$c1" = "$c2" ] && [ "$s1" = "$s2" ] \
   && [ "$c1" = "$CANON_COMPUTE_SHA" ] && [ "$s1" = "$CANON_SPAN_SHA" ]; then
    pass "refresh: idempotent (identical bytes on repeat)"
else
    fail "refresh: idempotence"
fi

# ── 5. Rollback / no-partial-update: force the 2nd header's staging to fail and
#      assert NEITHER real header changed (never a mismatched pair) ───────────
# Block hull_span.h's temp write by pre-creating its temp path as a directory.
before_compute="$(sha compute/foo/hull_compute.h)"
before_span="$(sha compute/foo/hull_span.h)"
mkdir -p compute/foo/hull_span.h.hull-tmp
"$HULL" compute refresh-header foo >/dev/null 2>&1
rc=$?
rmdir compute/foo/hull_span.h.hull-tmp 2>/dev/null
if [ "$rc" -ne 0 ]; then
    pass "refresh: fails non-zero when a header cannot be staged"
else
    fail "refresh: should fail when a header cannot be staged (rc=$rc)"
fi
if [ "$(sha compute/foo/hull_compute.h)" = "$before_compute" ] \
   && [ "$(sha compute/foo/hull_span.h)" = "$before_span" ]; then
    pass "refresh: rollback — neither header changed on failure (no mismatched pair)"
else
    fail "refresh: rollback left a partial/mismatched update"
fi
# No stray temp/backup files left behind by the failed refresh.
if [ -z "$(ls compute/foo/*.hull-tmp compute/foo/*.hull-bak 2>/dev/null)" ]; then
    pass "refresh: no stray .hull-tmp/.hull-bak files after failure"
else
    fail "refresh: leftover temp/backup files after failure"
fi

# ── 6. new refuses to clobber an existing module (unchanged behavior) ────────
"$HULL" compute new foo >/dev/null 2>&1
if [ $? -ne 0 ]; then
    pass "new: refuses an existing module dir"
else
    fail "new: should refuse an existing module dir"
fi

echo ""
echo "compute-headers: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
