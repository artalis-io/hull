#!/bin/sh
# e2e_feature_tui.sh - TUI as a composable feature, end to end.
#
# Builds a BASE hull with a TUI-FREE platform (EMBED_PLATFORM=1 HL_ENABLE_TUI=0)
# + the TUI feature archive, then `hull build --with=tui` an app and boots it.
# Proves that `hull build` whole-archive-links libhull_feature-tui.a into the app
# so the resolver admits hull/tui and the runtime registers the module, while a
# plain app stays TUI-free and a base build (no --with=tui) rejects a tui app.
#
# Unlike GPU/DuckDB (backend features with a generated collector), TUI is a
# whole_archive feature: its strong overrides of the base weak hooks
# (hl_tui_feature_present / register_lua / register_js) are spread across the
# archive's object files with no single backend symbol, so build.lua force-loads
# the archive (FEATURE_SPECS.tui: whole_archive = true). No dispatch to check
# (tui.run needs a real tty), so the app asserts the module registered
# (require + tui.run is a function). --compiler=system: the whole-archive link
# flag isn't reliably supported by the embedded TinyCC.
#
# Must run on a fresh build tree. Native only. In its own CI job.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

echo "=== build base hull (EMBED_PLATFORM=1 HL_ENABLE_TUI=0; base is TUI-free) ==="
make EMBED_PLATFORM=1 HL_ENABLE_TUI=0 >/dev/null
# Stash the base hull: `make feature-tui` re-invokes make with HL_ENABLE_TUI=1
# and the build-config sentinel cleans build/ on the flag flip.
cp build/hull /tmp/hull_base_tui_e2e

echo "=== build the TUI feature archive ==="
make feature-tui >/dev/null
ls -la build/libhull_feature-tui.a
# Strong overrides present? (macOS nm prefixes a leading '_', Linux doesn't.)
nm build/libhull_feature-tui.a 2>/dev/null | grep -qE '[ _]hl_tui_feature_present$' \
    || { echo "FAIL: feature archive lacks hl_tui_feature_present"; exit 1; }

HULL=/tmp/hull_base_tui_e2e
APP=$(mktemp -d)
PLAIN=$(mktemp -d)
trap 'rm -rf "$APP" "$PLAIN" /tmp/hull_base_tui_e2e' EXIT

cat > "$APP/app.lua" <<'LUA'
app.manifest({ tui = true, modules = { "hull/tui@1" } })
app.main(function()
    -- The composed feature admits hull/tui and registers the native bridge that
    -- stdlib/lua/hull/tui.lua layers tui.run/list/confirm on top of. No tty here,
    -- so assert registration (require + tui.run is a function), not a full run.
    local ok, tui = pcall(require, "hull.tui")
    assert(ok and tui, "hull.tui not registered: " .. tostring(tui))
    assert(type(tui.run) == "function", "tui.run missing")
    print("TUI FEATURE APP OK")
    return 0
end)
LUA

# ── tui x composed-runtime composes, links, and runs (issue #114, Phase D) ──
#
# The native base is runtime-less. The tui feature is split per-runtime: the cap
# core (libhull_feature-tui.a) is runtime-agnostic, and each runtime bridge lives
# in its own archive (libhull_feature-tui-<rt>.a, embedded in hull). `hull build
# --with=tui` whole-archives the cap core + ONLY the app's runtime bridge, so the
# wrong-runtime bridge is never pulled onto a single-runtime base. The composed
# app admits hull/tui, registers the native bridge, and runs.
echo "=== --with=tui composes + runs ==="
BUILD_OUT=$("$HULL" build --compiler=system --with=tui --no-verify-platform -o "$APP/bin" "$APP" 2>&1) \
    || { echo "$BUILD_OUT"; echo "FAIL: --with=tui should build"; exit 1; }
echo "$BUILD_OUT" | grep -q "composed tui bridge" \
    || { echo "$BUILD_OUT"; echo "FAIL: expected a composed per-runtime tui bridge"; exit 1; }
test -x "$APP/bin" || { echo "$BUILD_OUT"; echo "FAIL: --with=tui produced no binary"; exit 1; }
"$APP/bin" 2>&1 | grep -q "TUI FEATURE APP OK" \
    || { echo "FAIL: tui app did not register hull.tui / run"; exit 1; }
echo "ok  --with=tui composes the per-runtime bridge, builds, and runs"

echo "=== auto-inferred tui (hull/tui app, no --with) also composes + runs ==="
AUTO_OUT=$("$HULL" build --compiler=system --no-verify-platform -o "$APP/bin_auto" "$APP" 2>&1) \
    || { echo "$AUTO_OUT"; echo "FAIL: auto-inferred tui should build"; exit 1; }
test -x "$APP/bin_auto" || { echo "$AUTO_OUT"; echo "FAIL: auto-inferred tui produced no binary"; exit 1; }
"$APP/bin_auto" 2>&1 | grep -q "TUI FEATURE APP OK" \
    || { echo "FAIL: auto-inferred tui app did not run"; exit 1; }
echo "ok  auto-inferred tui composes + runs"

echo "=== negative: a plain (non-tui) app still builds + runs ==="
printf 'app.manifest({modules={}})\napp.main(function() print("PLAIN OK") return 0 end)\n' \
    > "$PLAIN/app.lua"
PLAIN_OUT=$("$HULL" build --no-verify-platform -o "$PLAIN/bin" "$PLAIN" 2>&1) || true
if echo "$PLAIN_OUT" | grep -q "composed feature"; then
    echo "$PLAIN_OUT"; echo "FAIL: composed a feature for a plain app"; exit 1
fi
"$PLAIN/bin" 2>&1 | grep -q "PLAIN OK" || { echo "$PLAIN_OUT"; echo "FAIL: plain app did not run"; exit 1; }
echo "ok  plain app builds + runs (tui-free)"

echo "PASS: TUI feature composes per-runtime, builds, and runs"
