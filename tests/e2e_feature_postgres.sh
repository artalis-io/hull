#!/bin/sh
# e2e_feature_postgres.sh — PostgreSQL as a composable feature, end to end.
#
# Builds a BASE hull (EMBED_PLATFORM=1, no Postgres compiled in) + the Postgres
# feature archive, then `hull build --with=postgres` an app and boots it. Proves
# that `hull build` composes libhull_feature-postgres.a + a generated registry
# (filling the hl_db_feature_backends hook, like DuckDB) into the app binary, so
# a base-built hull gains the postgres:// backend purely from the feature.
#
# This is a BUILD-only e2e (no live server): the app connects to an unreachable
# host and asserts the error is a CONNECTION failure, not a "needs the Postgres
# feature" scheme rejection — i.e. the backend is registered and live. A real
# connect + auth + query is covered by the monolithic e2e_postgres.sh job.
#
# The point of running this on Linux: it exercises the GNU-ld --start-group
# compose link, which the pg backend needs because its archive references base
# symbols (tls_client for sslmode) that a DB-only app doesn't otherwise pull.
# --compiler=system: the whole build links through the system cc.
#
# Must run on a fresh build tree (no prior HL_ENABLE_POSTGRES=1 objects); its own
# CI job. Native only.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

echo "=== build base hull (EMBED_PLATFORM=1; base has no postgres backend) ==="
make EMBED_PLATFORM=1 >/dev/null
# Stash the base hull: `make feature-postgres` re-invokes make with
# HL_ENABLE_POSTGRES=1 and the build-config sentinel cleans build/ on the flip.
cp build/hull /tmp/hull_base_pg_e2e

echo "=== build the Postgres feature archive ==="
make feature-postgres >/dev/null
ls -la build/libhull_feature-postgres.a
nm build/libhull_feature-postgres.a 2>/dev/null | grep -qE '[ _]hl_db_backend_postgres$' \
    || { echo "FAIL: feature archive lacks hl_db_backend_postgres"; exit 1; }

HULL=/tmp/hull_base_pg_e2e
APP=$(mktemp -d)
PLAIN=$(mktemp -d)
trap 'rm -rf "$APP" "$PLAIN" /tmp/hull_base_pg_e2e' EXIT

cat > "$APP/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/db@1" },
    -- Unreachable host + short timeout: the connect must FAIL, and the failure
    -- proves the backend routed postgres:// (a scheme rejection would fire first).
    databases = { named = { pg = "postgres://u:p@10.255.255.1:5432/db?connect_timeout=1" } },
})
app.main(function()
    local ok, err = pcall(function()
        require("hull.db").connect("pg").query("SELECT 1")
    end)
    assert(not ok, "expected the unreachable connect to fail")
    if tostring(err):find("feature") then
        print("PG BACKEND MISSING: " .. tostring(err))
    else
        print("PG FEATURE APP OK")  -- backend registered; failed on connect, as intended
    end
    return 0
end)
LUA

echo "=== hull build --with=postgres (exercises the GNU-ld --start-group link) ==="
BUILD_OUT=$("$HULL" build --compiler=system --with=postgres --no-verify-platform -o "$APP/bin" "$APP" 2>&1) || true
echo "$BUILD_OUT"
echo "$BUILD_OUT" | grep -q "composed feature 'postgres'" || { echo "FAIL: feature not composed"; exit 1; }
test -x "$APP/bin" || { echo "FAIL: no composed binary produced"; exit 1; }

echo "=== boot the composed binary (expect a connection error, not a feature error) ==="
# Runs under the REAL kernel sandbox: the manifest declares a network database
# (databases.named pg), so hl_sandbox_policy_from_manifest grants network_outbound
# and the connect is allowed to attempt + fail gracefully (unreachable host).
# This also regression-tests that a DB-only app can reach its database sandboxed.
RUN_OUT=$("$APP/bin" 2>&1) || true
echo "$RUN_OUT" | grep -q "PG FEATURE APP OK" || {
    echo "$RUN_OUT"
    echo "FAIL: postgres backend not registered in the composed binary"; exit 1
}
echo "ok  --with=postgres composed + backend live"

echo "=== negative: a plain app must NOT compose postgres ==="
printf 'app.manifest({modules={}})\napp.main(function() print("PLAIN OK") return 0 end)\n' \
    > "$PLAIN/app.lua"
PLAIN_OUT=$("$HULL" build --no-verify-platform -o "$PLAIN/bin" "$PLAIN" 2>&1) || true
if echo "$PLAIN_OUT" | grep -q "composed feature"; then
    echo "$PLAIN_OUT"; echo "FAIL: composed a feature for a plain app"; exit 1
fi
"$PLAIN/bin" 2>&1 | grep -q "PLAIN OK" || { echo "FAIL: plain app did not run"; exit 1; }
echo "ok  plain app stayed postgres-free"

echo "PASS: Postgres feature composed into an app binary; base stays postgres-free"
