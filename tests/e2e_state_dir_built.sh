#!/bin/sh
# A ROOTLESS built tool (platform gap 3, PR 3): a built app.main binary whose
# manifest keeps everything under "@state/" (its fs grants, its databases,
# the default included) needs nothing from its working directory, so it runs
# from anywhere - the home directory and / included, which the sandbox
# refuses for any other app. Both runtimes, sandbox on. Needs the platform
# library (hull build); skipped on a cosmo APE and without an embedded /
# built platform lib, like the compiler-free suite.
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
HULL_BIN="${HULL_BIN:-build/hull}"
case "$HULL_BIN" in /*) ;; *) HULL_BIN="$(pwd)/$HULL_BIN" ;; esac
. "$(dirname "$0")/lib/hull_rc.sh"
hull_rc_init "$HULL_BIN"
if hull_is_ape "$HULL_BIN"; then
    echo "SKIP: hull build of a native tool is not available on a cosmo/APE hull"
    exit 0
fi
work=$(mktemp -d)
trap 'chmod -R u+rwx "$work" 2>/dev/null; rm -rf "$work"' EXIT
out="$work/output"
fail() { echo "FAIL: $1"; cat "$out" 2>/dev/null || true; exit 1; }
line() { sed -n "s/^\(.*\[app\] \)\{0,1\}$1=\(.*\)$/\2/p" "$out" | head -1; }
fakehome="$work/home"; mkdir -p "$fakehome"

# One probe build: without a platform library to compose there is nothing to
# test here (CI builds it; `make platform` locally).
mkdir -p "$work/probe-src"; cp tests/fixtures/state_dir_db/app.lua "$work/probe-src/"
probe_rc=0
probe=$("$HULL_BIN" build --no-verify-platform -o "$work/probe" "$work/probe-src" 2>&1) || probe_rc=$?
if [ "$probe_rc" != 0 ]; then
    # A FAILED build naming the platform library is the one thing skipped
    # (the build's log mentions "not embedded" as a note on success too).
    case "$probe" in
        *"platform library not embedded"*|*"cannot find libhull_platform.a"*)
            echo "SKIP: no platform library to compose (make platform, or an EMBED_PLATFORM=1 hull)"
            exit 0 ;;
    esac
    fail "probe build: $probe"
fi
[ -x "$work/probe" ] || fail "probe build produced no binary: $probe"

for ext in lua js; do
    src="$work/src-$ext"; mkdir -p "$src/migrations"
    cp "tests/fixtures/state_dir_db/app.$ext" "$src/"
    cp tests/fixtures/state_dir_db/migrations/001_init.sql "$src/migrations/"
    tool="$work/tool-$ext"
    "$HULL_BIN" build --no-verify-platform -o "$tool" "$src" > "$out" 2>&1 || fail "$ext: hull build"
    xdg="$work/xdg-$ext"

    # From the (fake) home directory: refused for a rooted app, fine here.
    rc=$(cd "$fakehome" && HOME="$fakehome" XDG_STATE_HOME="$xdg" hull_run "$out" "$tool" --hull-no-verify-platform)
    [ "$rc" = 0 ] || fail "$ext rootless from the home directory: exit $rc"
    [ "$(line count)" = 1 ] || fail "$ext rootless: count (got $(line count))"
    [ -f "$xdg/hull-e2e-state-db/data.db" ] || fail "$ext rootless: the database is not under the state dir"
    [ ! -e "$fakehome/data.db" ] || fail "$ext rootless: a data.db was made in the home directory"
    echo "PASS: $ext a rootless built tool runs from the home directory, its state under XDG_STATE_HOME"

    # From /: nothing can be written there, and nothing needs to be.
    rc=$(cd / && HOME="$fakehome" XDG_STATE_HOME="$xdg" hull_run "$out" "$tool" --hull-no-verify-platform)
    [ "$rc" = 0 ] || fail "$ext rootless from /: exit $rc"
    [ "$(line count)" = 2 ] || fail "$ext rootless from /: count (got $(line count))"
    echo "PASS: $ext a rootless built tool runs from /"
done

# The rule stands for every other built app: one that declares no state
# directory is granted its working directory, so / and the home are refused.
for ext in lua js; do
    src="$work/rooted-$ext"; mkdir -p "$src"
    cp "tests/fixtures/cli_interrupt/app.$ext" "$src/"
    tool="$work/rooted-tool-$ext"
    "$HULL_BIN" build --no-verify-platform -o "$tool" "$src" > "$out" 2>&1 || fail "$ext: hull build (rooted)"
    rc=$(cd "$fakehome" && HOME="$fakehome" hull_run "$out" "$tool" --hull-no-verify-platform none)
    [ "$rc" != 0 ] || fail "$ext rooted: a built app ran from the home directory"
    grep -q 'home directory' "$out" || fail "$ext rooted: no refusal for the home directory"
    echo "PASS: $ext a built app without a state directory is still refused from the home directory"
done
echo '6/6 rootless built tool end-to-end tests passed'
