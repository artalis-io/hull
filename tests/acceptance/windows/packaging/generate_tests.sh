#!/bin/sh
# generate_tests.sh - positive + negative tests for packaging/windows/generate.sh.
# Exercises the fail-closed hull.sha256 handling (missing / duplicate / malformed
# / bad-hash entries) and the strict tag validation. POSIX sh; no network (uses
# --sha256-file). Run: sh tests/acceptance/windows/packaging/generate_tests.sh
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

unset CDPATH 2>/dev/null || true
SELF_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
GEN="$SELF_DIR/../../../../packaging/windows/generate.sh"
[ -f "$GEN" ] || { echo "cannot find generate.sh at $GEN" >&2; exit 1; }

HEX64=$(printf '%064d' 0)                 # 64 hex chars (zeros)
SHORT63=$(printf '%063d' 0)               # 63 chars (wrong length)
NONHEX64=gg$(printf '%062d' 0)            # 64 chars, non-hex (leading gg)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
pass=0; fail=0

# run_expect <ok|err> <label> <tag> <manifest-file>
run_expect() {
    exp=$1; label=$2; tag=$3; mf=$4
    out="$tmp/out"; rm -rf "$out"
    if sh "$GEN" --tag "$tag" --sha256-file "$mf" --out "$out" >/dev/null 2>&1; then rc=0; else rc=1; fi
    if { [ "$exp" = ok ] && [ "$rc" -eq 0 ]; } || { [ "$exp" = err ] && [ "$rc" -ne 0 ]; }; then
        echo "PASS: $label"; pass=$((pass + 1))
    else
        echo "FAIL: $label (expected $exp, got rc=$rc)"; fail=$((fail + 1))
    fi
}

# Positive: exactly one valid hull-cosmo entry (among other assets) + valid tag.
printf '%s  hull-cosmo\n%s  hull-linux-x86_64\n' "$HEX64" "$HEX64" > "$tmp/valid.sha256"
run_expect ok  "one valid hull-cosmo entry + valid tag"      v0.14.0 "$tmp/valid.sha256"

# Missing entry.
printf '%s  hull-linux-x86_64\n' "$HEX64" > "$tmp/missing.sha256"
run_expect err "missing hull-cosmo entry"                    v0.14.0 "$tmp/missing.sha256"

# Duplicate valid entries.
printf '%s  hull-cosmo\n%s  hull-cosmo\n' "$HEX64" "$HEX64" > "$tmp/dup.sha256"
run_expect err "duplicate valid hull-cosmo entries"          v0.14.0 "$tmp/dup.sha256"

# Valid + malformed duplicate (short hash, still names hull-cosmo).
printf '%s  hull-cosmo\ndeadbeef  hull-cosmo\n' "$HEX64" > "$tmp/valid_plus_malformed.sha256"
run_expect err "valid + malformed hull-cosmo duplicate"      v0.14.0 "$tmp/valid_plus_malformed.sha256"

# Non-hex hash (64 chars, non-hex).
printf '%s  hull-cosmo\n' "$NONHEX64" > "$tmp/nonhex.sha256"
run_expect err "non-hex hull-cosmo hash"                     v0.14.0 "$tmp/nonhex.sha256"

# Wrong-length hash (63 hex).
printf '%s  hull-cosmo\n' "$SHORT63" > "$tmp/short.sha256"
run_expect err "wrong-length hull-cosmo hash"                v0.14.0 "$tmp/short.sha256"

# Invalid / injection-shaped tags (the manifest is valid; only the tag is bad).
run_expect err "injection tag [v1.2.3; rm -rf /]"            'v1.2.3; rm -rf /' "$tmp/valid.sha256"
run_expect err "trailing-junk tag [v1.2.3xyz]"              'v1.2.3xyz'        "$tmp/valid.sha256"
run_expect err "no-v tag [1.2.3]"                           '1.2.3'            "$tmp/valid.sha256"
run_expect err "short tag [v1.2]"                           'v1.2'             "$tmp/valid.sha256"
run_expect err "trailing-space tag [v1.2.3 ]"              'v1.2.3 '          "$tmp/valid.sha256"
run_expect err "non-numeric tag [vX.Y.Z]"                   'vX.Y.Z'           "$tmp/valid.sha256"

echo "generate_tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
