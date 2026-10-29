#!/bin/sh
# e2e_db_live_files.sh: an app cannot truncate a database file SQLite holds
# open (audit 33).
#
# SQLite maps a WAL database's index (<db>-shm) MAP_SHARED in every
# connection, outside the fs.mmap SIGBUS guard. compute.stream's file output
# and an fs.write through an in-root symlink both truncated it in place, and
# the next query killed the process with SIGBUS (exit 138 / 135). Each run
# below must exit 0: the writes refused with database_file_in_use, the
# database intact.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

HULL="${HULL:-build/hull}"
HULL_ABS="$(cd "$(dirname "$HULL")" && pwd)/$(basename "$HULL")"
FIX="$(cd "$(dirname "$0")" && pwd)/fixtures/db_live_files"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/hull-db-live.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

echo "=== e2e: live SQLite files are not truncated, replaced or removed ==="

for rt in lua js; do
    dir="$WORK/$rt"
    mkdir -p "$dir/data" "$dir/compute"
    cp "$FIX/app.$rt" "$dir/"
    cp "$FIX/compute/echo.wasm" "$dir/compute/"
    ln -s app.db-shm "$dir/data/shmlink"

    rc=0
    (cd "$dir" && "$HULL_ABS" -d data/app.db "app.$rt") > "$dir/out.log" 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        fail "$rt: exit $rc (138 / 135 = SIGBUS)"
        sed 's/^/      /' "$dir/out.log" | tail -15
        continue
    fi
    if grep -q "STREAM false.*database_file_in_use\|STREAM nil database_file_in_use" "$dir/out.log" &&
       grep -q "LINK .*database_file_in_use" "$dir/out.log" &&
       grep -q "WAL .*database_file_in_use" "$dir/out.log" &&
       grep -q "AFTER 1" "$dir/out.log"; then
        pass "$rt: stream / symlink write / -wal replace refused, database intact"
    else
        fail "$rt: unexpected output"
        sed 's/^/      /' "$dir/out.log" | tail -15
    fi
done

echo ""
echo "e2e_db_live_files: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
