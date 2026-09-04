#!/bin/sh
# e2e_feature_db_shared.sh - the shared SQL-wire transport composed by BOTH DB
# features at once.
#
# The Postgres and MySQL wire clients share one byte transport,
# cap/db_transport.c (docs/db_transport_extraction.md). cap_db_transport.o is
# bundled in BOTH libhull_feature-postgres.a and libhull_feature-mysql.a; because
# those archives are pull-by-symbol (not whole-archived), composing both must
# extract the transport EXACTLY ONCE - no duplicate-symbol error - in either
# --with order. This test proves that end to end: it builds a base hull + both
# feature archives, composes an app with `--with=postgres --with=mysql` and again
# with the reversed order, and asserts each links and carries exactly one
# hl_db_transport_connect definition.
#
# Must run on a fresh build tree; its own CI job. Native only.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

BASE=/tmp/hull_base_db_shared_e2e
PGA=/tmp/libhull_feature-postgres_db_shared.a
APP=$(mktemp -d)
trap 'rm -rf "$APP" "$BASE" "$PGA"' EXIT

echo "=== build base hull (EMBED_PLATFORM=1; base has neither DB backend) ==="
make EMBED_PLATFORM=1 >/dev/null
cp build/hull "$BASE"
# The base must carry zero shared-transport symbols.
nm build/hull 2>/dev/null | grep -qE 'hl_db_transport_' \
    && { echo "FAIL: base hull contains hl_db_transport_* (should be composed-only)"; exit 1; }
echo "ok  base is transport-free"

echo "=== build both feature archives (each flag-flip cleans build/, so stash pg) ==="
make feature-postgres >/dev/null
cp build/libhull_feature-postgres.a "$PGA"
make feature-mysql >/dev/null
# Restore the postgres archive alongside the mysql one so the compose resolver
# (build/ first) finds BOTH.
cp "$PGA" build/libhull_feature-postgres.a
for a in postgres mysql; do
    ar t "build/libhull_feature-$a.a" | grep -q '^cap_db_transport.o$' \
        || { echo "FAIL: libhull_feature-$a.a lacks cap_db_transport.o"; exit 1; }
done
echo "ok  cap_db_transport.o present in both archives"

cat > "$APP/app.lua" <<'LUA'
app.manifest({ modules = { "hull/db@1" } })
app.main(function() print("DB SHARED OK") return 0 end)
LUA

# Count the defined hl_db_transport_connect symbols: exactly one proves the shared
# object was extracted once (a duplicate would be a link error; zero would mean the
# transport never composed).
assert_one_impl() {
    _bin=$1; _label=$2
    test -x "$_bin" || { echo "FAIL ($_label): no composed binary"; exit 1; }
    _n=$(nm "$_bin" 2>/dev/null | grep -cE ' [Tt] _?hl_db_transport_connect$' || true)
    [ "$_n" = 1 ] || { echo "FAIL ($_label): hl_db_transport_connect defs=$_n (want 1)"; exit 1; }
    echo "ok  $_label: links, exactly one hl_db_transport_connect impl"
}

echo "=== compose order A: --with=postgres --with=mysql ==="
OUTA=$("$BASE" build --compiler=system --with=postgres --with=mysql \
        --no-verify-platform -o "$APP/binA" "$APP" 2>&1) || { echo "$OUTA"; echo "FAIL: order A build"; exit 1; }
echo "$OUTA" | grep -q "composed feature 'postgres'" || { echo "$OUTA"; echo "FAIL: postgres not composed (A)"; exit 1; }
echo "$OUTA" | grep -q "composed feature 'mysql'"    || { echo "$OUTA"; echo "FAIL: mysql not composed (A)"; exit 1; }
assert_one_impl "$APP/binA" "order A"
"$APP/binA" 2>&1 | grep -q "DB SHARED OK" || { echo "FAIL: order A binary did not run"; exit 1; }

echo "=== compose order B: --with=mysql --with=postgres (reversed) ==="
OUTB=$("$BASE" build --compiler=system --with=mysql --with=postgres \
        --no-verify-platform -o "$APP/binB" "$APP" 2>&1) || { echo "$OUTB"; echo "FAIL: order B build"; exit 1; }
assert_one_impl "$APP/binB" "order B"
"$APP/binB" 2>&1 | grep -q "DB SHARED OK" || { echo "FAIL: order B binary did not run"; exit 1; }

echo "PASS: shared db_transport composed once by both DB features in both --with orders"
