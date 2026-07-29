#!/bin/sh
# e2e_feature_sqlite.sh — SQLite as a composable feature (docs/sqlite_feature.md).
#
# Proves the Phase B compose end to end: a SQLite-less DB-core base
# (HL_SQLITE_FEATURE=1: the vtable + selector + generic db.* caps + the weak
# hl_db_feature_backends hook, ZERO compiled backend) plus libhull_feature-sqlite.a,
# joined by `hull build --with=sqlite`, runs a real db.query through the composed
# backend. The base carries no sqlite; the produced app gets it from the archive.
#
# The build dances across two config-sentinel states (SQLite=1 for the archive,
# SQLite=0 for the base), so it stashes + restores build/hull + the platform lib
# and leaves build/ as it found it. Heavy (two clean rebuilds); its own CI job.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
HULL="$ROOT/build/hull"
[ -x "$HULL" ] || { echo "SKIP: build/hull not built"; exit 0; }
case "$(file "$HULL" 2>/dev/null || true)" in
    *cosmo*|*"APE"*) echo "SKIP: cosmo keeps SQLite in-base (fat APE can't force-load)"; exit 0;;
esac

W=""
STASH=$(mktemp -d)
cp "$HULL" "$STASH/hull"                                   # the default toolchain
[ -f build/libhull_platform.a ] && cp build/libhull_platform.a "$STASH/plat.a" || true
cleanup() {
    # Restore build/ to the default toolchain + platform lib we started with.
    cp "$STASH/hull" build/hull 2>/dev/null || true
    [ -f "$STASH/plat.a" ] && cp "$STASH/plat.a" build/libhull_platform.a 2>/dev/null || true
    rm -rf "$STASH"
    [ -n "$W" ] && rm -rf "$W" || true
}
trap cleanup EXIT
fail() { echo "FAIL: $1"; exit 1; }

# 1. The feature archive (SQLite=1 config).
make feature-sqlite >/dev/null 2>&1 || fail "make feature-sqlite"
cp build/libhull_feature-sqlite.a "$STASH/feat.a"

# 2. The SQLite-less DB-core base (SQLite=0). The sentinel clean wipes build/hull.
make platform HL_SQLITE_FEATURE=1 >/dev/null 2>&1 || fail "make platform HL_SQLITE_FEATURE=1"
if command -v nm >/dev/null 2>&1; then
    n=$(nm build/libhull_platform.a 2>/dev/null | grep -cE ' [Tt] _?sqlite3_open$' || true)
    [ "$n" = 0 ] || fail "base is not SQLite-less ($n sqlite3_open symbols)"
    nm build/libhull_platform.a 2>/dev/null | grep -q hl_db_backend_select || fail "base lost the DB core"
fi
echo "ok  SQLite-less DB-core base builds (0 sqlite3_open, DB core intact)"

# 3. Assemble: default toolchain hull + the SQLite-less base + the archive.
cp "$STASH/hull" build/hull
cp "$STASH/feat.a" build/libhull_feature-sqlite.a

# 4. Compose an app and run it: db.query must return through the composed backend.
W=$(mktemp -d)
mkdir -p "$W/app"
printf 'local db = require("hull.db").default()\napp.manifest({ modules = { "hull/db@1" } })\napp.main(function()\n  db.exec("CREATE TABLE t(x INTEGER)")\n  db.exec("INSERT INTO t VALUES(42)")\n  local r = db.query("SELECT x FROM t")\n  return (r[1] and r[1].x == 42) and 0 or 3\nend)\n' > "$W/app/app.lua"

"$HULL" build --with=sqlite --no-verify-platform "$W/app" -o "$W/app/bin" >/dev/null 2>&1 \
    || fail "hull build --with=sqlite (compose onto the SQLite-less base)"

if command -v nm >/dev/null 2>&1; then
    w=$(nm "$W/app/bin" 2>/dev/null | grep -cE ' [Tt] _?sqlite3_open$' || true)
    [ "$w" -ge 1 ] || fail "composed app has no SQLite (should come from the archive)"
fi
rc=0; "$W/app/bin" -d ":memory:" >/dev/null 2>&1 || rc=$?
[ "$rc" = 0 ] || fail "composed db app: db.query should return 42 (exit 0), got $rc"
echo "ok  hull build --with=sqlite: SQLite-less base + archive -> db.query runs (exit 0)"

# 5. A plain app on the SAME SQLite-less base without --with=sqlite fails closed
#    (no default backend for its :memory: DSN) — proves the base really has none.
mkdir -p "$W/plain"
printf 'local db = require("hull.db").default()\napp.manifest({ modules = { "hull/db@1" } })\napp.main(function()\n  local ok = pcall(function() db.exec("CREATE TABLE t(x)") end)\n  return ok and 5 or 0\nend)\n' > "$W/plain/app.lua"
if "$HULL" build --no-verify-platform "$W/plain" -o "$W/plain/bin" >/dev/null 2>&1; then
    rc=0; "$W/plain/bin" -d ":memory:" >/dev/null 2>&1 || rc=$?
    [ "$rc" = 0 ] || fail "uncomposed app on a SQLite-less base should fail closed on db use (exit 0 from pcall-guard), got $rc"
    echo "ok  uncomposed app on the SQLite-less base has no backend (db use fails closed)"
fi

echo "PASS: e2e_feature_sqlite (SQLite-less base + libhull_feature-sqlite.a composes + runs)"
