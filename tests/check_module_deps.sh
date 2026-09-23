#!/bin/sh
# check_module_deps.sh - gate: a stdlib module declares every first-party
# module it requires.
#
# The resolver admits a module's DEPS automatically, so an app that declares
# `hull/ssh@1` never has to name what SSH is built from. That only works if the
# registry says what a module is built from. When it does not, the app loads,
# the require runs, and the gate refuses it at the worst possible moment:
#
#   module 'hull.crypto' is not declared in app.manifest.
#
# That shipped. `hull/ssh` reached for `hull.crypto` in its key exchange, its
# host-key check and its userauth signature, listed no deps at all, and no test
# noticed for four releases - because every SSH suite passes `opts.crypto`, the
# injection seam that exists precisely so the protocol can be driven without
# the capability layer. A seam a test always fills is a seam no test checks.
#
# So this compares the two things that must agree, mechanically:
#
#   what a module REQUIRES   grep over its own sources
#   what a module DECLARES   the .deps rows in src/hull/module_registry.c
#
# A direct require must be reachable through the declared deps (transitively -
# the resolver admits the whole closure, so naming a dep of a dep is optional).
#
# Scope: first-party stdlib only, both runtimes. It reads sources as TEXT and
# does not run anything, so it needs no build and no hull binary.
#
# Known and deliberate limits, because a gate that overstates itself is worse
# than one that does not exist:
#   - a require on a line that begins with a comment marker is skipped, but one
#     trailing live code is not; a commented-out require mid-line would be a
#     false positive, and none exists today.
#   - a computed require (`require("hull." .. name)`) is invisible. Nothing in
#     the stdlib does that, and a gate cannot resolve it without running it.
#   - names that are not registry modules (`hull.ssh.transport`,
#     `hull.encoding.base64`) are ignored: they are internal files, not modules
#     an app can declare.
#
# Usage: sh tests/check_module_deps.sh
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
REGISTRY="$SRCDIR/src/hull/module_registry.c"
WORK="$(mktemp -d 2>/dev/null || mktemp -d -t hulldeps)"
trap 'rm -rf "$WORK"' EXIT INT TERM

[ -f "$REGISTRY" ] || { echo "check-module-deps: no $REGISTRY"; exit 1; }

# ── 1. The registry: "<name>\t<dep> <dep> ..." per module ───────────────
#
# Parsed as text rather than by running `hull modules available`, so the gate
# holds on a tree that has not been built - which is when a dep mistake is
# cheapest to find.
awk '
    /^[ \t]*\.name[ \t]*=[ \t]*"/ {
        line = $0
        sub(/^[^"]*"/, "", line); sub(/".*$/, "", line)
        name = line; deps = ""; next
    }
    /^[ 	]*\.deps[ 	]*=/ { collecting = 1 }
    collecting {
        if (name == "") { collecting = 0; next }
        line = $0
        # Every "quoted" entry is a dep; the trailing 0 is not. A .deps
        # array WRAPS once it is long enough, so accumulate until the
        # closing brace - reading only the first line silently truncated
        # the longest rows, which are exactly the ones worth checking.
        while (match(line, /"[^"]+"/)) {
            d = substr(line, RSTART + 1, RLENGTH - 2)
            deps = deps (deps == "" ? "" : " ") d
            line = substr(line, RSTART + RLENGTH)
        }
        if ($0 ~ /}/) {
            print name "	" deps
            name = ""; deps = ""; collecting = 0
        }
    }
' "$REGISTRY" > "$WORK/deps.tsv"

MODCOUNT=$(wc -l < "$WORK/deps.tsv" | tr -d ' ')
if [ "$MODCOUNT" -lt 20 ]; then
    echo "check-module-deps: parsed only $MODCOUNT registry modules - the table"
    echo "  shape must have changed. Fix this parser rather than lowering the bar."
    exit 1
fi

cut -f1 "$WORK/deps.tsv" | sort > "$WORK/modules.txt"

is_module() { grep -qxF "$1" "$WORK/modules.txt"; }

direct_deps() { awk -F'\t' -v m="$1" '$1 == m { print $2 }' "$WORK/deps.tsv"; }

# Transitive closure of a module's declared deps. The resolver admits the whole
# closure, so a module may reach hull/json through hull/jwt without naming it.
closure() {
    c_seen=""
    c_queue="$(direct_deps "$1")"
    while [ -n "$c_queue" ]; do
        c_next=""
        for c_d in $c_queue; do
            case " $c_seen " in *" $c_d "*) continue ;; esac
            c_seen="$c_seen $c_d"
            c_next="$c_next $(direct_deps "$c_d")"
        done
        c_queue="$c_next"
    done
    echo "$c_seen"
}

# ── 2. Which module owns a source file ──────────────────────────────────
#
# stdlib/lua/hull/web/middleware/cors.lua -> hull/web/middleware/cors
# stdlib/lua/hull/ssh/transport.lua       -> hull/ssh  (walk up to a module)
owner_of() {
    o_rel="${1#"$SRCDIR"/}"
    o_rel="${o_rel#stdlib/lua/}"
    o_rel="${o_rel#stdlib/js/}"
    o_rel="${o_rel%.lua}"
    o_rel="${o_rel%.js}"
    while [ -n "$o_rel" ] && [ "$o_rel" != "." ]; do
        if is_module "$o_rel"; then echo "$o_rel"; return 0; fi
        case "$o_rel" in */*) o_rel="${o_rel%/*}" ;; *) break ;; esac
    done
    return 1
}

# ── 3. Compare ──────────────────────────────────────────────────────────
FAILED=0
CHECKED=0
PAIRS=0

for f in $(find "$SRCDIR/stdlib/lua/hull" "$SRCDIR/stdlib/js/hull" \
                -name '*.lua' -o -name '*.js' 2>/dev/null \
           | grep -v '/tests/' | sort); do
    owner=$(owner_of "$f") || continue
    CHECKED=$((CHECKED + 1))

    # require("hull.x.y") / require('hull.x') / from "hull:x:y"
    reqs=$(grep -v '^[[:space:]]*--' "$f" 2>/dev/null \
           | grep -v '^[[:space:]]*//' \
           | grep -oE '(require\(["'"'"']hull[.][A-Za-z0-9_.-]+|from[[:space:]]+["'"'"']hull:[A-Za-z0-9_:-]+)' \
           | sed -e 's/.*hull[.:]//' -e 's/[.:]/\//g' \
           | sed 's/^/hull\//' | sort -u)

    for r in $reqs; do
        is_module "$r" || continue          # an internal file, not a module
        [ "$r" = "$owner" ] && continue     # a module's own submodule
        PAIRS=$((PAIRS + 1))
        case " $(closure "$owner") " in
            *" $r "*) ;;
            *)
                echo "  $owner requires $r but does not declare it"
                echo "      seen in: ${f#"$SRCDIR"/}"
                FAILED=$((FAILED + 1))
                ;;
        esac
    done
done

if [ "$PAIRS" -eq 0 ]; then
    echo "check-module-deps: found no require pairs at all across $CHECKED files."
    echo "  The extraction must have broken - a passing run that checks nothing"
    echo "  is the failure this guard exists for."
    exit 1
fi

if [ "$FAILED" -ne 0 ]; then
    echo ""
    echo "check-module-deps: FAIL - $FAILED undeclared dependency/ies."
    echo "  Add the module to that row's .deps in src/hull/module_registry.c."
    echo "  The resolver admits deps transitively, so an app declaring the outer"
    echo "  module then gets them without naming each one."
    exit 1
fi

echo "check-module-deps: OK - $CHECKED stdlib files, $PAIRS module-to-module"
echo "  requires, every one declared (or reachable through a declared dep)."
