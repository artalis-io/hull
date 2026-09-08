#!/bin/sh
# scripts/cosmo_fat_repair.sh - restore the fat-cosmo pair invariant.
#
# cosmocc builds every translation unit TWICE and writes `foo.o` beside
# `.aarch64/foo.o`; the archives that collect them are paired the same way
# (`libkeel.a` + `.aarch64/libkeel.a`). Only the first of each pair is a make
# target. Nothing in Hull's Makefile - or in Keel's - ever NAMES an
# `.aarch64/` counterpart, so make cannot know one is missing, and an
# interrupted build leaves a half-written pair that every later `make` skips as
# up to date. The build then dies at the fat link with a message that names the
# archive rather than the cause:
#
#   aarch64-unknown-cosmo-ar: src/.aarch64/socket_posix.o: No such file
#   cosmocc: fatal error: vendor/keel/libkeel.a: linker input missing
#            concomitant vendor/keel/.aarch64/libkeel.a file
#
# and it stays dead until someone runs `make clean`.
#
# That is not hypothetical. It is the state the Windows source build inherits
# every time its watchdog kills a wedged cc1 (the HANG section in
# .github/workflows/windows-source-build.yml), which is what stops that job's
# retry-after-stall from working: the retry resumes from the objects already
# built, and one of them is now half a pair. Ctrl-C reproduces it on any host.
#
# The repair is the invariant make cannot express: an x86_64 artifact with no
# aarch64 counterpart is unusable in a fat link, so delete it and let the
# ordinary rules rebuild both halves. The reverse case needs nothing - a
# missing x86_64 artifact IS a make target, so it rebuilds on its own.
#
# Usage: cosmo_fat_repair.sh <dir|file>...
#
#   <dir>   scan recursively for `*.o` whose `.aarch64/` sibling is missing.
#   <file>  check that one path (used for the two paired ARCHIVES, which
#           cannot be found by scanning: build/libhull_platform.a is
#           deliberately single-arch even under cosmocc, so a blanket `*.a`
#           sweep would delete it on every invocation and never stop).
#
# Reports to STDERR and never to stdout: the caller is a parse-time $(shell),
# whose stdout becomes makefile text. Always exits 0 - a tree we could not
# inspect is not a reason to refuse to build.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

# Remove $1 if its `.aarch64/` sibling does not exist. A path with no
# counterpart is reported, once, on the way out.
repair_one() {
    _p=$1
    [ -f "$_p" ] || return 0
    _d=${_p%/*}
    _b=${_p##*/}
    [ "$_d" = "$_p" ] && _d=.
    [ -f "$_d/.aarch64/$_b" ] && return 0
    rm -f "$_p" || return 0
    echo "cosmo-fat-repair: removed half-built $_p (no .aarch64 counterpart)" >&2
}

for arg in "$@"; do
    if [ -d "$arg" ]; then
        # Objects only. `-name '*.o'` deliberately does not match the `.o.d`
        # depfiles written alongside them; an orphaned depfile is harmless
        # (make regenerates it with the object).
        find "$arg" -name '*.o' ! -path '*/.aarch64/*' 2>/dev/null | while IFS= read -r obj; do
            repair_one "$obj"
        done
    elif [ -e "$arg" ]; then
        repair_one "$arg"
    fi
done

exit 0
