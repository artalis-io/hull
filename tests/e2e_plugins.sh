#!/bin/sh
# e2e_plugins.sh - `hull plugins list|run` and the build plugin demonstration
# (docs/buildcontext_design.md §9, §12 "Single pipeline and plugin parity",
# §13.5), through the real hull binary:
#   - the inventory plugin written in Lua and in JavaScript, each over the
#     same app written in Lua and in JavaScript: all four commit the same
#     artifact bytes and diagnostics;
#   - a second run is served from the build result store; an edit to an
#     annotation changes the selection (a new result), an edit elsewhere in a
#     selected file is caught by the dependency manifest (run again, same
#     bytes); HULL_NO_BUILD_CACHE runs every time;
#   - `hull plugins list` reads a JavaScript plugin's manifest through the
#     extraction child; a refused preset or configuration stops the run;
#   - nothing is written into the app; `hull build`, project discovery and
#     the app's `hull test` leave the plugin out;
#   - `hull test` of the plugin runs its transform against a stub host
#     (test.run_transform / test.runTransform).
set -eu

HULL=${HULL:-build/hull}
case "$HULL" in /*) ;; *) HULL="$PWD/$HULL" ;; esac
FIX="$PWD/tests/fixtures/plugin_demo"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
HOME="$WORK/home"
mkdir -p "$HOME"
export HOME
unset HULL_CACHE_DIR HULL_NO_CACHE HULL_NO_BUILD_CACHE 2>/dev/null || true
fail=0
ok()  { echo "  PASS: $1"; }
bad() { echo "  FAIL: $1"; fail=1; }
has() { grep -qF -- "$2" "$1"; }

CONFIG='{"plugins":[{"path":"plugins/inventory","config":{"title":"Inventory"}}]}'
# make_app APP PLUGIN -> the app directory
make_app() {
    d="$WORK/$1-$2"
    rm -rf "$d"
    mkdir -p "$d/plugins"
    cp -R "$FIX/app_$1/." "$d/"
    cp -R "$FIX/inventory_$2" "$d/plugins/inventory"
    printf '%s\n' "$CONFIG" > "$d/hull.build.json"
    echo "$d"
}
# The outputs and diagnostics of a run (everything but the identity line).
body() { sed 1d "$1"; }

echo "== plugins: four pairings, one result"
for app in lua js; do
    for pl in lua js; do
        d=$(make_app "$app" "$pl")
        (cd "$d" && find . -type f | sort) > "$WORK/before.txt"
        "$HULL" plugins run "$d" > "$WORK/run-$app-$pl.txt" 2>&1 || true
        if has "$WORK/run-$app-$pl.txt" "committed  2 units" &&
           has "$WORK/run-$app-$pl.txt" "artifact inventory  sha256 " &&
           has "$WORK/run-$app-$pl.txt" "info inventory.count: 2 declarations"; then
            ok "$app app, $pl plugin: committed"
        else
            bad "$app app, $pl plugin"; cat "$WORK/run-$app-$pl.txt"
        fi
        (cd "$d" && find . -type f | sort) > "$WORK/after.txt"
        cmp -s "$WORK/before.txt" "$WORK/after.txt" || { bad "$app/$pl: the app changed"; diff "$WORK/before.txt" "$WORK/after.txt" || true; }
    done
done
body "$WORK/run-lua-lua.txt" > "$WORK/ref.txt"
same=1
for r in lua-js js-lua js-js; do
    body "$WORK/run-$r.txt" | cmp -s - "$WORK/ref.txt" || same=0
done
[ "$same" -eq 1 ] && ok "the four results are byte-identical" || { bad "results differ"; cat "$WORK"/run-*.txt; }
want=$(printf 'Inventory\nbye: parting\nhello: greeting\n' | (sha256sum 2>/dev/null || shasum -a 256) | cut -d' ' -f1)
has "$WORK/ref.txt" "sha256 $want" && ok "the artifact is the expected inventory" || { bad "artifact bytes"; cat "$WORK/ref.txt"; }

echo "== plugins: the result store"
d="$WORK/lua-js"
"$HULL" plugins run "$d" > "$WORK/again.txt" 2>&1 || true
has "$WORK/again.txt" "cached  2 units" && ok "a second run is served from the store" || { bad "cache"; cat "$WORK/again.txt"; }
body "$WORK/again.txt" | cmp -s - "$WORK/ref.txt" && ok "with the same outputs" || bad "cached outputs differ"
HULL_NO_BUILD_CACHE=1 "$HULL" plugins run "$d" > "$WORK/nocache.txt" 2>&1 || true
has "$WORK/nocache.txt" "committed  2 units" && ok "HULL_NO_BUILD_CACHE runs it again" || { bad "no cache"; cat "$WORK/nocache.txt"; }
sed 's/return "hello"/return "HELLO"/' "$d/app.lua" > "$d/app.lua.new" && mv "$d/app.lua.new" "$d/app.lua"
"$HULL" plugins run "$d" > "$WORK/body.txt" 2>&1 || true
if has "$WORK/body.txt" "committed  2 units" && body "$WORK/body.txt" | cmp -s - "$WORK/ref.txt"; then
    ok "an edit outside the annotations re-runs (the dependency manifest) with the same result"
else
    bad "dependency edit"; cat "$WORK/body.txt"
fi
printf '\n---@demo extra\nlocal function more() end\n' >> "$d/app.lua"
"$HULL" plugins run "$d" > "$WORK/more.txt" 2>&1 || true
has "$WORK/more.txt" "committed  3 units" && ok "a new annotation changes the selection" || { bad "selection"; cat "$WORK/more.txt"; }

echo "== plugins: list, refusals"
d=$(make_app lua js)
"$HULL" plugins list "$d" > "$WORK/list.txt" 2>&1 || true
if has "$WORK/list.txt" "app:plugins/inventory#" && has "$WORK/list.txt" "javascript, source -> demo-inventory" &&
   has "$WORK/list.txt" "claims: source:lua:demo source:javascript:demo"; then
    ok "list reads a JavaScript plugin's manifest"
else
    bad "list"; cat "$WORK/list.txt"
fi
"$HULL" plugins list "$d" --json > "$WORK/list.json" 2>&1 || true
has "$WORK/list.json" '"identity":"app:plugins/inventory#' && ok "list --json" || { bad "list --json"; cat "$WORK/list.json"; }
printf '%s\n' '{"plugins":[{"path":"plugins/inventory"}]}' > "$d/hull.build.json"
rc=0; "$HULL" plugins run "$d" > "$WORK/noconf.txt" 2>&1 || rc=$?
[ "$rc" -eq 2 ] && has "$WORK/noconf.txt" "config 'title' is required" && ok "a missing required config stops the run" || { bad "config ($rc)"; cat "$WORK/noconf.txt"; }
d=$(make_app lua lua)
sed 's|"hull/build-plugin@1" }|"hull/build-plugin@1", "hull/time@1" }|' "$d/plugins/inventory/app.lua" > "$WORK/p.lua" && cp "$WORK/p.lua" "$d/plugins/inventory/app.lua"
rc=0; "$HULL" plugins run "$d" > "$WORK/preset.txt" 2>&1 || rc=$?
[ "$rc" -eq 2 ] && has "$WORK/preset.txt" "hull/time" && ok "a plugin outside the preset is not registered" || { bad "preset ($rc)"; cat "$WORK/preset.txt"; }
rc=0; "$HULL" plugins frob "$d" > "$WORK/usage.txt" 2>&1 || rc=$?
[ "$rc" -eq 2 ] && has "$WORK/usage.txt" "usage: hull plugins" && ok "an unknown verb is a usage error" || bad "usage ($rc)"

echo "== plugins: hull build leaves the plugin out"
d=$(make_app lua lua)
"$HULL" build "$d" > "$WORK/build.txt" 2>&1 || true
has "$WORK/build.txt" "hull build: 1 Lua file(s) from" && ok "the plugin's sources are not the app's" || { bad "build"; cat "$WORK/build.txt"; }

echo "== plugins: hull test, discovery"
for pl in lua js; do
    d=$(make_app lua "$pl")
    "$HULL" test "$d/plugins/inventory" > "$WORK/ptest.txt" 2>&1 || true
    has "$WORK/ptest.txt" "2/2 tests passed" && ok "$pl plugin: hull test runs the transform against a stub host" || { bad "$pl plugin test"; cat "$WORK/ptest.txt"; }
    "$HULL" test "$d" > "$WORK/atest.txt" 2>&1 || true
    ! has "$WORK/atest.txt" "test_inventory" && ok "$pl plugin: the app's hull test leaves the plugin's tests out" || { bad "$pl app test"; cat "$WORK/atest.txt"; }
done
"$HULL" agent inspect "$d" > "$WORK/inspect.json" 2>&1 || true
if has "$WORK/inspect.json" '"path":"app.lua"' && ! has "$WORK/inspect.json" 'plugins/inventory'; then
    ok "project discovery leaves the plugin's sources out"
else
    bad "discovery"; head -c 2000 "$WORK/inspect.json"
fi

[ "$fail" -eq 0 ] && echo "e2e_plugins: OK" || { echo "e2e_plugins: FAILED"; exit 1; }
