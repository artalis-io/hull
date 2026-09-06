#!/bin/sh
# E2E test for the install script and shell completions.
#
# - install.sh: dry-run mode hits every code path (platform detection,
#   asset selection, version resolution, prefix resolution) without
#   actually downloading.
# - completions: syntax-checked in their respective shells; behaviour
#   spot-checked for bash.
#
# Usage: sh tests/e2e_install.sh
#        make e2e-install
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
PASS=0
FAIL=0

assert() {
    msg="$1"; shift
    if "$@"; then
        echo "  ok  $msg"
        PASS=$((PASS + 1))
    else
        echo "  FAIL $msg"
        FAIL=$((FAIL + 1))
    fi
}

assert_contains() {
    msg="$1"; haystack="$2"; needle="$3"
    if echo "$haystack" | grep -qF -- "$needle"; then
        echo "  ok  $msg"
        PASS=$((PASS + 1))
    else
        echo "  FAIL $msg"
        echo "    expected to contain: $needle"
        echo "    got:"
        echo "$haystack" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
}

echo "── install.sh ──"

# Dry-run with explicit version (skips the GitHub API call)
OUT=$(HULL_VERSION=v0.0.1 HULL_DRY_RUN=1 sh "$SRCDIR/install.sh" 2>&1)

assert_contains "detects platform"  "$OUT" "platform:"
assert_contains "selects asset"     "$OUT" "asset:"
assert_contains "shows version"     "$OUT" "version:   v0.0.1"
assert_contains "shows prefix"      "$OUT" "prefix:"
assert_contains "shows dry-run mode" "$OUT" "dry-run mode"
assert_contains "would download"    "$OUT" "downloading hull"
assert_contains "would install"     "$OUT" "installing to"

# Explicit flavor switching
OUT_COSMO=$(HULL_VERSION=v0.0.1 HULL_FLAVOR=cosmo HULL_DRY_RUN=1 sh "$SRCDIR/install.sh" 2>&1)
assert_contains "HULL_FLAVOR=cosmo selects hull-cosmo" "$OUT_COSMO" "asset:     hull-cosmo"

# Prefix override
OUT_PREFIX=$(HULL_VERSION=v0.0.1 HULL_PREFIX=/opt/hull/bin HULL_DRY_RUN=1 sh "$SRCDIR/install.sh" 2>&1)
assert_contains "HULL_PREFIX override" "$OUT_PREFIX" "prefix:    /opt/hull/bin"

# No releases yet: latest should fail cleanly
OUT_NORELEASE=$(HULL_REPO=artalis-io/hull-nonexistent-test sh "$SRCDIR/install.sh" 2>&1 || true)
assert_contains "no-release error message" "$OUT_NORELEASE" "could not find any release"

echo ""
echo "── shell completions ──"

# Bash syntax
assert "bash completion syntax" bash -n "$SRCDIR/completions/hull.bash"

# Zsh syntax (skip if zsh not installed)
if command -v zsh >/dev/null 2>&1; then
    assert "zsh completion syntax" zsh -n "$SRCDIR/completions/_hull"
else
    echo "  skip zsh not installed"
fi

# Fish syntax (skip if fish not installed)
if command -v fish >/dev/null 2>&1; then
    assert "fish completion syntax" fish -n "$SRCDIR/completions/hull.fish"
else
    echo "  skip fish not installed"
fi

# Bash behavior: complete a few representative inputs
BASH_OUT=$(bash -c '
    source '"$SRCDIR"'/completions/hull.bash
    # "hull bu<TAB>"
    COMP_WORDS=(hull bu); COMP_CWORD=1; _hull; echo "bu=${COMPREPLY[*]}"
    # "hull build --com<TAB>"
    COMP_WORDS=(hull build --com); COMP_CWORD=2; _hull; echo "com=${COMPREPLY[*]}"
    # "hull build --compiler=t<TAB>"
    COMP_WORDS=(hull build --compiler=t); COMP_CWORD=2; _hull; echo "ctcc=${COMPREPLY[*]}"
    # "hull agent <TAB>"
    COMP_WORDS=(hull agent ""); COMP_CWORD=2; _hull; echo "agent=${COMPREPLY[*]}"
    # "hull deploy <TAB>"
    COMP_WORDS=(hull deploy ""); COMP_CWORD=2; _hull; echo "deploy=${COMPREPLY[*]}"
    # "hull doctor <TAB>" - doctor grew --tui/--fix and split from version
    COMP_WORDS=(hull doctor ""); COMP_CWORD=2; _hull; echo "doctor=${COMPREPLY[*]}"
    COMP_WORDS=(hull version ""); COMP_CWORD=2; _hull; echo "version=${COMPREPLY[*]}"
')

assert_contains "complete \"hull bu\" → build"            "$BASH_OUT" "bu=build"
assert_contains "complete \"hull build --com\" → --compiler" "$BASH_OUT" "com=--compiler"
assert_contains "complete \"hull build --compiler=t\" → tcc" "$BASH_OUT" "ctcc=tcc"
assert_contains "complete \"hull agent \" → subcommands"   "$BASH_OUT" "agent=routes db request status errors test context migrate deploy"
assert_contains "complete \"hull deploy \" → targets"      "$BASH_OUT" "deploy=dockerfile systemd fly"
assert_contains "complete \"hull doctor \" → --json --tui --fix" "$BASH_OUT" "doctor=--json --tui --fix"
assert_contains "complete \"hull version \" → --json only"       "$BASH_OUT" "version=--json"

echo ""
echo "── Summary ──"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
