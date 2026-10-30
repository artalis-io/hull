#!/bin/sh
# e2e_feature_duckdb.sh - DuckDB as a composable feature, end to end.
#
# Builds a BASE hull (EMBED_PLATFORM=1, no DuckDB compiled in) + the DuckDB
# feature archive, then `hull build --with=duckdb` an app and runs it. Proves
# that `hull build` composes the feature lib + a generated registry into the app
# binary so db_select routes duckdb:// to the composed backend -- while the base
# stays DuckDB-free. A plain app must NOT get the feature. See
# docs/features_and_flavors.md.
#
# Must run on a fresh build tree (no prior HL_ENABLE_DUCKDB=1 objects), so it
# lives in its own CI job. Requires `make fetch-duckdb` to have run.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."

echo "=== build base hull (EMBED_PLATFORM=1; base is DuckDB-free) ==="
make EMBED_PLATFORM=1 >/dev/null
# Stash the base hull: `make feature-duckdb` re-invokes make with
# HL_ENABLE_DUCKDB=1, and the build-config sentinel cleans build/ on the flag
# flip (EMBED_PLATFORM is fingerprinted since #179), which would wipe build/hull.
# Save the build-tool binary first so it survives the feature-archive build
# (mirrors e2e_feature_gpu.sh).
cp build/hull /tmp/hull_base_duckdb_e2e

echo "=== build the DuckDB feature archive ==="
make feature-duckdb >/dev/null
ls -la build/libhull_feature-duckdb.a

HULL=/tmp/hull_base_duckdb_e2e
# hull build takes a local feature archive only from the running hull's own
# directory (never ./build), so stage it beside the copied binary.
cp build/libhull_feature-duckdb.a "$(dirname "$HULL")/"
APP=$(mktemp -d)
PLAIN=$(mktemp -d)
trap 'rm -rf "$APP" "$PLAIN" /tmp/hull_base_duckdb_e2e' EXIT

cat > "$APP/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/db@1" },
    databases = { named = { analytics = "duckdb://:memory:",
                            filedb = "duckdb://data/fdb.duckdb" } },
    -- The file database lives in a granted directory. app.main hands DuckDB
    -- no fs policy, so it still runs with external access off (mode A).
    fs = { read = { "data/" }, write = { "data/" } },
})
app.main(function()
    local d = require("hull.db").connect("analytics")
    local r = d.query("SELECT 42 AS answer")
    assert(r and r[1] and r[1].answer == 42, "duckdb query failed")
    -- Audit 35: a string of up to 12 bytes is inline in DuckDB's vector;
    -- the decoder pointed the value into a dead stack copy, so short columns
    -- of one row read each other's bytes.
    local s = d.query("SELECT 'x1' AS a, 42 AS n, 'y2' AS b, 'alice' AS c, " ..
                      "'a-long-string-over-twelve' AS e, 'bob' AS f")[1]
    assert(s.a == "x1" and s.n == 42 and s.b == "y2" and s.c == "alice"
           and s.e == "a-long-string-over-twelve" and s.f == "bob",
           "short strings: " .. tostring(s.a) .. "," .. tostring(s.b) .. "," ..
           tostring(s.c) .. "," .. tostring(s.f))
    -- Audit 32: statements that run SQL from a file or copy every table
    -- without naming one are refused by Hull itself (not only by DuckDB's
    -- external-access setting), in any statement of the text.
    for _, sql in ipairs({
        "IMPORT DATABASE 'imp'",
        "EXPORT DATABASE 'exp'",
        "ATTACH ':memory:' AS c; COPY FROM DATABASE memory TO c",
        "PRAGMA import_database('imp')",
        "SELECT 1; /* x */ export -- y\n database 'exp'",
    }) do
        local ok, err = pcall(d.exec, sql)
        assert(not ok and tostring(err):find("COPY FROM DATABASE are not available", 1, true),
               "not refused by Hull: " .. sql .. " -> " .. tostring(err))
    end
    -- Audit 33: ATTACH / DETACH / USE / SET search_path|schema switch the
    -- catalog an unqualified name resolves to, which would read _hull_* tables
    -- through a cross-catalog view with no _hull_ in the app SQL. Refused by
    -- Hull itself, in any statement of the text.
    for _, sql in ipairs({
        "ATTACH ':memory:' AS c2",
        "USE c2",
        "SET schema = 'c2.main'",
        "SET search_path = 'a,b'",
        "RESET schema",
        "SELECT 1; /* x */ attach -- y\n ':memory:' AS c3",
    }) do
        local ok, err = pcall(d.exec, sql)
        assert(not ok and tostring(err):find("switch catalogs", 1, true),
               "not refused by Hull: " .. sql .. " -> " .. tostring(err))
    end
    -- Audit 34: DuckDB keeps the database file and its WAL / temp files
    -- readable from SQL whatever external access says, so read_blob of the
    -- file returned its raw bytes, _hull_* rows included. And a table name
    -- built at run time reached storage_info (column min / max values),
    -- pragma_table_info and histogram. Both refused by Hull itself.
    local f = require("hull.db").connect("filedb")
    f.exec("CREATE TABLE IF NOT EXISTS users (name VARCHAR, n INTEGER)")
    f.exec("INSERT INTO users VALUES ('alice', 7)")
    f.exec("CHECKPOINT")
    f.exec("INSERT INTO users VALUES ('bob', 9)")   -- in the WAL
    local rows = f.query("SELECT count(*) AS c FROM users")
    assert(rows[1].c >= 2, "file database did not take writes")
    for _, sql in ipairs({
        "SELECT octet_length(content) AS n FROM read_blob('data/fdb.duckdb')",
        "SELECT content FROM read_text('data/fdb.duckdb.wal')",
        "SELECT content FROM read_blob('./data/FDB.DUCKDB')",
        "SELECT content FROM read_blob(concat('fdb.', 'duckdb'))",
        "SELECT content FROM read_blob(E'\\x66db.duckdb')",
        "SELECT * FROM read_csv('x.csv')",            -- no fs policy: none
        "SELECT * FROM glob('*')",
        "COPY users TO 'data/fdb.duckdb.wal'",
        "COPY users FROM (concat('fdb.', 'duckdb'))",
    }) do
        local ok, err = pcall(f.query, sql)
        assert(not ok and tostring(err):find("may not name the database file", 1, true),
               "not refused by Hull: " .. sql .. " -> " .. tostring(err))
    end
    for _, sql in ipairs({
        "SELECT * FROM pragma_storage_info(concat('us', 'ers'))",
        "SELECT * FROM pragma_storage_info('users')",
        "PRAGMA storage_info('users')",
        "CALL pragma_storage_info(chr(117) || 'sers')",
        "SELECT * FROM pragma_table_info(concat('_', 'hull_sessions'))",
        "PRAGMA table_info(chr(117) || 'sers')",
        "SELECT * FROM pragma_show(concat('us', 'ers'))",
        "SELECT * FROM histogram(concat('us', 'ers'), name)",
        "CALL enable_logging(level = 'debug')",
        -- Audit 36: a catalog / schema qualifier hid the macro's name from
        -- the table-position check, and DuckDB ran it.
        "SELECT * FROM main.histogram(concat('us', 'ers'), name)",
        "SELECT * FROM system.main.histogram(concat('us', 'ers'), name)",
        "SELECT * FROM main.\"histogram\"(concat('us', 'ers'), name)",
        "SELECT * FROM main /* c */ . histogram(concat('us', 'ers'), name)",
        "SELECT * FROM main.pragma_table_info(concat('us', 'ers'))",
        "CALL system.main.pragma_storage_info('users')",
    }) do
        local ok, err = pcall(f.query, sql)
        assert(not ok and tostring(err):find("plain literal or name", 1, true),
               "not refused by Hull: " .. sql .. " -> " .. tostring(err))
    end
    -- A literal or plain name stays usable, as does the aggregate histogram.
    assert(#f.query("SELECT * FROM pragma_table_info('users')") == 2)
    assert(#f.query("PRAGMA table_info(users)") == 2)
    assert(#f.query("SELECT histogram(n) AS h FROM users") == 1)
    -- A qualified macro with a plain source, and a qualified table, run.
    assert(#f.query("SELECT * FROM main.histogram(users, n)") >= 1)
    assert(#f.query("SELECT * FROM main.users") >= 2)
    for _, sql in ipairs({
        "SELECT * FROM system.main.query_table(concat('us', 'ers'))",
        "SELECT * FROM main.\"query\"('SELECT 1')",
    }) do
        local ok = pcall(f.query, sql)
        assert(not ok, "qualified dynamic SQL not refused: " .. sql)
    end
    print("DUCKDB FEATURE APP OK")
    return 0
end)
LUA

echo "=== hull build --with=duckdb (system compiler: features are C++) ==="
BUILD_OUT=$("$HULL" build --compiler=system --with=duckdb --no-verify-platform -o "$APP/bin" "$APP" 2>&1) || true
echo "$BUILD_OUT"
echo "$BUILD_OUT" | grep -q "composed feature 'duckdb'" || { echo "FAIL: feature not composed"; exit 1; }
# From the app directory: the file database (data/fdb.duckdb) is relative to it.
mkdir -p "$APP/data"
RUN_OUT=$(cd "$APP" && ./bin 2>&1) || true
echo "$RUN_OUT"
if ! echo "$RUN_OUT" | grep -q "DUCKDB FEATURE APP OK"; then
    echo "--- app run failed; diagnostics ---"
    file "$APP/bin" 2>/dev/null || true
    command -v ldd >/dev/null 2>&1 && ldd "$APP/bin" 2>&1 | head || true
    echo "--- retry with --no-sandbox (isolates sandbox vs link/static-init) ---"
    if (cd "$APP" && ./bin --hull-no-sandbox) 2>&1 | grep -q "DUCKDB FEATURE APP OK"; then
        echo "DIAG: works with --no-sandbox -> a sandboxed syscall blocks DuckDB init"
        if command -v strace >/dev/null 2>&1; then
            echo "--- strace (sandboxed run): syscalls that returned an error ---"
            strace -f -o /tmp/ddb_strace.txt "$APP/bin" >/dev/null 2>&1 || true
            grep -aE "= -1 E" /tmp/ddb_strace.txt | grep -av readlink | tail -25 || true
        fi
    else
        echo "DIAG: still fails with --no-sandbox -> link/static-init, not sandbox"
    fi
    echo "FAIL: app did not run duckdb"; exit 1
fi
echo "ok  --with=duckdb composed + ran"

echo "=== an extensionless database name: migrations run, ordinary SQL passes ==="
# Audit 35: #912 refused every statement holding the database file's name as
# a substring. With `duckdb://data/hull` every _hull_migrations statement was
# refused (the app could not boot); `"show" = 1` and histogram(lower(x)) were
# refused as table-name values. The name is read only in literals, on path
# component boundaries, and show / histogram by position.
EXT=$(mktemp -d)
mkdir -p "$EXT/migrations" "$EXT/data"
cat > "$EXT/migrations/001_init.sql" <<'SQL'
CREATE TABLE hull_items (id INTEGER, name VARCHAR, "show" INTEGER);
INSERT INTO hull_items VALUES (1, 'Alpha', 0), (2, 'beta', 0);
SQL
cat > "$EXT/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/db@1" },
    fs = { read = { "data/" }, write = { "data/" } },
})
app.main(function()
    local d = require("hull.db").default()
    d.exec([[UPDATE hull_items SET "show" = 1 WHERE id = ?]], { 1 })
    local r = d.query([[SELECT count(*) AS c FROM hull_items WHERE "show" = ?]], { 1 })
    assert(r[1].c == 1, "show column update")
    assert(#d.query("SELECT id, histogram(lower(name)) AS h FROM hull_items GROUP BY id") == 2,
           "aggregate histogram")
    for _, sql in ipairs({
        "SELECT content FROM read_blob('data/hull')",
        "SELECT content FROM read_text('./data/HULL.wal')",
        "COPY hull_items TO 'data/hull.wal'",
    }) do
        local ok, err = pcall(d.query, sql)
        assert(not ok and tostring(err):find("may not name the database file", 1, true),
               "not refused by Hull: " .. sql .. " -> " .. tostring(err))
    end
    print("EXTENSIONLESS DUCKDB OK")
    return 0
end)
LUA
EXT_BUILD=$("$HULL" build --compiler=system --with=duckdb --no-verify-platform -o "$EXT/bin" "$EXT" 2>&1) || true
echo "$EXT_BUILD" | grep -q "composed feature 'duckdb'" || { echo "$EXT_BUILD"; echo "FAIL: extensionless app not composed"; exit 1; }
EXT_OUT=$(cd "$EXT" && ./bin --hull-d duckdb://data/hull 2>&1) || true
echo "$EXT_OUT" | tail -5
rm -rf "$EXT"
echo "$EXT_OUT" | grep -q "EXTENSIONLESS DUCKDB OK" || { echo "FAIL: duckdb://data/hull app did not run its migrations and SQL"; exit 1; }
echo "ok  duckdb://data/hull: migrations ran, ordinary SQL passed, the file stayed refused"

echo "=== a long DuckDB query is stopped by the run's instruction budget ==="
# Audit 15: the statement ran inside one duckdb_execute_prepared call, which no
# budget or watchdog saw - this query held the thread for minutes. It now runs
# task by task, charged to the run, and is interrupted once over the limit (or
# past the run's deadline, while DuckDB's own threads hold the tasks).
LONG=$(mktemp -d)
cat > "$LONG/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/db@1" },
    databases = { named = { analytics = "duckdb://:memory:" } },
})
app.main(function()
    local d = require("hull.db").connect("analytics")
    d.query("SELECT count(*) AS n FROM range(100000000000) t(i) WHERE i % 7 = 3")
    print("LONG QUERY FINISHED")
    return 0
end)
LUA
LONG_BUILD=$("$HULL" build --compiler=system --with=duckdb --no-verify-platform -o "$LONG/bin" "$LONG" 2>&1) || true
echo "$LONG_BUILD" | grep -q "composed feature 'duckdb'" || { echo "$LONG_BUILD"; echo "FAIL: long-query app not composed"; exit 1; }
T0=$(date +%s)
if command -v timeout >/dev/null 2>&1; then
    LONG_OUT=$(timeout 120 "$LONG/bin" --hull-max-instructions 5000000 --hull-max-run-ms 20000 2>&1) || true
else
    LONG_OUT=$("$LONG/bin" --hull-max-instructions 5000000 --hull-max-run-ms 20000 2>&1) || true
fi
T1=$(date +%s)
rm -rf "$LONG"
echo "$LONG_OUT" | tail -5
if echo "$LONG_OUT" | grep -q "LONG QUERY FINISHED"; then
    echo "FAIL: the long DuckDB query ran to completion past the limit"; exit 1
fi
echo "$LONG_OUT" | grep -Eq "instruction limit exceeded|run exceeded its time limit" || { echo "FAIL: long DuckDB query not stopped by the run's limits"; exit 1; }
[ $((T1 - T0)) -lt 60 ] || { echo "FAIL: stopping the query took $((T1 - T0)) s"; exit 1; }
echo "ok  long DuckDB query interrupted in $((T1 - T0)) s"

echo "=== negative: a plain sqlite app must NOT compose duckdb ==="
printf 'app.manifest({modules={"hull/db@1"}})\napp.main(function() print("PLAIN OK") return 0 end)\n' \
    > "$PLAIN/app.lua"
PLAIN_OUT=$("$HULL" build --no-verify-platform -o "$PLAIN/bin" "$PLAIN" 2>&1) || true
if echo "$PLAIN_OUT" | grep -q "composed feature"; then
    echo "$PLAIN_OUT"; echo "FAIL: composed a feature for a plain app"; exit 1
fi
"$PLAIN/bin" 2>&1 | grep -q "PLAIN OK" || { echo "FAIL: plain app did not run"; exit 1; }
echo "ok  plain app stayed DuckDB-free"

echo "=== negative: tcc cannot link a C++ feature (must be rejected) ==="
# On Linux the guard fires; on macOS tcc is unavailable -- either way, no binary.
if "$HULL" build --compiler=tcc --with=duckdb --no-verify-platform -o "$APP/bin_tcc" "$APP" >/dev/null 2>&1; then
    echo "FAIL: tcc produced a C++ feature binary (should be rejected)"; exit 1
fi
echo "ok  tcc + feature rejected"

echo "PASS: DuckDB feature composed into an app binary; base stays clean"
