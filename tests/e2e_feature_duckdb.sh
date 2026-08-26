#!/bin/sh
# e2e_feature_duckdb.sh - DuckDB as a composable feature, end to end.
#
# Builds a BASE hull (EMBED_PLATFORM=1, no DuckDB compiled in) + the DuckDB
# feature archive, then `hull build --with=duckdb` an app and runs it. Proves
# that `hull build` composes the feature lib + a generated registry into the app
# binary so db_select routes duckdb:// to the composed backend -- while the base
# stays DuckDB-free. A plain app must NOT get the feature. See
# docs/features_and_flavors.md.
#
# Must run on a fresh build tree (no prior HL_ENABLE_DUCKDB=1 objects), so it
# lives in its own CI job. Requires `make fetch-duckdb` to have run.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

echo "=== build base hull (EMBED_PLATFORM=1; base is DuckDB-free) ==="
make EMBED_PLATFORM=1 >/dev/null
# Stash the base hull: `make feature-duckdb` re-invokes make with
# HL_ENABLE_DUCKDB=1, and the build-config sentinel cleans build/ on the flag
# flip (EMBED_PLATFORM is fingerprinted since #179), which would wipe build/hull.
# Save the build-tool binary first so it survives the feature-archive build
# (mirrors e2e_feature_gpu.sh).
cp build/hull /tmp/hull_base_duckdb_e2e

echo "=== build the DuckDB feature archive ==="
make feature-duckdb >/dev/null
ls -la build/libhull_feature-duckdb.a

HULL=/tmp/hull_base_duckdb_e2e
APP=$(mktemp -d)
PLAIN=$(mktemp -d)
trap 'rm -rf "$APP" "$PLAIN" /tmp/hull_base_duckdb_e2e' EXIT

cat > "$APP/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/db@1" },
    databases = { named = { analytics = "duckdb://:memory:" } },
})
app.main(function()
    local d = require("hull.db").connect("analytics")
    local r = d.query("SELECT 42 AS answer")
    assert(r and r[1] and r[1].answer == 42, "duckdb query failed")
    print("DUCKDB FEATURE APP OK")
    return 0
end)
LUA

echo "=== hull build --with=duckdb (system compiler: features are C++) ==="
BUILD_OUT=$("$HULL" build --compiler=system --with=duckdb --no-verify-platform -o "$APP/bin" "$APP" 2>&1) || true
echo "$BUILD_OUT"
echo "$BUILD_OUT" | grep -q "composed feature 'duckdb'" || { echo "FAIL: feature not composed"; exit 1; }
RUN_OUT=$("$APP/bin" 2>&1) || true
echo "$RUN_OUT"
if ! echo "$RUN_OUT" | grep -q "DUCKDB FEATURE APP OK"; then
    echo "--- app run failed; diagnostics ---"
    file "$APP/bin" 2>/dev/null || true
    command -v ldd >/dev/null 2>&1 && ldd "$APP/bin" 2>&1 | head || true
    echo "--- retry with --no-sandbox (isolates sandbox vs link/static-init) ---"
    if "$APP/bin" --no-sandbox 2>&1 | grep -q "DUCKDB FEATURE APP OK"; then
        echo "DIAG: works with --no-sandbox -> a sandboxed syscall blocks DuckDB init"
        if command -v strace >/dev/null 2>&1; then
            echo "--- strace (sandboxed run): syscalls that returned an error ---"
            strace -f -o /tmp/ddb_strace.txt "$APP/bin" >/dev/null 2>&1 || true
            grep -aE "= -1 E" /tmp/ddb_strace.txt | grep -av readlink | tail -25 || true
        fi
    else
        echo "DIAG: still fails with --no-sandbox -> link/static-init, not sandbox"
    fi
    echo "FAIL: app did not run duckdb"; exit 1
fi
echo "ok  --with=duckdb composed + ran"

echo "=== negative: a plain sqlite app must NOT compose duckdb ==="
printf 'app.manifest({modules={"hull/db@1"}})\napp.main(function() print("PLAIN OK") return 0 end)\n' \
    > "$PLAIN/app.lua"
PLAIN_OUT=$("$HULL" build --no-verify-platform -o "$PLAIN/bin" "$PLAIN" 2>&1) || true
if echo "$PLAIN_OUT" | grep -q "composed feature"; then
    echo "$PLAIN_OUT"; echo "FAIL: composed a feature for a plain app"; exit 1
fi
"$PLAIN/bin" 2>&1 | grep -q "PLAIN OK" || { echo "FAIL: plain app did not run"; exit 1; }
echo "ok  plain app stayed DuckDB-free"

echo "=== negative: tcc cannot link a C++ feature (must be rejected) ==="
# On Linux the guard fires; on macOS tcc is unavailable -- either way, no binary.
if "$HULL" build --compiler=tcc --with=duckdb --no-verify-platform -o "$APP/bin_tcc" "$APP" >/dev/null 2>&1; then
    echo "FAIL: tcc produced a C++ feature binary (should be rejected)"; exit 1
fi
echo "ok  tcc + feature rejected"

echo "PASS: DuckDB feature composed into an app binary; base stays clean"
