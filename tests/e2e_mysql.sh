#!/bin/sh
# e2e_mysql.sh: end-to-end MySQL/MariaDB backend test (§2.10 Phase 6).
#
# Builds hull with HL_ENABLE_MYSQL=1, starts a real MySQL 8 in Docker, and
# exercises the full path: mysql:// DSN -> backend selection -> connect +
# handshake -> parameterized db.exec / db.query via the binary prepared-stmt
# protocol -> typed row decode, plus multi-statement migrations, db.async on a
# worker connection, and the DB-backed stdlib (session / outbox / inbox / rbac /
# audit-log / transaction / insert_if_absent) on the MySQL dialect.
#
# Two auth paths: mysql_native_password over plaintext (main phase), and
# caching_sha2_password full-auth over TLS (TLS phase: MySQL 8 ships TLS on by
# default with auto-generated certs). Skips cleanly when Docker is unavailable.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

CONTAINER=hull-mysql-e2e
MYPORT=55336
PORT=18092
DSN="mysql://hull:s3cretpw@127.0.0.1:${MYPORT}/hulldb?sslmode=disable"
DSN_TLS="mysql://hull:s3cretpw@127.0.0.1:${MYPORT}/hulldb?sslmode=require"

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

# Poll until the hull app server answers on $PORT, instead of a fixed `sleep`.
# A loaded CI runner can take longer than a couple seconds to bind the port and
# open the (lazy) first DB connection; the old fixed sleep raced and produced a
# flaky `curl: (7) connection refused`. Waits up to ~30s, then bails with the
# server log so a genuine startup failure is still diagnosable.
wait_for_server() {
    _log=$1
    i=0
    while [ "$i" -lt 60 ]; do
        if curl -fsS "http://127.0.0.1:${PORT}/" >/dev/null 2>&1; then
            return 0
        fi
        # Server process died? Fail fast rather than polling a dead pid.
        if [ -n "${SVR:-}" ] && ! kill -0 "$SVR" 2>/dev/null; then
            echo "FAIL: app server exited during startup"
            [ -n "$_log" ] && { echo "--- server log ---"; cat "$_log" 2>/dev/null; }
            return 1
        fi
        i=$((i + 1))
        sleep 0.5
    done
    echo "FAIL: app server did not become ready on port ${PORT}"
    [ -n "$_log" ] && { echo "--- server log ---"; cat "$_log" 2>/dev/null; }
    return 1
}

echo "=== building hull with HL_ENABLE_MYSQL=1 ==="
make HL_ENABLE_MYSQL=1 >/dev/null

echo "=== starting mysql (native_password default for the plaintext phase) ==="
docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
docker run -d --name "$CONTAINER" \
    -e MYSQL_ROOT_PASSWORD=rootpw \
    -e MYSQL_DATABASE=hulldb \
    -e MYSQL_USER=hull -e MYSQL_PASSWORD=s3cretpw \
    -p "${MYPORT}:3306" mysql:8.0 \
    --default-authentication-plugin=mysql_native_password >/dev/null

echo "=== waiting for mysql ==="
# The mysql:8 entrypoint runs init against a temporary socket-only server, then
# restarts the real networked one. mysqladmin ping answers on the socket during
# init (too early), so gate on an actual TCP query with the app user: it only
# succeeds once the real server is listening on 3306 with the user created.
ready=0
i=0
while [ "$i" -lt 90 ]; do
    if docker exec "$CONTAINER" \
        mysql -uhull -ps3cretpw -h127.0.0.1 --protocol=tcp hulldb \
        -e "SELECT 1" >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 1
    i=$((i + 1))
done
[ "$ready" = 1 ] || { echo "FAIL: mysql did not become ready"; exit 1; }

APPDIR=$(mktemp -d)
# Multi-statement migration file -> the runner's script path -> exec_multi
# (CLIENT_MULTI_STATEMENTS). Auto-runs at startup before app.lua loads.
mkdir -p "$APPDIR/migrations"
cat > "$APPDIR/migrations/001_init.sql" <<'SQL'
CREATE TABLE mig_test (id INT PRIMARY KEY, label VARCHAR(255) NOT NULL);
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
audit_log.init({ fingerprint_salt = "e2e-mysql-fingerprint-salt" })
db.exec("DROP TABLE IF EXISTS e2e")
db.exec("CREATE TABLE e2e (id BIGINT AUTO_INCREMENT PRIMARY KEY, name TEXT, score INT, active TINYINT)")
db.exec("INSERT INTO e2e (name, score, active) VALUES (?, ?, ?)", { "alice", 10, 1 })
db.exec("INSERT INTO e2e (name, score, active) VALUES (?, ?, ?)", { "bob", 20, 0 })
db.exec("INSERT INTO e2e (name, score, active) VALUES (?, ?, ?)", { "carol", nil, 1 })
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
-- db.async: offloads the query to a worker thread (its own MySQL connection).
app.get("/async", function(req, res)
    local rows = db.async.query(
        "SELECT id, name, active FROM e2e WHERE score >= ? ORDER BY id", { 0 })
    res:json({ rows = rows })
end)
-- stdlib on MySQL: session (dialect DDL + roundtrip), outbox
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
-- Backend-agnostic DB-backed stdlib on real MySQL: inbox dedup, rbac
-- roles/permissions, audit-log record/list, transaction commit, and
-- insert_if_absent (INSERT IGNORE dialect). Proves each module's CREATE TABLE
-- + CRUD run on the MySQL dialect, not just SQLite.
app.get("/stdlib2", function(req, res)
    local inbox_new = inbox.check_and_mark("evt-1", "webhook")   -- false = new
    local inbox_dup = inbox.check_and_mark("evt-1", "webhook")   -- true  = duplicate

    rbac.define_role("editor", { "posts.write" })
    rbac.assign("u1", "editor")
    local can_write  = rbac.has_permission("u1", "posts.write")
    local can_delete = rbac.has_permission("u1", "posts.delete")

    audit_log.record("u1", "login", req, {})
    local events = audit_log.list("u1", {})

    db.exec("CREATE TABLE IF NOT EXISTS stdlib2_txn (id BIGINT)")
    transaction.run(function()
        db.exec("INSERT INTO stdlib2_txn (id) VALUES (?)", { 1 })
    end)
    local txn_count = db.query("SELECT count(*) AS c FROM stdlib2_txn")[1].c

    -- db.insert_if_absent -> INSERT IGNORE: first insert wins, second ignored.
    -- VARCHAR PK (MySQL forbids a bare TEXT primary key without a prefix length).
    db.exec("CREATE TABLE IF NOT EXISTS stdlib2_ins (message_key VARCHAR(255) PRIMARY KEY, val BIGINT)")
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

echo "=== running app against mysql (native auth, plaintext) ==="
./build/hull -d "$DSN" -p "$PORT" "$APPDIR/app.lua" >"$APPDIR/serve.log" 2>&1 &
SVR=$!
wait_for_server "$APPDIR/serve.log" || exit 1

RESP=$(curl -fsS "http://127.0.0.1:${PORT}/" || echo FAIL)
echo "response: $RESP"

fail=0
echo "$RESP" | grep -q '"count":3'                 || { echo "::error count != 3"; fail=1; }
echo "$RESP" | grep -q '"name":"alice"'            || { echo "::error alice missing"; fail=1; }
echo "$RESP" | grep -q '"name":"bob"'              || { echo "::error bob missing"; fail=1; }
echo "$RESP" | grep -q '"name":"carol"'            && { echo "::error carol should be filtered out"; fail=1; }
echo "$RESP" | grep -q '"active":1'                || { echo "::error tinyint decode"; fail=1; }
echo "$RESP" | grep -q '"score":10'                || { echo "::error int decode"; fail=1; }

# migration runner on MySQL (multi-statement file via exec_multi)
RESP_MIG=$(curl -fsS "http://127.0.0.1:${PORT}/migrated" || echo FAIL)
echo "migrated response: $RESP_MIG"
echo "$RESP_MIG" | grep -q '"mig_rows":2'          || { echo "::error migration rows"; fail=1; }
echo "$RESP_MIG" | grep -q '"first":"alpha"'       || { echo "::error migration seed"; fail=1; }

# db.async path (worker thread opens its own MySQL connection through the vtable)
RESP_ASYNC=$(curl -fsS "http://127.0.0.1:${PORT}/async" || echo FAIL)
echo "async response: $RESP_ASYNC"
echo "$RESP_ASYNC" | grep -q '"name":"alice"'      || { echo "::error async alice missing"; fail=1; }
echo "$RESP_ASYNC" | grep -q '"name":"bob"'        || { echo "::error async bob missing"; fail=1; }
echo "$RESP_ASYNC" | grep -q '"name":"carol"'      && { echo "::error async carol not filtered"; fail=1; }

# stdlib modules on MySQL (session / outbox / auth-health / search guard)
RESP_STDLIB=$(curl -fsS "http://127.0.0.1:${PORT}/stdlib" || echo FAIL)
echo "stdlib response: $RESP_STDLIB"
echo "$RESP_STDLIB" | grep -q '"backend":"mysql"'       || { echo "::error backend not mysql"; fail=1; }
echo "$RESP_STDLIB" | grep -q '"session_role":"admin"'  || { echo "::error session roundtrip"; fail=1; }
echo "$RESP_STDLIB" | grep -q '"outbox_pending":1'      || { echo "::error outbox enqueue"; fail=1; }
echo "$RESP_STDLIB" | grep -q '"sessions_ok":true'      || { echo "::error auth-health table probe"; fail=1; }
echo "$RESP_STDLIB" | grep -q '"search_guarded":true'   || { echo "::error search SQLite-only guard"; fail=1; }

# backend-agnostic DB stdlib on MySQL: inbox / rbac / audit-log / transaction / insert_if_absent
RESP_STDLIB2=$(curl -fsS "http://127.0.0.1:${PORT}/stdlib2" || echo FAIL)
echo "stdlib2 response: $RESP_STDLIB2"
echo "$RESP_STDLIB2" | grep -q '"inbox_new":false'   || { echo "::error inbox first-seen not new"; fail=1; }
echo "$RESP_STDLIB2" | grep -q '"inbox_dup":true'    || { echo "::error inbox dedup"; fail=1; }
echo "$RESP_STDLIB2" | grep -q '"rbac_write":true'   || { echo "::error rbac has_permission grant"; fail=1; }
echo "$RESP_STDLIB2" | grep -q '"rbac_delete":false' || { echo "::error rbac has_permission deny"; fail=1; }
echo "$RESP_STDLIB2" | grep -q '"audit_count":1'     || { echo "::error audit-log record/list"; fail=1; }
echo "$RESP_STDLIB2" | grep -q '"txn_count":1'       || { echo "::error transaction commit"; fail=1; }
echo "$RESP_STDLIB2" | grep -q '"insert_if_absent":10' || { echo "::error insert_if_absent (first-wins) on mysql"; fail=1; }

if [ "$fail" = 0 ]; then
    echo "PASS: mysql backend end-to-end (sync + db.async + stdlib -> typed decode)"
else
    echo "--- server log ---"; cat "$APPDIR/serve.log"
    exit 1
fi

kill "$SVR" 2>/dev/null || true
SVR=

# ── db.open() dynamic connections against MySQL (roadmap §2.2) ─────────
# The manifest allowlists the mysql scheme and the 127.0.0.0/8 host CIDR. A DSN
# inside the CIDR opens and queries; a host outside it and a scheme not in the
# allowlist are rejected before any connect. udf is SQLite-only, so a MySQL
# connection object must NOT expose it.
echo "=== db.open dynamic allow/deny against mysql ==="
APPDIR_DYN=$(mktemp -d)
cat > "$APPDIR_DYN/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/db@1" },
    databases = { dynamic = { schemes = { "mysql" }, hosts = { "127.0.0.0/8" } } },
})
local db = require("hull.db")
app.main(function(ctx)
    local allow_dsn = ctx.args[1]
    local ok_open, conn = pcall(function() return db.open(allow_dsn) end)
    if ok_open then
        print("allow_count=" .. tostring(conn.query("SELECT count(*) AS c FROM e2e")[1].c))
        print("my_has_udf=" .. tostring(conn.udf ~= nil))
        conn.close()
    else
        print("allow_count=ERR:" .. tostring(conn))
    end
    local ok_host = pcall(function() return db.open("mysql://u@10.9.9.9:3306/db") end)
    print("deny_host=" .. (ok_host and "ok" or "denied"))
    local ok_scheme = pcall(function() return db.open(":memory:") end)
    print("deny_scheme=" .. (ok_scheme and "ok" or "denied"))
    return 0
end)
LUA

DYN_OUT=$(./build/hull "$APPDIR_DYN/app.lua" -- "$DSN" 2>&1 || true)
echo "$DYN_OUT" | grep -qE "allow_count=3" || { echo "::error db.open allowed host did not query"; echo "$DYN_OUT"; exit 1; }
echo "$DYN_OUT" | grep -qE "deny_host=denied" || { echo "::error db.open out-of-CIDR host not rejected"; echo "$DYN_OUT"; exit 1; }
echo "$DYN_OUT" | grep -qE "deny_scheme=denied" || { echo "::error db.open disallowed scheme not rejected"; echo "$DYN_OUT"; exit 1; }
echo "$DYN_OUT" | grep -qE "my_has_udf=false" || { echo "::error mysql connection exposes a udf sub-object (should be gated off)"; echo "$DYN_OUT"; exit 1; }
echo "PASS: db.open dynamic connections (CIDR host allow + out-of-CIDR deny + scheme deny)"
rm -rf "$APPDIR_DYN"

# ── TLS + caching_sha2_password phase ─────────────────────────────────
# MySQL 8 ships TLS on by default (auto-generated certs). Switch the user to
# caching_sha2_password so its cache is empty, then connect over TLS: the
# full-auth path sends the cleartext password over the encrypted channel.
echo "=== switching user to caching_sha2_password ==="
docker exec "$CONTAINER" mysql -uroot -prootpw -e \
    "ALTER USER 'hull'@'%' IDENTIFIED WITH caching_sha2_password BY 's3cretpw';" >/dev/null 2>&1

APPDIR_TLS=$(mktemp -d)
cat > "$APPDIR_TLS/app.lua" <<'LUA'
app.manifest({ modules = { "hull/db@1", "hull/http-server@1" } })
local db = require("hull.db").default()
app.get("/", function(req, res)
    -- Ssl_cipher is non-empty only when THIS session is over TLS.
    local r = db.query("SHOW SESSION STATUS LIKE 'Ssl_cipher'")
    local n = db.query("SELECT count(*) AS c FROM e2e")
    res:json({ cipher = r[1] and r[1].Value or "", count = n[1].c })
end)
LUA

echo "=== running app over TLS (sslmode=require + caching_sha2 full auth) ==="
./build/hull -d "$DSN_TLS" -p "$PORT" "$APPDIR_TLS/app.lua" >"$APPDIR_TLS/serve.log" 2>&1 &
SVR=$!
wait_for_server "$APPDIR_TLS/serve.log" || exit 1

RESP_TLS=$(curl -fsS "http://127.0.0.1:${PORT}/" || echo FAIL)
echo "response: $RESP_TLS"

tfail=0
echo "$RESP_TLS" | grep -q '"cipher":""' && { echo "::error connection not over TLS (empty cipher)"; tfail=1; }
echo "$RESP_TLS" | grep -q '"cipher":"' || { echo "::error no cipher field"; tfail=1; }
echo "$RESP_TLS" | grep -q '"count":3'  || { echo "::error query over TLS failed"; tfail=1; }

if [ "$tfail" = 0 ]; then
    echo "PASS: mysql TLS end-to-end (SSLRequest -> handshake -> caching_sha2 full auth over TLS -> encrypted query)"
    rm -rf "$APPDIR_TLS"
else
    echo "--- server log ---"; cat "$APPDIR_TLS/serve.log" 2>/dev/null || true
    exit 1
fi
