#!/bin/sh
# tests/check_dry_run_is_inert.sh - `make -n` must describe, never delete.
#
# WHY THIS EXISTS. Two parse-time $(shell ...) hooks in the Makefile DELETE
# build artifacts: the build-fingerprint purge (drop every Hull .o when the
# configuration changed) and the fat-cosmo pair repair. $(shell) runs while
# make PARSES, and -n does not suppress parsing - so a dry run performed part
# of a build.
#
# That cost a working tree. tests/check_keel_flag_propagation.sh calls itself
# "dry run, no artifacts" and shells `make -n <vars> vendor/keel/libkeel.a`
# with variables that differ from whatever the developer last built with. The
# fingerprint mismatch fired the purge, so running the LINT gate deleted 273
# objects and build/hull.
#
# WHAT IS ACTUALLY FRAGILE is the detection, not the wiring: GNU make reports
# -n through MAKEFLAGS in a shape that is easy to get subtly wrong (short
# options collected into the first word WITHOUT a dash, long options as
# separate dash-prefixed words - so a naive `findstring n` reads
# --no-print-directory as a dry run). So the detection block is lifted OUT of
# the real Makefile and exercised directly, against a marker file in a temp
# directory where a regression can destroy nothing.
#
# The wiring is then checked two ways. On a tree with no build objects - a
# fresh checkout, which is what CI lints - the real Makefile is dry-run with a
# deliberately mismatched fingerprint and must leave a planted marker alone.
# On a tree that HAS objects the same run would delete them if the guard were
# broken, so that case is asserted structurally instead, and says so.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu
cd "$(dirname "$0")/.."

MAKE="${MAKE:-make}"
FAILED=0
pass() { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; FAILED=$((FAILED + 1)); }
skip() { printf '  skip  %s\n' "$1"; }

echo "check-dry-run-inert: asserting a dry run deletes nothing"

# ── 1. The detection idiom, in isolation ────────────────────────────
#
# Lifted from the Makefile rather than restated, so a change to the idiom is
# covered instead of shadowed by a stale copy.
tmp=$(mktemp -d) || { echo "cannot mktemp" >&2; exit 1; }
trap 'rm -rf "$tmp"' EXIT INT TERM

sed -n '/^HL_MAKE_SHORT_OPTS :=/,/^HL_TREE_MUST_NOT_CHANGE :=/p' Makefile > "$tmp/detect.mk"
if ! grep -q 'HL_TREE_MUST_NOT_CHANGE :=' "$tmp/detect.mk"; then
    bad "cannot find the HL_TREE_MUST_NOT_CHANGE detection block in Makefile"
    echo "check-dry-run-inert: 1 failure(s)" >&2
    exit 1
fi
pass "detection block extracted from Makefile"

{
    cat "$tmp/detect.mk"
    printf 'ifeq ($(HL_TREE_MUST_NOT_CHANGE),)\n'
    printf '$(shell rm -f %s/marker)\n' "$tmp"
    printf 'endif\n'
    printf 'probe check-anything lint check-hardening:\n\t@echo goal\n'
} > "$tmp/Makefile"

# $1 = description, $2 = expected marker state after the run (gone|kept),
# rest = make flags.
detect_case() {
    desc=$1; want=$2; shift 2
    : > "$tmp/marker"
    ( cd "$tmp" && $MAKE "$@" >/dev/null 2>&1 ) || true
    if [ -e "$tmp/marker" ]; then got=kept; else got=gone; fi
    if [ "$got" = "$want" ]; then pass "$desc"; else bad "$desc (marker $got, wanted $want)"; fi
}

detect_case "a real build runs the hook"                 gone probe
detect_case "-n does not"                                kept -n probe
detect_case "-Bn does not"                               kept -Bn probe
detect_case "-n with a variable does not"                kept -n FOO=bar probe
# The trap for a naive `findstring n`: a long option that contains an 'n' but
# is not -n. This must still be treated as a real build.
detect_case "--no-print-directory is not a dry run"      gone --no-print-directory probe

# The second reason to stay inert: a goal that builds nothing. A gate is
# normally run WITHOUT the flags the tree was built with, so letting the purge
# fire there deletes the build it was asked only to inspect.
detect_case "a check-* goal builds nothing, so stays inert" kept check-anything
detect_case "lint builds nothing either"                     kept lint
# check-hardening is the exception: it depends on build/hull, so it DOES build
# and must keep the purge.
detect_case "check-hardening does build, so acts"           gone check-hardening

# ── 2. The wiring, in the real Makefile ─────────────────────────────
objs=$(ls build/*.o 2>/dev/null | wc -l | tr -d ' ')
if [ "${objs:-0}" -eq 0 ]; then
    # Safe to exercise for real: there is nothing a broken guard could destroy.
    mkdir -p build
    saved=""
    if [ -f build/.build-config ]; then
        saved=$(cat build/.build-config)
    fi
    printf 'a-configuration-this-tree-was-never-built-with\n' > build/.build-config
    : > build/cap_zzdryrunprobe.o
    $MAKE -n HL_OPT=-O0 HL_ENABLE_LTO=1 probe-nonexistent-target >/dev/null 2>&1 || true
    if [ -e build/cap_zzdryrunprobe.o ]; then
        pass "the real Makefile's purge does not fire under -n"
    else
        bad "the real Makefile's purge DELETED a build object under -n"
    fi
    rm -f build/cap_zzdryrunprobe.o
    if [ -n "$saved" ]; then
        printf '%s\n' "$saved" > build/.build-config
    else
        rm -f build/.build-config
    fi
else
    # Running the real check here would delete this tree's objects if the guard
    # were broken - the very damage being gated. Assert the shape instead.
    hooks=$(grep -c '^\$(shell ' Makefile || true)
    guarded=$(awk '/^ifeq \(\$\(HL_TREE_MUST_NOT_CHANGE\),\)/{g=1} /^endif$/{g=0} /^\$\(shell /{if(g)c++} END{print c+0}' Makefile)
    if [ "$hooks" -gt 0 ] && [ "$hooks" = "$guarded" ]; then
        pass "all $hooks parse-time \$(shell) hooks sit inside the inertness guard"
    else
        bad "$((hooks - guarded)) of $hooks parse-time \$(shell) hooks are OUTSIDE the guard"
    fi
    skip "live purge check ($objs build objects present; a regression would delete them)"
fi

if [ "$FAILED" -eq 0 ]; then
    echo "check-dry-run-inert: OK"
    exit 0
fi
echo "check-dry-run-inert: $FAILED failure(s)" >&2
exit 1
