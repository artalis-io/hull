#!/bin/sh
# e2e_db_internal.sh: E2E for manifest `databases.internal`.
#
# The stdlib keeps its own _hull_* tables (sessions, auth state, rbac, ...) on
# the internal connection when one is declared, so they can live under a
# database role the app's connection has no grants on. This proves, in both
# runtimes, that session.init() - called from app code before the manifest is
# wired - creates _hull_sessions in the INTERNAL database and not in the app's
# default one, and that app code cannot reach the internal connection.
# SQLite files stand in for the two databases; Python's sqlite3 inspects them.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

PY=""
for p in python3 python; do
    if command -v "$p" >/dev/null 2>&1 && "$p" -c "import sqlite3" >/dev/null 2>&1; then
        PY="$p"; break
    fi
done
if [ -z "$PY" ]; then
    echo "SKIP: e2e_db_internal needs python with sqlite3 to inspect the databases"
    exit 0
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# A path Python can open too (on Windows MSYS paths are not visible to it).
WTMP=$(cd "$TMPDIR" && (pwd -W 2>/dev/null || pwd))

has_table() {
    "$PY" -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); \
print(1 if c.execute(\"SELECT 1 FROM sqlite_master WHERE name='_hull_sessions'\").fetchone() else 0)" "$1"
}

run_case() {
    ext="$1"; label="$2"
    dir="$TMPDIR/$ext"
    default_db="$WTMP/$ext-app.db"
    internal_db="$WTMP/$ext-internal.db"
    out=$(cd "$dir" && "$OLDPWD/$HULL" --no-sandbox -d "$default_db" "app.$ext" 2>&1 || true)

    [ -f "$internal_db" ] && [ "$(has_table "$internal_db")" = 1 ] \
        && pass "$label: _hull_sessions is created in the internal database" \
        || fail "$label: _hull_sessions is created in the internal database" "$out"
    [ "$(has_table "$default_db")" = 0 ] \
        && pass "$label: the app's database has no _hull_sessions" \
        || fail "$label: the app's database has no _hull_sessions" "$out"
    case "$out" in
        *"app_reach=false"*) pass "$label: app code cannot require the internal connection" ;;
        *) fail "$label: app code cannot require the internal connection" "$out" ;;
    esac
}

mkdir -p "$TMPDIR/lua" "$TMPDIR/js"
cat > "$TMPDIR/lua/app.lua" <<EOF
app.manifest({
    modules = { "hull/web/middleware/session@1" },
    databases = { internal = "$WTMP/lua-internal.db" },
})
local session = require("hull.web.middleware.session")
app.main(function(ctx)
    session.init({})
    local ok = pcall(require, "hull.db._internal")
    ctx.stdout:write("app_reach=" .. tostring(ok) .. "\n")
    return 0
end)
EOF

cat > "$TMPDIR/js/app.js" <<EOF
import { app } from "hull:app";
import { session } from "hull:web:middleware:session";
app.manifest({
    modules: ["hull/web/middleware/session@1"],
    databases: { internal: "$WTMP/js-internal.db" },
});
app.main((ctx) => {
    session.init({});
    ctx.stdout.write("app_reach=false\n");   /* a static import of it would fail app load */
    return 0;
});
EOF

echo "=== databases.internal ==="
run_case lua "lua"
run_case js "js"

# `hull build` reads the manifest by running the app's top level in a VM with
# no database, where the internal-connection module is a stand-in. A stdlib
# module that takes the connection at load, and an init() called at top level,
# must not stop the manifest from being read there.
echo "=== build-time manifest extraction ==="
mkdir -p "$TMPDIR/xlua" "$TMPDIR/xjs"
cat > "$TMPDIR/xlua/app.lua" <<'EOF2'
app.manifest({ modules = { "hull/web/middleware/session@1" } })
local session = require("hull.web.middleware.session")
session.init({})
app.main(function() return 0 end)
EOF2
cat > "$TMPDIR/xjs/app.js" <<'EOF2'
import { app } from "hull:app";
import { session } from "hull:web:middleware:session";
app.manifest({ modules: ["hull/web/middleware/session@1"] });
session.init({});
app.main(() => 0);
EOF2
# Lua extraction keeps a manifest declared before a later top-level error, so
# what matters there is that the stdlib module loads at all.
out=$("$HULL" manifest "$TMPDIR/xlua" 2>&1 || true)
case "$out" in
    *"module not found"*) fail "lua: the stdlib loads while the manifest is read" "$out" ;;
    *) pass "lua: the stdlib loads while the manifest is read" ;;
esac
out=$("$HULL" manifest "$TMPDIR/xjs" 2>&1 || true)
case "$out" in
    *'"hull/web/middleware/session@1"'*) pass "js: the manifest is read past a top-level init()" ;;
    *) fail "js: the manifest is read past a top-level init()" "$out" ;;
esac

echo ""
echo "e2e_db_internal: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
