#!/bin/sh
# E2E tests — SQL migration pipeline
#
# Usage: sh tests/e2e_migrate.sh
# Requires: build/hull already built
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL=./build/hull
PASS=0
FAIL=0

if [ ! -x "$HULL" ]; then
    echo "e2e-migrate: hull binary not found at $HULL — run 'make' first"
    exit 1
fi

fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

echo ""
echo "=== E2E: SQL Migrations ==="

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# ── Test 1: hull migrate new creates sequential files ─────────────

echo ""
echo "--- hull migrate new ---"

mkdir -p "$TMPDIR/app1"
cat > "$TMPDIR/app1/app.lua" << 'EOF'
app.get("/", function(_req, res)
    res:json({ status = "ok" })
end)
EOF

$HULL migrate new init "$TMPDIR/app1" 2>&1
if [ -f "$TMPDIR/app1/migrations/001_init.sql" ]; then
    pass "migrate new creates 001_init.sql"
else
    fail "migrate new did not create 001_init.sql"
fi

$HULL migrate new add_users "$TMPDIR/app1" 2>&1
if [ -f "$TMPDIR/app1/migrations/002_add_users.sql" ]; then
    pass "migrate new creates 002_add_users.sql"
else
    fail "migrate new did not create 002_add_users.sql"
fi

# ── Test 2: hull migrate runs pending SQL files ──────────────────

echo ""
echo "--- hull migrate (run) ---"

mkdir -p "$TMPDIR/app2/migrations"
cat > "$TMPDIR/app2/app.lua" << 'EOF'
app.get("/", function(_req, res)
    res:json({ status = "ok" })
end)
EOF

cat > "$TMPDIR/app2/migrations/001_init.sql" << 'EOF'
CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL
);
EOF

cat > "$TMPDIR/app2/migrations/002_seed.sql" << 'EOF'
INSERT INTO users (name) VALUES ('alice');
INSERT INTO users (name) VALUES ('bob');
EOF

OUTPUT=$($HULL migrate -d "$TMPDIR/app2/data.db" "$TMPDIR/app2" 2>&1)
case "$OUTPUT" in
    *"applied 2 migration"*)
        pass "migrate applies 2 pending migrations" ;;
    *)
        fail "migrate did not apply 2 migrations: $OUTPUT" ;;
esac

# ── Test 3: hull migrate is idempotent ───────────────────────────

OUTPUT=$($HULL migrate -d "$TMPDIR/app2/data.db" "$TMPDIR/app2" 2>&1)
case "$OUTPUT" in
    *"already up to date"*)
        pass "migrate is idempotent" ;;
    *)
        fail "migrate not idempotent: $OUTPUT" ;;
esac

# ── Test 4: hull migrate status ──────────────────────────────────

echo ""
echo "--- hull migrate status ---"

OUTPUT=$($HULL migrate status -d "$TMPDIR/app2/data.db" "$TMPDIR/app2" 2>&1)
case "$OUTPUT" in
    *"[x] 001_init.sql"*)
        pass "status shows 001_init.sql as applied" ;;
    *)
        fail "status missing 001_init.sql: $OUTPUT" ;;
esac
case "$OUTPUT" in
    *"[x] 002_seed.sql"*)
        pass "status shows 002_seed.sql as applied" ;;
    *)
        fail "status missing 002_seed.sql: $OUTPUT" ;;
esac

# ── Test 5: SQL error prevents migration ─────────────────────────

echo ""
echo "--- SQL error handling ---"

mkdir -p "$TMPDIR/app3/migrations"
cat > "$TMPDIR/app3/app.lua" << 'EOF'
app.get("/", function(_req, res)
    res:json({ status = "ok" })
end)
EOF

cat > "$TMPDIR/app3/migrations/001_bad.sql" << 'EOF'
CREATE TABLE this_is_valid (id INTEGER);
INVALID SQL SYNTAX HERE;
EOF

OUTPUT=$($HULL migrate -d "$TMPDIR/app3/data.db" "$TMPDIR/app3" 2>&1 || true)
case "$OUTPUT" in
    *"failed"*)
        pass "SQL error is reported" ;;
    *)
        fail "SQL error not caught: $OUTPUT" ;;
esac

# ── Test 6: No migrations directory is not an error ──────────────

echo ""
echo "--- no migrations dir ---"

mkdir -p "$TMPDIR/app4"
cat > "$TMPDIR/app4/app.lua" << 'EOF'
app.get("/", function(_req, res)
    res:json({ status = "ok" })
end)
EOF

OUTPUT=$($HULL migrate -d "$TMPDIR/app4/data.db" "$TMPDIR/app4" 2>&1)
case "$OUTPUT" in
    *"no migrations found"*)
        pass "no migrations dir is not an error" ;;
    *)
        fail "unexpected output with no migrations dir: $OUTPUT" ;;
esac

# ── Test 7: hull migrate new validates name ──────────────────────

echo ""
echo "--- name validation ---"

OUTPUT=$($HULL migrate new "bad name!" "$TMPDIR/app1" 2>&1 || true)
case "$OUTPUT" in
    *"invalid name"*)
        pass "rejects invalid migration name" ;;
    *)
        fail "accepted invalid name: $OUTPUT" ;;
esac

# ── Test 8: Auto-run on app startup ──────────────────────────────

echo ""
echo "--- auto-run on startup ---"

mkdir -p "$TMPDIR/app5/migrations"
cat > "$TMPDIR/app5/app.lua" << 'EOF'
app.get("/", function(_req, res)
    local rows = db.query("SELECT name FROM users ORDER BY name")
    res:json(rows)
end)
EOF

cat > "$TMPDIR/app5/migrations/001_schema.sql" << 'EOF'
CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL);
INSERT INTO users (name) VALUES ('alice');
EOF

# Start server briefly — migrations should auto-run
$HULL -p 18931 -d "$TMPDIR/app5/data.db" "$TMPDIR/app5/app.lua" &
SERVER_PID=$!

# Wait for server
for i in 1 2 3 4 5 6 7 8 9 10; do
    if curl -s "http://127.0.0.1:18931/" >/dev/null 2>&1; then
        break
    fi
    sleep 0.5
done

RESP=$(curl -s "http://127.0.0.1:18931/")
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

case "$RESP" in
    *"alice"*)
        pass "auto-run migrations apply on startup" ;;
    *)
        fail "auto-run did not apply: $RESP" ;;
esac

# ── Test 9: --no-migrate skips auto-run ──────────────────────────

echo ""
echo "--- --no-migrate flag ---"

mkdir -p "$TMPDIR/app6/migrations"
cat > "$TMPDIR/app6/app.lua" << 'EOF'
app.get("/", function(_req, res)
    res:json({ status = "ok" })
end)
EOF

cat > "$TMPDIR/app6/migrations/001_schema.sql" << 'EOF'
CREATE TABLE test_table (id INTEGER PRIMARY KEY);
EOF

# Start server with --no-migrate
$HULL -p 18932 -d "$TMPDIR/app6/data.db" --no-migrate "$TMPDIR/app6/app.lua" &
SERVER_PID=$!

for i in 1 2 3 4 5 6 7 8 9 10; do
    if curl -s "http://127.0.0.1:18932/" >/dev/null 2>&1; then
        break
    fi
    sleep 0.5
done

kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

# Check that _hull_migrations table does NOT exist (migration was skipped)
TABLES=$(sqlite3 "$TMPDIR/app6/data.db" ".tables" 2>/dev/null || echo "")
case "$TABLES" in
    *"_hull_migrations"*)
        fail "--no-migrate did not skip migrations" ;;
    *)
        pass "--no-migrate skips auto-run" ;;
esac

# ── Summary ───────────────────────────────────────────────────────

echo ""
echo "$PASS passed, $FAIL failed"
if [ $FAIL -gt 0 ]; then
    exit 1
fi
