#!/bin/sh
# tests/check_cosmo_fat_repair.sh - behaviour gate for scripts/cosmo_fat_repair.sh.
#
# The repair runs from a parse-time $(shell) in the Makefile and DELETES build
# artifacts, so two properties are load-bearing and neither is visible from
# reading a build log:
#
#   1. It removes exactly the half-written pairs and nothing else. Over-reach
#      means an artifact that legitimately has no aarch64 counterpart (the
#      single-arch build/libhull_platform.a) gets deleted and rebuilt on every
#      single invocation, which never converges.
#   2. It writes NOTHING to stdout. The caller's stdout becomes makefile text,
#      so one stray line is a syntax error in the middle of the build.
#
# Synthetic trees only: no cosmocc, no compiler, runs in under a second on any
# host. See the script header for what the repair is for.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPAIR="$SCRIPT_DIR/scripts/cosmo_fat_repair.sh"

fails=0
pass() { printf '  ok    %s\n' "$1"; }
fail() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }

check() { # check <description> <condition-result>
    if [ "$2" -eq 0 ]; then pass "$1"; else fail "$1"; fi
}

tmp=$(mktemp -d) || { echo "cannot mktemp"; exit 1; }
trap 'rm -rf "$tmp"' EXIT INT TERM

# ── Fixture ─────────────────────────────────────────────────────────
#   paired.o        + .aarch64/paired.o      -> kept
#   orphan.o        (no counterpart)         -> removed
#   nested/deep.o   (no counterpart)         -> removed (recursive)
#   nested/kept.o   + nested/.aarch64/kept.o -> kept
#   orphan.o.d                               -> kept (depfile, not an object)
#   stray.txt                                -> kept (not an object)
#   .aarch64/lonely.o                        -> kept (never the scan's target)
mkdir -p "$tmp/build/.aarch64" "$tmp/build/nested/.aarch64"
: > "$tmp/build/paired.o";              : > "$tmp/build/.aarch64/paired.o"
: > "$tmp/build/orphan.o";              : > "$tmp/build/orphan.o.d"
: > "$tmp/build/stray.txt"
: > "$tmp/build/.aarch64/lonely.o"
: > "$tmp/build/nested/deep.o"
: > "$tmp/build/nested/kept.o";         : > "$tmp/build/nested/.aarch64/kept.o"

# Paired + unpaired archives, named explicitly (they are never swept).
: > "$tmp/build/libhull.a";             : > "$tmp/build/.aarch64/libhull.a"
mkdir -p "$tmp/keel"
: > "$tmp/keel/libkeel.a"   # no counterpart -> removed
# The single-arch platform archive: passed to NOTHING, and living in a scanned
# directory. It must survive, or every build deletes and rebuilds it forever.
: > "$tmp/build/libhull_platform.a"

out=$(sh "$REPAIR" "$tmp/build" "$tmp/keel" "$tmp/keel/libkeel.a" \
        "$tmp/build/libhull.a" "$tmp/does/not/exist" 2>"$tmp/stderr")
rc=$?

echo "== cosmo_fat_repair =="

check "exits 0" "$rc"
[ -z "$out" ]; check "writes nothing to stdout (the caller is a \$(shell))" $?

[ ! -e "$tmp/build/orphan.o" ];            check "removes an unpaired object" $?
[ ! -e "$tmp/build/nested/deep.o" ];       check "recurses into subdirectories" $?
[ ! -e "$tmp/keel/libkeel.a" ];            check "removes an unpaired named archive" $?

[ -e "$tmp/build/paired.o" ];              check "keeps a paired object" $?
[ -e "$tmp/build/nested/kept.o" ];         check "keeps a paired nested object" $?
[ -e "$tmp/build/libhull.a" ];             check "keeps a paired named archive" $?
[ -e "$tmp/build/libhull_platform.a" ];    check "keeps the single-arch platform archive" $?
[ -e "$tmp/build/orphan.o.d" ];            check "keeps an orphaned depfile" $?
[ -e "$tmp/build/stray.txt" ];             check "keeps a non-object file" $?
[ -e "$tmp/build/.aarch64/lonely.o" ];     check "never removes an aarch64 object" $?

grep -q 'orphan\.o' "$tmp/stderr"; check "names what it removed, on stderr" $?

# Idempotence: a second pass over the repaired tree must be silent and must not
# touch what survived. A repair that keeps finding work is a rebuild loop.
out2=$(sh "$REPAIR" "$tmp/build" "$tmp/keel" "$tmp/build/libhull.a" 2>"$tmp/stderr2")
[ -z "$out2" ] && [ ! -s "$tmp/stderr2" ]; check "is silent on an already-repaired tree" $?
[ -e "$tmp/build/paired.o" ] && [ -e "$tmp/build/libhull_platform.a" ]
check "leaves the survivors alone on a second pass" $?

# No arguments at all is a clean no-op, not a usage error: $(KEEL_LIB) is empty
# in the pure-compute flavor, so the Makefile can legitimately pass fewer paths.
sh "$REPAIR" >/dev/null 2>&1; check "no arguments is a clean no-op" $?

if [ "$fails" -ne 0 ]; then
    echo "cosmo_fat_repair: $fails check(s) failed"
    exit 1
fi
echo "cosmo_fat_repair: all checks passed"
