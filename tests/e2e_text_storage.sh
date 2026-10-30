#!/bin/sh
# e2e_text_storage.sh - audit 39 on SQLite: request / app text the stdlib
# stores (an idempotent response echoing request bytes, a User-Agent with
# obs-text bytes, a filename that is not UTF-8) is stored safely, both
# runtimes. The same apps run against Postgres / MySQL in e2e_postgres.sh /
# e2e_mysql.sh (tests/lib/hull_text_audit39.sh), where the raw text was
# refused after the side effect.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PORT_LUA=${A39_PORT_LUA:-18393}
PORT_JS=${A39_PORT_JS:-18394}

if [ ! -x "$HULL" ]; then
    echo "e2e_text_storage: hull binary not found at $HULL - run 'make' first"
    exit 1
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "SKIP: curl not available"
    exit 0
fi

. "$SRCDIR/tests/lib/hull_text_audit39.sh"

WORK=$(mktemp -d 2>/dev/null || mktemp -d -t hull_a39)
trap 'rm -rf "$WORK"' EXIT
text_audit39_apps "$WORK"

rc=0
echo "=== audit 39: stored request / app text (SQLite, Lua) ==="
if text_audit39_run "$HULL" "$WORK/l/app.lua" "$PORT_LUA" lua; then
    echo "PASS: Lua"
else
    echo "FAIL: Lua"; rc=1
fi
echo "=== audit 39: stored request / app text (SQLite, JS) ==="
if text_audit39_run "$HULL" "$WORK/j/app.js" "$PORT_JS" js; then
    echo "PASS: JS"
else
    echo "FAIL: JS"; rc=1
fi
exit $rc
