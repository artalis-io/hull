#!/bin/sh
# The per-user state directory (shared/state_dir.h, platform gap 3), both
# runtimes, sandboxed (the kernel grant is part of what is proved):
#   - "@state/" writes and reads land under $XDG_STATE_HOME/<name>, made 0700
#     by Hull; a private write is 0600; the app root is a separate namespace
#   - HULL_STATE_DIR redirects it; one that is too broad is refused at load
#   - ".." after the prefix, "@other/", and the state root's .hull are refused
#   - a "@state/" grant without state.name, an "@other/" grant and an unusable
#     state.name are refused at load
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
HULL_BIN="${HULL_BIN:-build/hull}"
case "$HULL_BIN" in /*) ;; *) HULL_BIN="$(pwd)/$HULL_BIN" ;; esac   # the legs cd into the app dir
. "$(dirname "$0")/lib/hull_rc.sh"
hull_rc_init "$HULL_BIN"
work=$(mktemp -d)
trap 'chmod -R u+rwx "$work" 2>/dev/null; rm -rf "$work"' EXIT
out="$work/output"
fail() { echo "FAIL: $1"; cat "$out" 2>/dev/null || true; exit 1; }
line() { sed -n "s/^\(.*\[app\] \)\{0,1\}$1=\(.*\)$/\2/p" "$out" | head -1; }
mode_of() {
    if stat -f %Lp "$1" >/dev/null 2>&1; then stat -f %Lp "$1"; else stat -c %a "$1"; fi
}
# The sandbox is on, so the kernel grant is exercised too. The fixtures are
# copied so the app directory is not the repo (a state dir never lives there).
appdir="$work/app"; mkdir -p "$appdir"; cp tests/fixtures/state_dir/app.lua tests/fixtures/state_dir/app.js "$appdir/"
baddir="$work/bad"; mkdir -p "$baddir"; cp tests/fixtures/state_dir_bad/*.lua "$baddir/"

for ext in lua js; do
    xdg="$work/xdg-$ext"
    rc=$(cd "$appdir" && XDG_STATE_HOME="$xdg" HULL_STATE_DIR= hull_run "$out" "$HULL_BIN" "app.$ext")
    [ "$rc" = 0 ] || fail "$ext roundtrip: exit $rc"
    [ "$(line read_back)" = "secret-bytes" ] || fail "$ext: read back"
    [ "$(line notes)" = "hello state" ] || fail "$ext: notes"
    [ "$(line stat_type)" = "file" ] || fail "$ext: stat"
    [ "$(line listed)" = "keys,notes.txt" ] || fail "$ext: list of the state root (got $(line listed))"
    [ "$(line app_root_denied)" = "true" ] || fail "$ext: the app root is another namespace"
    sdir="$xdg/hull-e2e-state"
    [ -d "$sdir" ] || fail "$ext: $sdir was not made"
    [ "$(mode_of "$sdir")" = 700 ] || fail "$ext: state dir mode $(mode_of "$sdir"), expected 700"
    [ "$(mode_of "$sdir/keys")" = 700 ] || fail "$ext: keys/ mode $(mode_of "$sdir/keys"), expected 700 (private write)"
    [ "$(mode_of "$sdir/keys/trust.key")" = 600 ] || fail "$ext: trust.key mode, expected 600"
    [ -f "$sdir/notes.txt" ] || fail "$ext: notes.txt not under the state dir"
    [ ! -e "$appdir/keys" ] || fail "$ext: keys/ landed in the app directory"
    echo "PASS: $ext @state/ writes, reads, stats and lists under XDG_STATE_HOME/<name>, 0700"

    rc=$(cd "$appdir" && XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" "app.$ext" -- traverse)
    [ "$rc" = 0 ] || fail "$ext traverse: exit $rc"
    [ "$(line traverse)" = "true" ] || fail "$ext: .. after the prefix was not refused"
    [ "$(line other_root)" = "true" ] || fail "$ext: @other/ was not refused"
    [ "$(line state_hull)" = "true" ] || fail "$ext: the state root's .hull was reachable"
    echo "PASS: $ext .., @other/ and @state/.hull are refused"

    over="$work/override-$ext"
    rc=$(cd "$appdir" && HULL_STATE_DIR="$over" hull_run "$out" "$HULL_BIN" "app.$ext")
    [ "$rc" = 0 ] || fail "$ext HULL_STATE_DIR: exit $rc"
    [ -f "$over/keys/trust.key" ] || fail "$ext: HULL_STATE_DIR was not used"
    [ "$(mode_of "$over")" = 700 ] || fail "$ext: HULL_STATE_DIR mode, expected 700"
    echo "PASS: $ext HULL_STATE_DIR redirects the state directory"

    rc=$(cd "$appdir" && HULL_STATE_DIR=/ hull_run "$out" "$HULL_BIN" "app.$ext")
    [ "$rc" != 0 ] || fail "$ext: HULL_STATE_DIR=/ was accepted"
    grep -q 'refusing to use it as the state directory' "$out" || fail "$ext: no refusal for HULL_STATE_DIR=/"
    echo "PASS: $ext HULL_STATE_DIR=/ is refused at load"
done

for fx in app other badname db; do
    rc=$(cd "$baddir" && XDG_STATE_HOME="$work/xdg-bad" hull_run "$out" "$HULL_BIN" "$fx.lua")
    [ "$rc" != 0 ] || fail "$fx.lua: a bad manifest ran"
    ! grep -q 'ran=yes' "$out" || fail "$fx.lua: main ran"
done
echo "PASS: a @state/ grant or database without state.name, an @other/ grant and a bad state.name are refused at load"

# ── The default database under the state directory (PR 2) ───────────────
has_agent=1
if "$HULL_BIN" doctor --json 2>/dev/null | grep -q '"http":false'; then has_agent=0; fi
for ext in lua js; do
    dbdir="$work/db-$ext"; mkdir -p "$dbdir/migrations"
    cp "tests/fixtures/state_dir_db/app.$ext" "$dbdir/"; cp tests/fixtures/state_dir_db/migrations/001_init.sql "$dbdir/migrations/"
    xdg="$work/xdg-db-$ext"; sdir="$xdg/hull-e2e-state-db"
    rc=$(cd "$dbdir" && XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" "app.$ext")
    [ "$rc" = 0 ] || fail "$ext db: exit $rc"
    [ "$(line count)" = 1 ] || fail "$ext db: count after the first run (got $(line count))"
    [ "$(line cache_rows)" = 1 ] || fail "$ext db: the named cache connection"
    [ -f "$sdir/data.db" ] || fail "$ext db: data.db is not under the state dir"
    [ -f "$sdir/cache.db" ] || fail "$ext db: cache.db is not under the state dir"
    [ ! -e "$dbdir/data.db" ] || fail "$ext db: the implicit data.db was left in the app directory"
    rc=$(cd "$dbdir" && XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" "app.$ext")
    [ "$rc" = 0 ] || fail "$ext db: second run exit $rc"
    [ "$(line count)" = 2 ] || fail "$ext db: the state database did not persist (got $(line count))"
    echo "PASS: $ext databases.named.default under @state/ is the migrated default, persistent, no stray data.db"

    rc=$(XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" migrate status "$dbdir")
    [ "$rc" = 0 ] || fail "$ext migrate status: exit $rc"
    grep -q '\[x\] 001_init.sql' "$out" || fail "$ext migrate status does not see the state database"
    rc=$(XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" migrate "$dbdir")
    [ "$rc" = 0 ] || fail "$ext migrate: exit $rc"
    grep -q 'already up to date' "$out" || fail "$ext migrate did not find the state database migrated"
    # `hull agent` needs the HTTP server (an HTTP-free build has no agent).
    if [ "$has_agent" = 1 ]; then
        rc=$(XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" agent db schema "$dbdir")
        [ "$rc" = 0 ] || fail "$ext agent db schema: exit $rc"
        grep -q '"name":"notes"' "$out" || fail "$ext agent db schema does not see the state database"
        echo "PASS: $ext hull migrate and hull agent db schema open the state database with no -d"
    else
        echo "PASS: $ext hull migrate opens the state database with no -d (no hull agent in this build)"
    fi
done
# ── Audit 44: the re-pointed default and the stdlib around it ─────────────
PY=""
for p in python3 python; do
    if command -v "$p" >/dev/null 2>&1 && "$p" -c "import sqlite3" >/dev/null 2>&1; then PY="$p"; break; fi
done
# sql DB QUERY -> first column of the first row ("" when none / no file)
sql() {
    "$PY" - "$1" "$2" <<'PYEOF'
import sqlite3, sys, os
if not os.path.exists(sys.argv[1]):
    print(""); sys.exit(0)
c = sqlite3.connect(sys.argv[1])
try:
    r = c.execute(sys.argv[2]).fetchone()
    print("" if r is None else r[0])
except sqlite3.Error as e:
    print("ERR:" + str(e))
PYEOF
}
a44_app() {   # a44_app <dir> <fixture> <ext>
    mkdir -p "$1/migrations"
    cp "tests/fixtures/state_dir_audit44/$2.$3" "$1/app.$3"
    cp tests/fixtures/state_dir_audit44/migrations/001_init.sql "$1/migrations/"
}
a44=0
# The top / migr fixtures declare middleware modules (they admit
# hull/http-server), so app.main returns 3 to exit before the serve loop.
for ext in lua js; do
    # db_registry_audit44.repoint_keeps_captured_default: a db.default()
    # object captured at top level, and a top-level rbac.init() /
    # session.init(), follow the re-pointed default.
    d="$work/a44-top-$ext"; a44_app "$d" top "$ext"
    xdg="$work/xdg-a44-top-$ext"; sdir="$xdg/hull-e2e-a44"
    rc=$(cd "$d" && XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" "app.$ext")
    [ "$rc" = 3 ] || fail "db_registry_audit44.repoint_keeps_captured_default ($ext): exit $rc"
    [ "$(line notes)" = 1 ] || fail "db_registry_audit44.repoint_keeps_captured_default ($ext): notes (got $(line notes))"
    [ "$(line roles)" = 1 ] || fail "db_registry_audit44.repoint_keeps_captured_default ($ext): roles (got $(line roles))"
    if [ -n "$PY" ]; then
        [ "$(sql "$sdir/data.db" "SELECT count(*) FROM notes")" = 1 ] \
            || fail "db_registry_audit44.repoint_keeps_captured_default ($ext): the captured write is not in <state>/data.db"
        [ "$(sql "$sdir/.hull/internal.db" "SELECT count(*) FROM _hull_user_roles")" = 1 ] \
            || fail "db_registry_audit44.repoint_keeps_captured_default ($ext): _hull_* are not in <state>/.hull/internal.db"
        [ "$(sql "$sdir/.hull/internal.db" "SELECT count(*) FROM sqlite_master WHERE name='_hull_sessions'")" = 1 ] \
            || fail "db_registry_audit44.repoint_keeps_captured_default ($ext): the top-level session table is not in <state>/.hull/internal.db"
    fi
    [ ! -e "$d/data.db" ] || fail "db_registry_audit44.repoint_keeps_captured_default ($ext): stray <app>/data.db"
    [ ! -e "$d/.hull/internal.db" ] || fail "db_registry_audit44.repoint_keeps_captured_default ($ext): stray <app>/.hull/internal.db"
    echo "PASS: db_registry_audit44.repoint_keeps_captured_default ($ext)"; a44=$((a44 + 1))

    # worker_db_audit44.dispatch_follows_manifest_default
    d="$work/a44-worker-$ext"; a44_app "$d" worker "$ext"
    xdg="$work/xdg-a44-worker-$ext"
    rc=$(cd "$d" && XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" "app.$ext")
    [ "$rc" = 0 ] || fail "worker_db_audit44.dispatch_follows_manifest_default ($ext): exit $rc"
    [ "$(line worker_ok)" = true ] || fail "worker_db_audit44.dispatch_follows_manifest_default ($ext): worker.dispatch failed"
    [ "$(line main_notes)" = 1 ] || fail "worker_db_audit44.dispatch_follows_manifest_default ($ext): the worker's row is not in the state database"
    [ ! -e "$d/data.db" ] || fail "worker_db_audit44.dispatch_follows_manifest_default ($ext): the worker made <app>/data.db"
    echo "PASS: worker_db_audit44.dispatch_follows_manifest_default ($ext)"; a44=$((a44 + 1))

    # migrate_audit44.move_internal_follows_state_default: `hull migrate`
    # moves the _hull_* tables into the file the runner reads.
    d="$work/a44-migr-$ext"; a44_app "$d" migr "$ext"
    xdg="$work/xdg-a44-migr-$ext"; sdir="$xdg/hull-e2e-a44m"
    rc=$(cd "$d" && XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" "app.$ext" --keep-internal-in-app-db -- seed)
    [ "$rc" = 3 ] || fail "migrate_audit44.move_internal_follows_state_default ($ext): seed exit $rc"
    [ "$(line roles)" = 1 ] || fail "migrate_audit44.move_internal_follows_state_default ($ext): seed roles"
    rc=$(XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" migrate "$d")
    [ "$rc" = 0 ] || fail "migrate_audit44.move_internal_follows_state_default ($ext): hull migrate exit $rc"
    [ ! -e "$sdir/data.db.hull-internal" ] || fail "migrate_audit44.move_internal_follows_state_default ($ext): moved into data.db.hull-internal"
    if [ -n "$PY" ]; then
        [ "$(sql "$sdir/.hull/internal.db" "SELECT count(*) FROM _hull_internal_meta WHERE key='app_db_moved'")" = 1 ] \
            || fail "migrate_audit44.move_internal_follows_state_default ($ext): no move record in <state>/.hull/internal.db"
    fi
    rc=$(cd "$d" && XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" "app.$ext" --no-migrate)
    [ "$rc" = 3 ] || fail "migrate_audit44.move_internal_follows_state_default ($ext): --no-migrate exit $rc"
    [ "$(line roles)" = 1 ] || fail "migrate_audit44.move_internal_follows_state_default ($ext): --no-migrate does not see the moved rows (roles=$(line roles))"
    echo "PASS: migrate_audit44.move_internal_follows_state_default ($ext)"; a44=$((a44 + 1))

    # db_registry_audit44.repoint_carries_toplevel_default_tables: a
    # top-level jobs.init() + jobs.enqueue() made _hull_jobs in the implicit
    # default; it is carried into the @state/ default, its row with it.
    d="$work/a44-jobs-$ext"; a44_app "$d" jobs "$ext"
    xdg="$work/xdg-a44-jobs-$ext"; sdir="$xdg/hull-e2e-a44j"
    rc=$(cd "$d" && XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" "app.$ext")
    [ "$rc" = 0 ] || fail "db_registry_audit44.repoint_carries_toplevel_default_tables ($ext): exit $rc"
    [ "$(line enqueued)" = 2 ] || fail "db_registry_audit44.repoint_carries_toplevel_default_tables ($ext): app.main's enqueue failed"
    if [ -n "$PY" ]; then
        [ "$(sql "$sdir/data.db" "SELECT count(*) FROM _hull_jobs")" = 2 ] \
            || fail "db_registry_audit44.repoint_carries_toplevel_default_tables ($ext): the jobs are not in <state>/data.db (got $(sql "$sdir/data.db" "SELECT count(*) FROM _hull_jobs"))"
        [ "$(sql "$sdir/data.db" "SELECT count(*) FROM _hull_jobs WHERE type = 'top'")" = 1 ] \
            || fail "db_registry_audit44.repoint_carries_toplevel_default_tables ($ext): the top-level job was not carried"
    fi
    [ ! -e "$d/data.db" ] || fail "db_registry_audit44.repoint_carries_toplevel_default_tables ($ext): stray <app>/data.db"
    echo "PASS: db_registry_audit44.repoint_carries_toplevel_default_tables ($ext)"; a44=$((a44 + 1))

    # agent_audit44.db_schema_creates_no_state_db: a read-only `hull agent
    # db schema` makes no state database (nor the state directory).
    if [ "$has_agent" = 1 ]; then
        d="$work/a44-agent-$ext"; a44_app "$d" migr "$ext"
        xdg="$work/xdg-a44-agent-$ext"; sdir="$xdg/hull-e2e-a44m"
        rc=$(XDG_STATE_HOME="$xdg" hull_run "$out" "$HULL_BIN" agent db schema "$d")
        [ ! -e "$sdir/data.db" ] || fail "agent_audit44.db_schema_creates_no_state_db ($ext): made $sdir/data.db"
        [ ! -e "$sdir" ] || fail "agent_audit44.db_schema_creates_no_state_db ($ext): made $sdir"
        echo "PASS: agent_audit44.db_schema_creates_no_state_db ($ext)"; a44=$((a44 + 1))
    fi
done
echo "$((15 + a44)) state directory end-to-end tests passed"
