#!/bin/sh
# e2e_postgres.sh: end-to-end PostgreSQL backend test (§1 Phase 2).
#
# Builds hull with HL_ENABLE_POSTGRES=1, starts a real PostgreSQL in Docker,
# and exercises the full path: postgres:// DSN -> backend selection -> connect
# + startup handshake -> parameterized db.exec / db.query -> typed row decode.
#
# Auth is SCRAM-SHA-256 (Phase 3, the postgres:16 default). Two transports are
# exercised: plaintext (sslmode=disable) and TLS (Phase 3b.2: SSLRequest ->
# mbedTLS handshake -> SCRAM over TLS, asserted via pg_stat_ssl). Skips cleanly
# when Docker (or, for the TLS phase, openssl) is unavailable.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

CONTAINER=hull-pg-e2e
PGPORT=55432
PORT=18091
DSN="postgres://hull:s3cretpw@127.0.0.1:${PGPORT}/hulldb?sslmode=disable"
DSN_TLS="postgres://hull:s3cretpw@127.0.0.1:${PGPORT}/hulldb?sslmode=require"

cleanup() {
    [ -n "${SVR:-}" ] && kill "$SVR" 2>/dev/null || true
    docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
    [ -n "${APPDIR:-}" ] && rm -rf "$APPDIR"
}
trap cleanup EXIT

if ! command -v docker >/dev/null 2>&1; then
    echo "SKIP: docker not available"
    exit 0
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "SKIP: curl not available"
    exit 0
fi

echo "=== building hull with HL_ENABLE_POSTGRES=1 ==="
make HL_ENABLE_POSTGRES=1 >/dev/null

echo "=== starting postgres (scram-sha-256 auth) ==="
docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
docker run -d --name "$CONTAINER" \
    -e POSTGRES_USER=hull -e POSTGRES_DB=hulldb \
    -e POSTGRES_PASSWORD=s3cretpw \
    -p "${PGPORT}:5432" postgres:16-alpine >/dev/null

echo "=== waiting for postgres ==="
ready=0
i=0
while [ "$i" -lt 30 ]; do
    if docker exec "$CONTAINER" pg_isready -U hull -d hulldb >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 1
    i=$((i + 1))
done
[ "$ready" = 1 ] || { echo "FAIL: postgres did not become ready"; exit 1; }

APPDIR=$(mktemp -d)
# A multi-statement migration file: exercises the runner's script path
# (PG simple Query protocol; the extended protocol rejects >1 statement
# per Parse). Auto-runs at startup before app.lua loads.
mkdir -p "$APPDIR/migrations"
cat > "$APPDIR/migrations/001_init.sql" <<'SQL'
CREATE TABLE mig_test (id INTEGER PRIMARY KEY, label TEXT NOT NULL);
CREATE INDEX idx_mig_test_label ON mig_test (label);
INSERT INTO mig_test (id, label) VALUES (1, 'alpha');
INSERT INTO mig_test (id, label) VALUES (2, 'beta');
SQL
cat > "$APPDIR/app.lua" <<'LUA'
app.manifest({ modules = {
    "hull/db@1", "hull/http-server@1", "hull/search@1",
    "hull/web/middleware/session@1", "hull/web/middleware/outbox@1",
    "hull/web/middleware/inbox@1", "hull/web/middleware/rbac@1",
    "hull/web/middleware/audit-log@1", "hull/web/middleware/transaction@1",
    "hull/web/auth-health@1",
} })
local db = require("hull.db").default()
local session = require("hull.web.middleware.session")
local outbox = require("hull.web.middleware.outbox")
local inbox = require("hull.web.middleware.inbox")
local rbac = require("hull.web.middleware.rbac")
local audit_log = require("hull.web.middleware.audit-log")
local transaction = require("hull.web.middleware.transaction")
local auth_health = require("hull.web.auth-health")
local search = require("hull.search")
session.init()
outbox.init()
inbox.init()
rbac.init()
audit_log.init({ fingerprint_salt = "e2e-postgres-fingerprint-salt" })
db.exec("DROP TABLE IF EXISTS e2e")
db.exec("CREATE TABLE e2e (id BIGSERIAL PRIMARY KEY, name TEXT, score INT, active BOOL)")
db.exec("INSERT INTO e2e (name, score, active) VALUES (?, ?, ?)", { "alice", 10, true })
db.exec("INSERT INTO e2e (name, score, active) VALUES (?, ?, ?)", { "bob", 20, false })
db.exec("INSERT INTO e2e (name, score, active) VALUES (?, ?, ?)", { "carol", nil, true })
app.get("/", function(req, res)
    local rows = db.query(
        "SELECT id, name, score, active FROM e2e WHERE score >= ? ORDER BY id", { 0 })
    res:json({ rows = rows, count = db.query("SELECT count(*) AS c FROM e2e")[1].c })
end)
-- migration ran at startup via the runner's script path (multi-statement).
app.get("/migrated", function(req, res)
    local rows = db.query("SELECT label FROM mig_test ORDER BY id")
    res:json({ mig_rows = #rows, first = rows[1] and rows[1].label or nil })
end)
-- db.async: offloads the query to a worker thread (its own PG connection).
app.get("/async", function(req, res)
    local rows = db.async.query(
        "SELECT id, name, active FROM e2e WHERE score >= ? ORDER BY id", { 0 })
    res:json({ rows = rows })
end)
-- stdlib on Postgres: session (dialect DDL + roundtrip), outbox
-- (autoincrement_id_ddl + insert), auth-health (dialect table probe),
-- and search's clear SQLite-only guard.
app.get("/stdlib", function(req, res)
    local sid = session.create({ user_id = "u1", role = "admin" })
    local loaded = session.load(sid)
    outbox.enqueue({ kind = "webhook", destination = "https://x.test", payload = "p" })
    local stats = outbox.stats()
    local health = auth_health.check()
    local search_ok, search_err = pcall(function() return search.query("docs", "x") end)
    res:json({
        backend        = db.backend_name,
        session_role   = loaded and loaded.role or nil,
        outbox_pending = stats.pending,
        sessions_ok    = health.sessions.ok,
        search_guarded = (not search_ok) and (tostring(search_err):find("SQLite") ~= nil) or false,
    })
end)
-- Backend-agnostic DB-backed stdlib on real Postgres: inbox dedup, rbac
-- roles/permissions, audit-log record/list, transaction commit. Proves each
-- module's CREATE TABLE + CRUD run on the PG dialect, not just SQLite.
app.get("/stdlib2", function(req, res)
    local inbox_new = inbox.check_and_mark("evt-1", "webhook")   -- false = new
    local inbox_dup = inbox.check_and_mark("evt-1", "webhook")   -- true  = duplicate

    rbac.define_role("editor", { "posts.write" })
    rbac.assign("u1", "editor")
    local can_write  = rbac.has_permission("u1", "posts.write")
    local can_delete = rbac.has_permission("u1", "posts.delete")

    audit_log.record("u1", "login", req, {})
    local events = audit_log.list("u1", {})

    -- own table so the shared `e2e` fixture's row count stays stable for the
    -- later db.open phase.
    db.exec("CREATE TABLE IF NOT EXISTS stdlib2_txn (id BIGINT)")
    transaction.run(function()
        db.exec("INSERT INTO stdlib2_txn (id) VALUES (?)", { 1 })
    end)
    local txn_count = db.query("SELECT count(*) AS c FROM stdlib2_txn")[1].c

    -- db.insert_if_absent (sibling of upsert; same builder, long column name
    -- exercises the buffer-estimate path): first insert wins, second ignored.
    db.exec("CREATE TABLE IF NOT EXISTS stdlib2_ins (message_key TEXT PRIMARY KEY, val BIGINT)")
    db.insert_if_absent("stdlib2_ins", { "message_key" },
                        { "message_key", "val" }, { "k1", 10 })
    db.insert_if_absent("stdlib2_ins", { "message_key" },
                        { "message_key", "val" }, { "k1", 20 })
    local ins_val = db.query(
        "SELECT val FROM stdlib2_ins WHERE message_key = ?", { "k1" })[1].val

    res:json({
        insert_if_absent = ins_val,
        inbox_new    = inbox_new,
        inbox_dup    = inbox_dup,
        rbac_write   = can_write,
        rbac_delete  = can_delete,
        audit_count  = #events,
        txn_count    = txn_count,
    })
end)
LUA

echo "=== running app against postgres ==="
./build/hull -d "$DSN" --no-sandbox -p "$PORT" "$APPDIR/app.lua" >"$APPDIR/serve.log" 2>&1 &
SVR=$!
sleep 2

RESP=$(curl -fsS "http://127.0.0.1:${PORT}/" || echo FAIL)
echo "response: $RESP"

fail=0
echo "$RESP" | grep -q '"count":3'                 || { echo "::error count != 3"; fail=1; }
echo "$RESP" | grep -q '"name":"alice"'            || { echo "::error alice missing"; fail=1; }
echo "$RESP" | grep -q '"name":"bob"'              || { echo "::error bob missing"; fail=1; }
echo "$RESP" | grep -q '"name":"carol"'            && { echo "::error carol should be filtered out"; fail=1; }
echo "$RESP" | grep -q '"active":true'             || { echo "::error bool decode"; fail=1; }
echo "$RESP" | grep -q '"score":10'                || { echo "::error int decode"; fail=1; }

# migration runner on Postgres (multi-statement file via the script path)
RESP_MIG=$(curl -fsS "http://127.0.0.1:${PORT}/migrated" || echo FAIL)
echo "migrated response: $RESP_MIG"
echo "$RESP_MIG" | grep -q '"mig_rows":2'          || { echo "::error migration rows"; fail=1; }
echo "$RESP_MIG" | grep -q '"first":"alpha"'       || { echo "::error migration seed"; fail=1; }

# db.async path (worker thread opens its own PG connection through the vtable)
RESP_ASYNC=$(curl -fsS "http://127.0.0.1:${PORT}/async" || echo FAIL)
echo "async response: $RESP_ASYNC"
echo "$RESP_ASYNC" | grep -q '"name":"alice"'      || { echo "::error async alice missing"; fail=1; }
echo "$RESP_ASYNC" | grep -q '"name":"bob"'        || { echo "::error async bob missing"; fail=1; }
echo "$RESP_ASYNC" | grep -q '"active":true'       || { echo "::error async bool decode"; fail=1; }
echo "$RESP_ASYNC" | grep -q '"name":"carol"'      && { echo "::error async carol not filtered"; fail=1; }

# stdlib modules on Postgres (session / outbox / auth-health / search guard)
RESP_STDLIB=$(curl -fsS "http://127.0.0.1:${PORT}/stdlib" || echo FAIL)
echo "stdlib response: $RESP_STDLIB"
echo "$RESP_STDLIB" | grep -q '"backend":"postgres"'    || { echo "::error backend not postgres"; fail=1; }
echo "$RESP_STDLIB" | grep -q '"session_role":"admin"'  || { echo "::error session roundtrip"; fail=1; }
echo "$RESP_STDLIB" | grep -q '"outbox_pending":1'      || { echo "::error outbox enqueue"; fail=1; }
echo "$RESP_STDLIB" | grep -q '"sessions_ok":true'      || { echo "::error auth-health table probe"; fail=1; }
echo "$RESP_STDLIB" | grep -q '"search_guarded":true'   || { echo "::error search SQLite-only guard"; fail=1; }

# backend-agnostic DB stdlib on Postgres: inbox / rbac / audit-log / transaction
RESP_STDLIB2=$(curl -fsS "http://127.0.0.1:${PORT}/stdlib2" || echo FAIL)
echo "stdlib2 response: $RESP_STDLIB2"
echo "$RESP_STDLIB2" | grep -q '"inbox_new":false'   || { echo "::error inbox first-seen not new"; fail=1; }
echo "$RESP_STDLIB2" | grep -q '"inbox_dup":true'    || { echo "::error inbox dedup"; fail=1; }
echo "$RESP_STDLIB2" | grep -q '"rbac_write":true'   || { echo "::error rbac has_permission grant"; fail=1; }
echo "$RESP_STDLIB2" | grep -q '"rbac_delete":false' || { echo "::error rbac has_permission deny"; fail=1; }
echo "$RESP_STDLIB2" | grep -q '"audit_count":1'     || { echo "::error audit-log record/list"; fail=1; }
echo "$RESP_STDLIB2" | grep -q '"txn_count":1'       || { echo "::error transaction commit"; fail=1; }
echo "$RESP_STDLIB2" | grep -q '"insert_if_absent":10' || { echo "::error insert_if_absent (first-wins) on pg"; fail=1; }

if [ "$fail" = 0 ]; then
    echo "PASS: postgres backend end-to-end (sync + db.async + stdlib -> typed decode)"
else
    echo "--- server log ---"; cat "$APPDIR/serve.log"
    exit 1
fi

kill "$SVR" 2>/dev/null || true
SVR=

# ── db.open() dynamic connections against Postgres (roadmap §2.2) ──────
# The manifest allowlists the postgres scheme and the 127.0.0.0/8 host CIDR.
# A DSN inside the CIDR opens and queries; a host outside it, and a scheme not
# in the allowlist, are rejected before any connect. Credentials ride in the
# DSN passed as argv[1] so they never sit in the app source. CLI mode (app.main).
echo "=== db.open dynamic allow/deny against postgres ==="
APPDIR_DYN=$(mktemp -d)
cat > "$APPDIR_DYN/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/db@1" },
    databases = { dynamic = { schemes = { "postgres" }, hosts = { "127.0.0.0/8" } } },
})
local db = require("hull.db")
app.main(function(ctx)
    local allow_dsn = ctx.args[1]
    local ok_open, conn = pcall(function() return db.open(allow_dsn) end)
    if ok_open then
        print("allow_count=" .. tostring(conn.query("SELECT count(*) AS c FROM e2e")[1].c))
        -- §2.5: udf is a SQLite-only capability, so a Postgres connection object
        -- must NOT expose a `udf` sub-object at all.
        print("pg_has_udf=" .. tostring(conn.udf ~= nil))
        conn.close()
    else
        print("allow_count=ERR:" .. tostring(conn))
    end
    local ok_host = pcall(function() return db.open("postgres://u@10.9.9.9:5432/db") end)
    print("deny_host=" .. (ok_host and "ok" or "denied"))
    local ok_scheme = pcall(function() return db.open(":memory:") end)
    print("deny_scheme=" .. (ok_scheme and "ok" or "denied"))
    return 0
end)
LUA

DYN_OUT=$(./build/hull --no-sandbox "$APPDIR_DYN/app.lua" -- "$DSN" 2>&1 || true)
echo "$DYN_OUT" | grep -qE "allow_count=3" || { echo "::error db.open allowed host did not query"; echo "$DYN_OUT"; exit 1; }
echo "$DYN_OUT" | grep -qE "deny_host=denied" || { echo "::error db.open out-of-CIDR host not rejected"; echo "$DYN_OUT"; exit 1; }
echo "$DYN_OUT" | grep -qE "deny_scheme=denied" || { echo "::error db.open disallowed scheme not rejected"; echo "$DYN_OUT"; exit 1; }
echo "$DYN_OUT" | grep -qE "pg_has_udf=false" || { echo "::error postgres connection exposes a udf sub-object (should be gated off)"; echo "$DYN_OUT"; exit 1; }
echo "PASS: db.open dynamic connections (CIDR host allow + out-of-CIDR deny + scheme deny)"
rm -rf "$APPDIR_DYN"

# ── TLS phase (Phase 3b.2) ────────────────────────────────────────────
# Enable SSL on the running container with a self-signed cert, then connect
# with sslmode=require and assert (via pg_stat_ssl) the session is encrypted.
if ! command -v openssl >/dev/null 2>&1; then
    echo "SKIP: openssl not available; TLS phase not run"
    exit 0
fi

echo "=== enabling TLS on postgres (self-signed cert) ==="
openssl req -new -x509 -days 1 -nodes -text \
    -subj "/CN=127.0.0.1" \
    -out "$APPDIR/server.crt" -keyout "$APPDIR/server.key" >/dev/null 2>&1

docker cp "$APPDIR/server.crt" "$CONTAINER:/tmp/server.crt" >/dev/null
docker cp "$APPDIR/server.key" "$CONTAINER:/tmp/server.key" >/dev/null
# Postgres refuses a key readable by group/world or not owned by the db user;
# stage the pair inside PGDATA (postgres-owned) with 600 on the key.
docker exec -u root "$CONTAINER" sh -c '
    cp /tmp/server.crt /tmp/server.key /var/lib/postgresql/data/ &&
    chown postgres:postgres /var/lib/postgresql/data/server.crt /var/lib/postgresql/data/server.key &&
    chmod 600 /var/lib/postgresql/data/server.key' >/dev/null

# ssl is SIGHUP-reloadable: ALTER SYSTEM + pg_reload_conf(), no restart.
docker exec -u postgres "$CONTAINER" psql -U hull -d hulldb -q \
    -c "ALTER SYSTEM SET ssl = on;" \
    -c "ALTER SYSTEM SET ssl_cert_file = 'server.crt';" \
    -c "ALTER SYSTEM SET ssl_key_file = 'server.key';" \
    -c "SELECT pg_reload_conf();" >/dev/null
sleep 2

APPDIR_TLS=$(mktemp -d)
cat > "$APPDIR_TLS/app.lua" <<'LUA'
app.manifest({ modules = { "hull/db@1", "hull/http-server@1" } })
local db = require("hull.db").default()
app.get("/", function(req, res)
    -- pg_stat_ssl.ssl is true only when THIS backend connection is over TLS.
    local r = db.query("SELECT ssl FROM pg_stat_ssl WHERE pid = pg_backend_pid()")
    local n = db.query("SELECT count(*) AS c FROM e2e")
    res:json({ ssl = r[1].ssl, count = n[1].c })
end)
LUA

echo "=== running app over TLS (sslmode=require) ==="
./build/hull -d "$DSN_TLS" --no-sandbox -p "$PORT" "$APPDIR_TLS/app.lua" >"$APPDIR_TLS/serve.log" 2>&1 &
SVR=$!
sleep 2

RESP_TLS=$(curl -fsS "http://127.0.0.1:${PORT}/" || echo FAIL)
echo "response: $RESP_TLS"

tfail=0
echo "$RESP_TLS" | grep -q '"ssl":true' || { echo "::error connection not over TLS"; tfail=1; }
echo "$RESP_TLS" | grep -q '"count":3'  || { echo "::error query over TLS failed"; tfail=1; }

if [ "$tfail" = 0 ]; then
    echo "PASS: postgres TLS end-to-end (SSLRequest -> handshake -> SCRAM over TLS -> encrypted query)"
    rm -rf "$APPDIR_TLS"
else
    echo "--- server log ---"; cat "$APPDIR_TLS/serve.log" 2>/dev/null || true
    exit 1
fi
