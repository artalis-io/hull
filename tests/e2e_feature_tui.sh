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

echo "=== hull build --with=tui ==="
BUILD_OUT=$("$HULL" build --compiler=system --with=tui --no-verify-platform -o "$APP/bin" "$APP" 2>&1) || true
echo "$BUILD_OUT"
echo "$BUILD_OUT" | grep -q "composed feature 'tui'" || { echo "FAIL: feature not composed"; exit 1; }
test -x "$APP/bin" || { echo "FAIL: no composed binary produced"; exit 1; }

echo "=== boot the composed binary ==="
RUN_OUT=$("$APP/bin" 2>&1) || true
echo "$RUN_OUT"
echo "$RUN_OUT" | grep -q "TUI FEATURE APP OK" || {
    echo "--- boot failed; diagnostics ---"
    file "$APP/bin" 2>/dev/null || true
    echo "FAIL: composed tui binary did not boot"; exit 1
}
echo "ok  --with=tui composed + booted"

echo "=== auto-compose: hull build (NO --with=tui) on a hull/tui app ==="
# TUI migrated from a base-builtin to a composable feature, so a stock hull must
# still build a hull/tui app transparently: build.lua infers --with=tui from the
# manifest's hull/tui declaration (skipped only when the base already has TUI, ie
# a monolithic HL_ENABLE_TUI build or cosmo). This is the primary UX; --with=tui
# above is the explicit superset.
AUTO_OUT=$("$HULL" build --compiler=system --no-verify-platform -o "$APP/bin_auto" "$APP" 2>&1) || true
echo "$AUTO_OUT" | grep -q "composing feature 'tui'" \
    || { echo "$AUTO_OUT"; echo "FAIL: did not infer tui from hull/tui declaration"; exit 1; }
test -x "$APP/bin_auto" || { echo "FAIL: no auto-composed binary"; exit 1; }
"$APP/bin_auto" 2>&1 | grep -q "TUI FEATURE APP OK" \
    || { echo "FAIL: auto-composed tui binary did not boot"; exit 1; }
echo "ok  auto-composed (no --with=tui) + booted"

echo "=== negative: a plain app must NOT compose tui ==="
printf 'app.manifest({modules={}})\napp.main(function() print("PLAIN OK") return 0 end)\n' \
    > "$PLAIN/app.lua"
PLAIN_OUT=$("$HULL" build --no-verify-platform -o "$PLAIN/bin" "$PLAIN" 2>&1) || true
if echo "$PLAIN_OUT" | grep -q "composed feature"; then
    echo "$PLAIN_OUT"; echo "FAIL: composed a feature for a plain app"; exit 1
fi
"$PLAIN/bin" 2>&1 | grep -q "PLAIN OK" || { echo "FAIL: plain app did not run"; exit 1; }
echo "ok  plain app stayed TUI-free"

echo "=== not-installed: a required hull/tui with no feature archive errors clearly ==="
# Auto-inference adds --with=tui for a declared hull/tui, but if the feature
# archive is neither local nor installed, the build must fail with a
# manifest-framed message (not a bare --with resolution miss) and produce no
# broken binary. Isolate: stage the hull where no archive sits beside it, run
# from an empty CWD (so the relative build/ + ../build/ search paths miss), and
# point HOME at an empty dir (no ~/.hull/feature).
STAGE=$(mktemp -d); FAKEHOME=$(mktemp -d); NOTINST=$(mktemp -d)
trap 'rm -rf "$APP" "$PLAIN" "$STAGE" "$FAKEHOME" "$NOTINST" /tmp/hull_base_tui_e2e' EXIT
cp "$HULL" "$STAGE/hull"
cp "$APP/app.lua" "$NOTINST/app.lua"   # same required hull/tui app
NI_OUT=$(cd "$STAGE" && HOME="$FAKEHOME" "$STAGE/hull" build --compiler=system \
    --no-verify-platform -o "$NOTINST/bin" "$NOTINST" 2>&1) || true
echo "$NI_OUT" | grep -q "declares hull/tui but the 'tui' feature is not installed" \
    || { echo "$NI_OUT"; echo "FAIL: missing manifest-framed not-installed error"; exit 1; }
echo "$NI_OUT" | grep -q "hull feature install tui" \
    || { echo "FAIL: not-installed error lacks install hint"; exit 1; }
test -x "$NOTINST/bin" && { echo "FAIL: produced a binary despite missing feature"; exit 1; }
echo "ok  not-installed required hull/tui failed clearly, no broken binary"

# NOTE: the "base build (no feature) rejects a tui app at load" case is covered
# deterministically by the module_resolver unit tests
# (tui_rejected_when_compiled_out). The not-installed build-time case above is
# the buildable-but-unresolved variant.

echo "PASS: TUI feature composed into an app binary; base stays TUI-free"
