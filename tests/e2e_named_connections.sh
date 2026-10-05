#!/bin/sh
# e2e_named_connections.sh: E2E for per-connection db.async / db.udf
#
# Proves the multi-backend handles-only API routes async + udf to the RIGHT
# connection (not always the default): a named SQLite connection's db.async
# opens its own worker connection to that database, and db.udf.register lands
# on that connection. Also (audit 6): a BEGIN through db.async is rolled back
# and fails rather than staying open on the pooled worker connection (M3), and
# a transaction a failed request leaves open is rolled back when that request
# ends, so a handler that resumes meanwhile does not join it (M1). Covers both
# runtimes. No Docker (SQLite only); the Postgres cross-backend variant lives
# in tests/e2e_postgres.sh.
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

    case "$out" in
        *"async_begin_err="*"cannot span db.async"*)
            pass "$label: BEGIN through db.async is refused" ;;
        *) fail "$label: BEGIN through db.async is refused" "$out" ;;
    esac
    got=$(printf '%s\n' "$out" | sed -n 's/.*after_async_begin_n=\([0-9]*\).*/\1/p' | head -1)
    [ "$got" = "2" ] && pass "$label: the worker connection left no transaction behind" \
                     || fail "$label: the worker connection left no transaction behind" "$got"
}

# ── Lua ────────────────────────────────────────────────────────────────
cat > "$TMPDIR/app.lua" << EOF
local db = require("hull.db")
app.manifest({ modules = { "hull/db@1" }, databases = { named = { cache = "$TMPDIR/cache_lua.db" } } })
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
  local ok, err = pcall(function() return c.async.exec("BEGIN IMMEDIATE") end)
  print("async_begin_err=" .. tostring(not ok and err or "none"))
  c.async.exec("INSERT INTO t VALUES (7)")
  c.exec("BEGIN IMMEDIATE")   -- SQLITE_BUSY behind a worker left in a write txn
  c.exec("COMMIT")
  print("after_async_begin_n=" .. tostring(c.query("SELECT count(*) AS n FROM t")[1].n))
  return 0
end)
EOF

echo "=== E2E: per-connection async/udf (Lua) ==="
run_case lua Lua

# ── JS ─────────────────────────────────────────────────────────────────
cat > "$TMPDIR/app.js" << EOF
import { app } from "hull:app";
import { db as dbMod } from "hull:db";
app.manifest({ modules: ["hull/db@1"], databases: { named: { cache: "$TMPDIR/cache_js.db" } } });
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
  let berr = "none";
  try { await c.async.exec("BEGIN IMMEDIATE"); } catch (e) { berr = String(e && e.message || e); }
  console.log("async_begin_err=" + berr);
  await c.async.exec("INSERT INTO t VALUES (7)");
  c.exec("BEGIN IMMEDIATE");
  c.exec("COMMIT");
  console.log("after_async_begin_n=" + c.query("SELECT count(*) AS n FROM t")[0].n);
  return 0;
});
EOF

echo "=== E2E: per-connection async/udf (JS) ==="
run_case js JS

# ── A transaction a failed request leaves is not joined by a resume ─────
# /slow parks on hull.sleep; /fail opens a transaction, inserts and raises
# while /slow is parked. /slow then resumes and inserts. The failed request's
# transaction must be gone by then: guarded only at the start of the NEXT
# entry, /slow's insert joined it and the next request rolled both back.
SERVER_PID=""
stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}
trap 'stop_server; rm -rf "$TMPDIR"' EXIT

run_resume_case() {
    ext="$1"; label="$2"; port="$3"
    db="$TMPDIR/resume_$ext.db"
    rm -f "$db"
    "$HULL" --no-sandbox -d "$db" -p "$port" "$TMPDIR/srv.$ext" \
        > "$TMPDIR/srv_$ext.log" 2>&1 &
    SERVER_PID=$!
    up=0
    for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if curl -s "http://127.0.0.1:$port/count" >/dev/null 2>&1; then up=1; break; fi
        sleep 0.3
    done
    if [ "$up" != 1 ]; then
        fail "$label: server started" "$(cat "$TMPDIR/srv_$ext.log")"
        stop_server; return
    fi
    curl -s "http://127.0.0.1:$port/slow" > "$TMPDIR/slow_$ext.out" 2>&1 &
    slow_pid=$!
    sleep 0.3
    curl -s -o /dev/null "http://127.0.0.1:$port/fail" || true
    wait "$slow_pid" || true
    got=$(curl -s "http://127.0.0.1:$port/count")
    case "$got" in
        *'"b":0'*) case "$got" in *'"a":1'*) got=ok ;; esac ;;
    esac
    case "$got" in
        ok) pass "$label: a resumed handler does not join a failed request's transaction" ;;
        *) fail "$label: a resumed handler does not join a failed request's transaction" \
                "$got / slow: $(cat "$TMPDIR/slow_$ext.out")" ;;
    esac
    stop_server
}

cat > "$TMPDIR/srv.lua" << 'EOF'
local db = require("hull.db")
app.manifest({ modules = { "hull/db@1", "hull/http-server@1" } })
app.main(function()
  db.default().exec("CREATE TABLE IF NOT EXISTS r(v TEXT)")
  return 0
end)
app.get("/slow", function(req, res)
  hull.sleep(800)
  db.default().exec("INSERT INTO r VALUES ('a')")
  res:json({ ok = true })
end)
app.get("/fail", function(req, res)
  local c = db.default()
  c.exec("BEGIN")
  c.exec("INSERT INTO r VALUES ('b')")
  error("boom")
end)
app.get("/count", function(req, res)
  local c = db.default()
  res:json({ a = c.query("SELECT count(*) AS n FROM r WHERE v = 'a'")[1].n,
             b = c.query("SELECT count(*) AS n FROM r WHERE v = 'b'")[1].n })
end)
EOF

cat > "$TMPDIR/srv.js" << 'EOF'
import { app } from "hull:app";
import { db } from "hull:db";
app.manifest({ modules: ["hull/db@1", "hull/http-server@1"] });
app.main(() => { db.default().exec("CREATE TABLE IF NOT EXISTS r(v TEXT)"); return 0; });
app.get("/slow", async (req, res) => {
  await hull.sleep(800);
  db.default().exec("INSERT INTO r VALUES ('a')");
  res.json({ ok: true });
});
app.get("/fail", (req, res) => {
  const c = db.default();
  c.exec("BEGIN");
  c.exec("INSERT INTO r VALUES ('b')");
  throw new Error("boom");
});
app.get("/count", (req, res) => {
  const c = db.default();
  res.json({ a: c.query("SELECT count(*) AS n FROM r WHERE v = 'a'")[0].n,
             b: c.query("SELECT count(*) AS n FROM r WHERE v = 'b'")[0].n });
});
EOF

if command -v curl >/dev/null 2>&1; then
    echo "=== E2E: stale transaction vs a resumed handler (Lua) ==="
    run_resume_case lua Lua 18931
    echo "=== E2E: stale transaction vs a resumed handler (JS) ==="
    run_resume_case js JS 18932
else
    echo "  SKIP: curl not found (stale transaction vs resume)"
fi

echo ""
echo "=== Results: $PASS/$TOTAL passed ==="
[ "$FAIL" -eq 0 ] || exit 1
