#!/bin/sh
# Database-open errors must exit cleanly before loading either runtime.
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
HULL_BIN="${HULL_BIN:-build/hull}"
. "$(dirname "$0")/lib/hull_rc.sh"
hull_rc_init "$HULL_BIN"
work=$(mktemp -d)
trap 'rm -f "$work/output"; rmdir "$work"' EXIT
dbdir="$work"
if [ "$HULL_RC_SHIFT" = 1 ]; then dbdir=$(cygpath -m "$work"); fi

for ext in lua js; do
    for kind in missing-parent directory; do
        case "$kind" in
            missing-parent) dbpath="$dbdir/missing/database.db" ;;
            directory) dbpath="$dbdir" ;;
        esac
        rc=$(hull_run "$work/output" "$HULL_BIN" \
            "examples/hello_cli/app.$ext" -d "$dbpath" -- world)
        if [ "$rc" != 1 ]; then
            echo "FAIL: $ext $kind database error: expected exit 1, got $rc"
            cat "$work/output"
            exit 1
        fi
        if ! grep -q 'sqlite open failed' "$work/output" ||
           ! grep -q 'app context init failed' "$work/output"; then
            echo "FAIL: $ext $kind database-open diagnostics missing"
            cat "$work/output"
            exit 1
        fi
        if grep -q 'hello world' "$work/output"; then
            echo "FAIL: $ext $kind ran app.main after database-open failure"
            exit 1
        fi
        echo "PASS: $ext $kind database error exits cleanly without running app.main"
    done
done
echo '4/4 database-open failure tests passed'
