#!/bin/sh
# e2e_build_plugin.sh - a build plugin as a Hull application on one channel
# (docs/buildcontext_design.md §9.1), driven by a fake host
# (tests/e2e_build_plugin_drive.c):
#   - the same transform in Lua and in JavaScript drives the host with
#     byte-identical calls (canonical argument encodings) and results, over
#     two actions on one process, the second ending in an error;
#   - a manifest the preset refuses (hull/time) never starts serving;
#   - a frame out of protocol ends the plugin;
#   - run by hand (no channel), app.transform says so;
#   - HULL_BUILD_PLUGIN_FD naming no channel stops hull before app code runs.
set -eu

HULL=${HULL:-build/hull}
DRIVE=${DRIVE:-build/e2e_build_plugin_drive}
FIX=tests/fixtures
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail=0
ok()  { echo "  PASS: $1"; }
bad() { echo "  FAIL: $1"; fail=1; }
has() { grep -qF -- "$2" "$1"; }

echo "== build plugin: Lua and JavaScript parity"
"$DRIVE" "$HULL" "$FIX/build_plugin_lua/app.lua" > "$WORK/lua.txt" 2>&1 || true
"$DRIVE" "$HULL" "$FIX/build_plugin_js/app.js" > "$WORK/js.txt" 2>&1 || true
for rt in lua js; do
    t="$WORK/$rt.txt"
    if has "$t" "written: hi:hello" && has "$t" "done: ok" &&
       has "$t" "done: error unit b fails" && has "$t" "exit: 0" &&
       [ "$(grep -c '^call ' "$t")" -eq 16 ]; then
        ok "$rt: two actions, sixteen calls, the second action's error, clean exit"
    else
        bad "$rt transcript"; cat "$t"
    fi
done
if cmp -s "$WORK/lua.txt" "$WORK/js.txt"; then
    ok "the Lua and JavaScript transcripts are byte-identical"
else
    bad "Lua / JavaScript transcripts differ"; diff "$WORK/lua.txt" "$WORK/js.txt" || true
fi

echo "== build plugin: preset, protocol, no channel"
"$DRIVE" "$HULL" "$FIX/build_plugin_refused/app.lua" none > "$WORK/refused.txt" 2>&1 || true
if has "$WORK/refused.txt" "exit: 1" && ! has "$WORK/refused.txt" "call "; then
    ok "a manifest declaring hull/time is refused before it serves"
else
    bad "refused preset"; cat "$WORK/refused.txt"
fi
for entry in build_plugin_lua/app.lua build_plugin_js/app.js; do
    "$DRIVE" "$HULL" "$FIX/$entry" garbage > "$WORK/garbage.txt" 2>&1 || true
    if has "$WORK/garbage.txt" "exit: 1"; then
        ok "$entry: an unknown frame ends the plugin"
    else
        bad "$entry: garbage frame"; cat "$WORK/garbage.txt"
    fi
    "$HULL" "$FIX/$entry" > "$WORK/hand.txt" 2>&1 || true
    if has "$WORK/hand.txt" "no channel to a build host"; then
        ok "$entry: run by hand, app.transform names the missing channel"
    else
        bad "$entry: run by hand"; cat "$WORK/hand.txt"
    fi
done
HULL_BUILD_PLUGIN_FD=3 "$HULL" "$FIX/build_plugin_lua/app.lua" > "$WORK/fd.txt" 2>&1 || true
if has "$WORK/fd.txt" "fd 3 is not a build channel"; then
    ok "HULL_BUILD_PLUGIN_FD without a channel stops hull"
else
    bad "bad descriptor"; cat "$WORK/fd.txt"
fi

[ "$fail" -eq 0 ] && echo "e2e_build_plugin: OK" || { echo "e2e_build_plugin: FAILED"; exit 1; }
