#!/bin/sh
# e2e_feature_duckdb.sh — DuckDB as a composable feature, end to end.
#
# Builds a BASE hull (EMBED_PLATFORM=1, no DuckDB compiled in) + the DuckDB
# feature archive, then `hull build` a duckdb:// app two ways (--with=duckdb and
# manifest-inferred) and runs each binary. Proves that `hull build` composes the
# feature lib + a generated registry into the app binary so db_select routes
# duckdb:// to the composed backend -- while the base stays DuckDB-free. A plain
# app must NOT get the feature. See docs/features_and_flavors.md.
#
# Must run on a fresh build tree (no prior HL_ENABLE_DUCKDB=1 objects), so it
# lives in its own CI job. Requires `make fetch-duckdb` to have run.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

echo "=== build base hull (EMBED_PLATFORM=1; base is DuckDB-free) ==="
make EMBED_PLATFORM=1 >/dev/null
echo "=== build the DuckDB feature archive ==="
make feature-duckdb >/dev/null

HULL=./build/hull
APP=$(mktemp -d)
trap 'rm -rf "$APP"' EXIT

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

echo "=== hull build --with=duckdb (explicit) ==="
"$HULL" build --with=duckdb --no-verify-platform -o "$APP/bin" "$APP" 2>&1 \
    | grep -q "composed feature 'duckdb'" || { echo "FAIL: feature not composed"; exit 1; }
"$APP/bin" 2>&1 | grep -q "DUCKDB FEATURE APP OK" || { echo "FAIL: app did not run duckdb"; exit 1; }
echo "ok  explicit --with=duckdb composed + ran"

echo "=== manifest-inferred (duckdb:// in databases, no --with) ==="
"$HULL" build --no-verify-platform -o "$APP/bin2" "$APP" 2>&1 \
    | grep -q "composed feature 'duckdb'" || { echo "FAIL: not inferred from manifest"; exit 1; }
"$APP/bin2" 2>&1 | grep -q "DUCKDB FEATURE APP OK" || { echo "FAIL: inferred app did not run"; exit 1; }
echo "ok  manifest-inferred composed + ran"

echo "=== negative: a plain sqlite app must NOT compose duckdb ==="
printf 'app.manifest({modules={"hull/db@1"}})\napp.main(function() print("PLAIN OK") return 0 end)\n' \
    > "$APP/plain.lua"
PLAIN=$(mktemp -d); trap 'rm -rf "$APP" "$PLAIN"' EXIT
cp "$APP/plain.lua" "$PLAIN/app.lua"
if "$HULL" build --no-verify-platform -o "$PLAIN/bin" "$PLAIN" 2>&1 | grep -q "composed feature"; then
    echo "FAIL: composed a feature for a plain app"; exit 1
fi
"$PLAIN/bin" 2>&1 | grep -q "PLAIN OK" || { echo "FAIL: plain app did not run"; exit 1; }
echo "ok  plain app stayed DuckDB-free"

echo "PASS: DuckDB feature composed into app binaries (explicit + inferred), base stays clean"
