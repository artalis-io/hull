#!/bin/sh
# e2e_named_connections.sh: E2E for per-connection db.async / db.udf
#
# Proves the multi-backend handles-only API routes async + udf to the RIGHT
# connection (not always the default): a named SQLite connection's db.async
# opens its own worker connection to that database, and db.udf.register lands
# on that connection. Covers both runtimes. No Docker (SQLite only); the
# Postgres cross-backend variant lives in tests/e2e_postgres.sh.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PASS=0
FAIL=0
TOTAL=0

pass() { PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)); echo "  FAIL: $1 (got: $2)"; }

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# The default DB holds x=1; the named "cache" DB holds x=99. If async/udf
# targeted the default instead of the bound connection, the cache assertions
# below would read 1 (or the udf would register on the wrong handle).
run_case() {
    ext="$1"; label="$2"
    app="$TMPDIR/app.$ext"
    default_db="$TMPDIR/default_$ext.db"
    cache_db="$TMPDIR/cache_$ext.db"
    rm -f "$default_db" "$cache_db"

    out=$("$HULL" --no-sandbox -d "$default_db" "$app" 2>&1 || true)

    got=$(printf '%s\n' "$out" | sed -n 's/.*cache_async_x=\([0-9]*\).*/\1/p' | head -1)
    [ "$got" = "99" ] && pass "$label: named async targets the cache DB" \
                       || fail "$label: named async targets the cache DB" "$got"

    got=$(printf '%s\n' "$out" | sed -n 's/.*default_async_x=\([0-9]*\).*/\1/p' | head -1)
    [ "$got" = "1" ] && pass "$label: default async targets the default DB" \
                     || fail "$label: default async targets the default DB" "$got"

    got=$(printf '%s\n' "$out" | sed -n 's/.*cache_udf_y=\([0-9]*\).*/\1/p' | head -1)
    [ "$got" = "198" ] && pass "$label: named udf registers on the cache DB" \
                       || fail "$label: named udf registers on the cache DB" "$got"
}

# ── Lua ────────────────────────────────────────────────────────────────
cat > "$TMPDIR/app.lua" << EOF
local db = require("hull.db")
app.manifest({ modules = { "hull/db@1" }, databases = { cache = "$TMPDIR/cache_lua.db" } })
app.main(function(ctx)
  local d = db.default()
  local c = db.connect("cache")
  d.exec("CREATE TABLE t(x INTEGER)"); d.exec("INSERT INTO t VALUES (1)")
  c.exec("CREATE TABLE t(x INTEGER)"); c.exec("INSERT INTO t VALUES (99)")
  local rc = c.async.query("SELECT x FROM t")
  local rd = d.async.query("SELECT x FROM t")
  print("cache_async_x=" .. tostring(rc[1].x))
  print("default_async_x=" .. tostring(rd[1].x))
  c.udf.register("hull_double", function(n) return n * 2 end)
  local ru = c.query("SELECT hull_double(x) AS y FROM t")
  print("cache_udf_y=" .. tostring(ru[1].y))
  return 0
end)
EOF

echo "=== E2E: per-connection async/udf (Lua) ==="
run_case lua Lua

# ── JS ─────────────────────────────────────────────────────────────────
cat > "$TMPDIR/app.js" << EOF
import { app } from "hull:app";
import { db as dbMod } from "hull:db";
app.manifest({ modules: ["hull/db@1"], databases: { cache: "$TMPDIR/cache_js.db" } });
app.main(async (ctx) => {
  const d = dbMod.default();
  const c = dbMod.connect("cache");
  d.exec("CREATE TABLE t(x INTEGER)"); d.exec("INSERT INTO t VALUES (1)");
  c.exec("CREATE TABLE t(x INTEGER)"); c.exec("INSERT INTO t VALUES (99)");
  const rc = await c.async.query("SELECT x FROM t");
  const rd = await d.async.query("SELECT x FROM t");
  console.log("cache_async_x=" + rc[0].x);
  console.log("default_async_x=" + rd[0].x);
  c.udf.register("hull_double", (n) => n * 2);
  const ru = c.query("SELECT hull_double(x) AS y FROM t");
  console.log("cache_udf_y=" + ru[0].y);
  return 0;
});
EOF

echo "=== E2E: per-connection async/udf (JS) ==="
run_case js JS

echo ""
echo "=== Results: $PASS/$TOTAL passed ==="
[ "$FAIL" -eq 0 ] || exit 1
