#!/bin/sh
# e2e_feature_mysql.sh — MySQL/MariaDB as a composable feature, end to end.
#
# Builds a BASE hull (EMBED_PLATFORM=1, no MySQL compiled in) + the MySQL feature
# archive, then `hull build --with=mysql` an app and boots it. Proves that
# `hull build` composes libhull_feature-mysql.a + a generated registry (filling
# the hl_db_feature_backends hook, like Postgres/DuckDB) into the app binary, so a
# base-built hull gains the mysql:// backend purely from the feature. One backend
# serves both mysql:// and mariadb://.
#
# This is a BUILD-only e2e (no live server): the app connects to an unreachable
# host and asserts the error is a CONNECTION failure, not a "needs the MySQL
# feature" scheme rejection — i.e. the backend is registered and live. A real
# connect + auth + query is covered by the monolithic e2e_mysql.sh job.
#
# Runs under the REAL kernel sandbox: the manifest declares a network database
# (databases.named my), so hl_sandbox_policy_from_manifest grants network_outbound
# and the connect is allowed to attempt + fail gracefully. This also exercises the
# GNU-ld --start-group compose link (the mysql backend references base tls_client +
# crypto that a DB-only app doesn't otherwise pull). --compiler=system.
#
# Must run on a fresh build tree (no prior HL_ENABLE_MYSQL=1 objects); its own CI
# job. Native only.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

echo "=== build base hull (EMBED_PLATFORM=1; base has no mysql backend) ==="
make EMBED_PLATFORM=1 >/dev/null
# Stash the base hull: `make feature-mysql` re-invokes make with HL_ENABLE_MYSQL=1
# and the build-config sentinel cleans build/ on the flip.
cp build/hull /tmp/hull_base_my_e2e

echo "=== build the MySQL feature archive ==="
make feature-mysql >/dev/null
ls -la build/libhull_feature-mysql.a
nm build/libhull_feature-mysql.a 2>/dev/null | grep -qE '[ _]hl_db_backend_mysql$' \
    || { echo "FAIL: feature archive lacks hl_db_backend_mysql"; exit 1; }

HULL=/tmp/hull_base_my_e2e
APP=$(mktemp -d)
PLAIN=$(mktemp -d)
trap 'rm -rf "$APP" "$PLAIN" /tmp/hull_base_my_e2e' EXIT

cat > "$APP/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/db@1" },
    -- Unreachable host + short timeout: the connect must FAIL, and the failure
    -- proves the backend routed mysql:// (a scheme rejection would fire first).
    databases = { named = { my = "mysql://u:p@10.255.255.1:3306/db?connect_timeout=1" } },
})
app.main(function()
    local ok, err = pcall(function()
        require("hull.db").connect("my").query("SELECT 1")
    end)
    assert(not ok, "expected the unreachable connect to fail")
    if tostring(err):find("feature") then
        print("MYSQL BACKEND MISSING: " .. tostring(err))
    else
        print("MYSQL FEATURE APP OK")  -- backend registered; failed on connect
    end
    return 0
end)
LUA

echo "=== hull build --with=mysql (exercises the GNU-ld --start-group link) ==="
BUILD_OUT=$("$HULL" build --compiler=system --with=mysql --no-verify-platform -o "$APP/bin" "$APP" 2>&1) || true
echo "$BUILD_OUT"
echo "$BUILD_OUT" | grep -q "composed feature 'mysql'" || { echo "FAIL: feature not composed"; exit 1; }
test -x "$APP/bin" || { echo "FAIL: no composed binary produced"; exit 1; }

echo "=== boot the composed binary (expect a connection error, not a feature error) ==="
RUN_OUT=$("$APP/bin" 2>&1) || true
echo "$RUN_OUT" | grep -q "MYSQL FEATURE APP OK" || {
    echo "$RUN_OUT"
    echo "FAIL: mysql backend not registered in the composed binary"; exit 1
}
echo "ok  --with=mysql composed + backend live"

echo "=== negative: a plain app must NOT compose mysql ==="
printf 'app.manifest({modules={}})\napp.main(function() print("PLAIN OK") return 0 end)\n' \
    > "$PLAIN/app.lua"
PLAIN_OUT=$("$HULL" build --no-verify-platform -o "$PLAIN/bin" "$PLAIN" 2>&1) || true
if echo "$PLAIN_OUT" | grep -q "composed feature"; then
    echo "$PLAIN_OUT"; echo "FAIL: composed a feature for a plain app"; exit 1
fi
"$PLAIN/bin" 2>&1 | grep -q "PLAIN OK" || { echo "FAIL: plain app did not run"; exit 1; }
echo "ok  plain app stayed mysql-free"

echo "PASS: MySQL feature composed into an app binary; base stays mysql-free"
