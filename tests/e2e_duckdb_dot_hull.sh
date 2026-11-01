#!/bin/sh
# e2e_duckdb_dot_hull.sh - DuckDB SQL cannot reach the app's .hull
# (audit convergence §7.2).
#
# The app's reserved .hull directory holds Hull's internal database (session
# tokens, TOTP secrets). With fs = { read = {"."}, write = {"."} } a duckdb://
# connection (mode B) had the whole app root in DuckDB's allowed_directories,
# so read_blob('.hull/internal.db') returned the internal file raw and COPY
# wrote into .hull. serve.c now grants the root as its top-level entries minus
# .hull, and the SQL scan refuses a literal naming .hull and a glob that could
# reach it. Runs the fixture under a hull built with `make HL_ENABLE_DUCKDB=1`
# (serve.c installs the mode-B policy there); ordinary reads in data/ must
# still work.
#
# Usage: sh tests/e2e_duckdb_dot_hull.sh [path/to/hull]
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu
cd "$(dirname "$0")/.."
HULL=${1:-$(pwd)/build/hull}
[ -x "$HULL" ] || { echo "SKIP: no hull at $HULL (make HL_ENABLE_DUCKDB=1)"; exit 0; }

APP=$(mktemp -d)
PID=""
cleanup() {
    [ -n "$PID" ] && kill "$PID" 2>/dev/null || true
    rm -rf "$APP"
}
trap cleanup EXIT

cp tests/fixtures/duckdb_dot_hull/app.lua "$APP/"
mkdir -p "$APP/data"
printf 'a,b\n1,2\n3,4\n' > "$APP/data/x.csv"

PORT=$((20000 + $$ % 10000))
(cd "$APP" && exec "$HULL" app.lua -p "$PORT" > srv.log 2>&1) &
PID=$!
OUT=""
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    sleep 0.5
    OUT=$(curl -s -m 20 "http://127.0.0.1:$PORT/probe" 2>/dev/null) && [ -n "$OUT" ] && break
done
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=""

echo "$OUT" | grep -q "^IDB NOLEAK " || { echo "$OUT"; tail -5 "$APP/srv.log"; echo "FAIL: duckdb_convergence_idb.served_root_grant_no_leak"; exit 1; }
echo "PASS: duckdb_convergence_idb.served_root_grant_no_leak"
echo "$OUT" | grep -q "READS OK 2 1" || { echo "$OUT"; tail -5 "$APP/srv.log"; echo "FAIL: duckdb_convergence_idb.served_root_grant_reads"; exit 1; }
[ ! -e "$APP/.hull/x.csv" ] && [ ! -e "$APP/late.csv" ] && [ -f "$APP/data/out.csv" ] \
    || { echo "FAIL: duckdb_convergence_idb.served_root_grant_files"; exit 1; }
echo "PASS: duckdb_convergence_idb.served_root_grant_reads"
