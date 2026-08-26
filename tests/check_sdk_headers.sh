#!/bin/sh
# tests/check_sdk_headers.sh - enforce that every MAINTAINED Hull-owned compute
# SDK header copy is byte-identical to the single canonical source: the embedded
# HULL_COMPUTE_H / HULL_SPAN_H literals in stdlib/cli/lua/hull/compute.lua.
#
# Maintained copies = committed hull_compute.h / hull_span.h under examples/ and
# tests/fixtures/, plus templates/hull_span.h. User-owned / generated files
# outside the maintained repository fixtures (e.g. .claude/worktrees, a user's own
# project scaffolded via `hull compute new`) are intentionally NOT checked.
#
# Reports the EXACT drifting path(s) and exits non-zero deterministically on any
# drift. Fix a reported drift by re-running `hull compute refresh-header` in the
# affected module directory. (#331)
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

CANON_C="$(mktemp)"; CANON_S="$(mktemp)"
trap 'rm -f "$CANON_C" "$CANON_S"' EXIT

extract() {  # literal-name, out-file
    awk -v m="local $1 = [[" '
        index($0, m) == 1 { f = 1; sub(/^local [A-Z_]+ = \[\[/, ""); }
        f { print }
        /^\]\]$/ { if (f) exit }
    ' stdlib/cli/lua/hull/compute.lua | sed '$d' > "$2"
}
extract HULL_COMPUTE_H "$CANON_C"
extract HULL_SPAN_H    "$CANON_S"
[ -s "$CANON_C" ] || { echo "::error::could not extract canonical HULL_COMPUTE_H from compute.lua"; exit 2; }
[ -s "$CANON_S" ] || { echo "::error::could not extract canonical HULL_SPAN_H from compute.lua"; exit 2; }

drift=0
checked=0

# Only MAINTAINED copies: tracked files under examples/ or tests/fixtures/ (plus
# templates/hull_span.h for the span header). Sorted for deterministic output.
maintained() { git ls-files "$1" "$2" 2>/dev/null | grep -E '^(examples|tests/fixtures|templates)/' | sort; }

echo "== compute-SDK header canonical-copy check =="
for f in $(maintained '**/hull_compute.h' 'ignore'); do
    checked=$((checked + 1))
    if ! cmp -s "$f" "$CANON_C"; then
        echo "::error::DRIFT $f differs from the canonical HULL_COMPUTE_H"
        drift=1
    fi
done
for f in $(maintained '**/hull_span.h' 'templates/hull_span.h'); do
    checked=$((checked + 1))
    if ! cmp -s "$f" "$CANON_S"; then
        echo "::error::DRIFT $f differs from the canonical HULL_SPAN_H"
        drift=1
    fi
done

if [ "$drift" -ne 0 ]; then
    echo "FAIL: a maintained SDK header copy drifted from the canonical embedded source."
    echo "Fix: run 'hull compute refresh-header' in the affected module directory, then commit."
    exit 1
fi
echo "OK: all $checked maintained hull_compute.h / hull_span.h copies are byte-identical to the canonical."
