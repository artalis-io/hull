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
    rc=$?
    # On failure, dump the DB container's own logs (bounded) BEFORE removing it,
    # so an unattended nightly captures WHY the engine never became ready (e.g. a
    # removed startup option on a newer version). Startup/init output only - no
    # secrets echoed.
    if [ "$rc" -ne 0 ]; then
        echo "=== docker logs $CONTAINER (tail 200; e2e failed rc=$rc) ==="
        docker logs --tail 200 "$CONTAINER" 2>&1 || true
        echo "=== end docker logs $CONTAINER ==="
    fi
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
# --default-authentication-plugin is a MySQL-8-only mysqld option; MariaDB rejects
# unknown options and exits, and it already defaults to mysql_native_password, so
# omit the flag there. MariaDB images accept the MYSQL_* env vars as aliases.
IMG="${MYSQL_IMAGE:-mysql:8.0}"
SERVER_ARGS="--default-authentication-plugin=mysql_native_password"
case "$IMG" in
    mariadb*) SERVER_ARGS="" ;;
esac
docker run -d --name "$CONTAINER" \
    -e MYSQL_ROOT_PASSWORD=rootpw \
    -e MYSQL_DATABASE=hulldb \
    -e MYSQL_USER=hull -e MYSQL_PASSWORD=s3cretpw \
    -p "${MYPORT}:3306" "$IMG" \
    $SERVER_ARGS >/dev/null

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

# ── hull/jobs SKIP LOCKED concurrency (Phase 2 correctness gate) ───────
# 200 jobs, 4 parallel claimer processes against real MySQL 8. The claim uses
# SELECT ... FOR UPDATE SKIP LOCKED then UPDATE-by-id (no RETURNING on MySQL),
# read back by claim_token, so each job is claimed by exactly one process.
echo "=== jobs: SKIP LOCKED concurrency on MySQL ==="
JOBSDIR=$(mktemp -d)
cat > "$JOBSDIR/seed.lua" <<'LUA'
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function() jobs.init(); for i = 1, 200 do jobs.enqueue("t", { n = i }) end; return 0 end)
LUA
cat > "$JOBSDIR/claim.lua" <<'LUA'
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  while true do local b = jobs.claim({ batch = 7 }); if #b == 0 then break end
    for _, j in ipairs(b) do ctx.stdout:write(j.id .. "\n") end end
  return 0
end)
LUA
./build/hull "$JOBSDIR/seed.lua" -d "$DSN" >/dev/null 2>&1
ji=1; while [ "$ji" -le 4 ]; do ./build/hull "$JOBSDIR/claim.lua" -d "$DSN" > "$JOBSDIR/out.$ji" 2>/dev/null & ji=$((ji + 1)); done
wait
jtotal=$(cat "$JOBSDIR"/out.* | grep -c . || true)
juniq=$(cat "$JOBSDIR"/out.* | sort -n | uniq | grep -c . || true)
if [ "$jtotal" -eq 200 ] && [ "$juniq" -eq 200 ]; then
    echo "PASS: jobs SKIP LOCKED - 200 claimed exactly once by 4 processes (MySQL)"
    rm -rf "$JOBSDIR"
else
    echo "::error jobs concurrency: total=$jtotal uniq=$juniq (want 200/200)"; exit 1
fi

# ── hull/jobs durable execution + observability on MySQL ───────────────
# Regression coverage for the attempt-history BIGINT fix (ms timestamps overflow
# a 32-bit INTEGER on MySQL) plus workflows / deterministic primitives / history.
# Asserted via DB state (not app stdout): a graceful-exit `-d mysql` app currently
# hits a MySQL-backend connection-teardown crash that swallows the final buffered
# stdout line - the DB writes commit first, so the state check is reliable. (The
# teardown crash is a separate backend bug, tracked independently.)
echo "=== jobs: durable execution + observability on MySQL (DB-state) ==="
WFDIR=$(mktemp -d)
cat > "$WFDIR/wf.lua" <<'LUA'
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init({ history = true, backoff = function() return 0 end })
  local threw = false
  jobs.workflow("wfm", function(w)
    local a = w.step("charge", function() return { amt = 10 } end)
    w.now(); w.uuid()   -- deterministic primitives (memoized)
    if not threw then threw = true; error("retry once") end
    return { amt = a.amt }
  end)
  jobs.start("wfm", {})
  jobs.run_worker({ drain = true, poll_ms = 1 })
  return 0
end)
LUA
./build/hull "$WFDIR/wf.lua" -d "$DSN" >/dev/null 2>&1 || true
wfstat=$(docker exec "$CONTAINER" mysql -uhull -ps3cretpw hulldb -N -se \
  "SELECT CONCAT((SELECT status FROM _hull_jobs WHERE type='__wf:wfm' ORDER BY id DESC LIMIT 1),',',(SELECT COUNT(*) FROM _hull_workflow_steps),',',(SELECT COUNT(*) FROM _hull_job_attempts),',',CASE WHEN (SELECT MAX(finished_ms) FROM _hull_job_attempts)>1000000000000 THEN 'bigint' ELSE 'small' END)" 2>/dev/null)
case "$wfstat" in
    done,*bigint)
        echo "PASS: jobs workflows + deterministic primitives + attempt history (MySQL, DB-verified: $wfstat)"
        rm -rf "$WFDIR" ;;
    *)  echo "::error jobs durable/observability on MySQL: got '$wfstat'"; exit 1 ;;
esac

# ── hull/jobs durable events + strict concurrency on MySQL ────────────
# Events (log + subscription cursor-lease drain) and strict per-key concurrency
# are otherwise only CI-tested on SQLite (e2e_jobs.sh). Their SQL diverges by
# backend - the lease CAS uses an affected-row count on MySQL (no RETURNING),
# and the strict counter reserve races SKIP LOCKED - so pin them on real MySQL
# here. Each phase drops the jobs tables first (via mysql, to bypass the
# _hull_* namespace guard) for a clean, deterministic slate.
jobs_reset_my() {
    docker exec "$CONTAINER" mysql -uhull -ps3cretpw hulldb -e \
      "SET FOREIGN_KEY_CHECKS=0; DROP TABLE IF EXISTS _hull_jobs, _hull_job_events, _hull_job_subscriptions, _hull_job_concurrency, _hull_job_attempts, _hull_workflow_steps; SET FOREIGN_KEY_CHECKS=1" \
      >/dev/null 2>&1 || true
}

echo "=== jobs: durable events + subscriptions on MySQL ==="
jobs_reset_my
EVDIR=$(mktemp -d)
cat > "$EVDIR/ev.lua" <<'LUA'
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/db@1", "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init({ events = true })
  jobs.handler("ok",  function(j) return { r = 1 } end)
  jobs.handler("bad", function(j) error("boom") end)
  jobs.enqueue("ok", { x = 1 })
  jobs.enqueue("bad", {}, { max_attempts = 1 })
  jobs.enqueue("nohandler", {})
  local c = jobs.enqueue("ok", {}); jobs.cancel(c)
  for _ = 1, 3 do jobs.work({ batch = 10 }) end
  local n = {}
  for _, e in ipairs(jobs.events({ limit = 100 })) do n[e.type] = (n[e.type] or 0) + 1 end
  -- Subscription cursor lease drain: on MySQL the lease CAS uses the affected-
  -- row count (no RETURNING). deliv == the full log => the lease was acquired.
  local seen = 0
  jobs.subscribe("s", function(ev) seen = seen + 1 end, { from = "beginning" })
  local d = jobs._events_drain("s", { now = 1000, batch = 100 })
  ctx.stdout:write(("JEV e=%d c=%d d=%d x=%d deliv=%d seen=%d\n"):format(
    n.enqueued or 0, n.completed or 0, n.dead or 0, n.cancelled or 0, d.delivered, seen))
  return 0
end)
LUA
evout=$(./build/hull "$EVDIR/ev.lua" -d "$DSN" 2>/dev/null)
case "$evout" in
    *"JEV e=4 c=1 d=2 x=1 deliv=8 seen=8"*)
        echo "PASS: jobs durable events + subscription lease drain (MySQL)"
        rm -rf "$EVDIR" ;;
    *)  echo "::error jobs events on MySQL: $evout"; exit 1 ;;
esac

echo "=== jobs: strict per-key concurrency on MySQL ==="
jobs_reset_my
STDIR=$(mktemp -d)
cat > "$STDIR/st.lua" <<'LUA'
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/db@1", "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  for i = 1, 4 do jobs.enqueue("t", { i = i },
      { concurrency_key = "A", concurrency = 2, concurrency_strict = true }) end
  local c1 = jobs.claim({ batch = 10 })          -- hard cap: keeps EXACTLY 2
  local kept = 0; for _, j in ipairs(c1) do if j._conc_key == "A" then kept = kept + 1 end end
  jobs.handler("tb", function(j) return end)
  for i = 1, 3 do jobs.enqueue("tb", { i = i },
      { concurrency_key = "B", concurrency = 2, concurrency_strict = true }) end
  for _ = 1, 6 do jobs.work({ batch = 10 }) end   -- slot released on each done
  local done = 0; for id = 5, 7 do local g = jobs.get(id); if g and g.status == "done" then done = done + 1 end end
  ctx.stdout:write(("JSTRICT cap_kept=%d drained=%d\n"):format(kept, done))
  return 0
end)
LUA
stout=$(./build/hull "$STDIR/st.lua" -d "$DSN" 2>/dev/null)
case "$stout" in
    *"JSTRICT cap_kept=2 drained=3"*)
        echo "PASS: jobs strict per-key concurrency (hard cap + slot release, MySQL)"
        rm -rf "$STDIR" ;;
    *)  echo "::error jobs strict on MySQL: $stout"; exit 1 ;;
esac

echo "=== jobs: strict concurrency fleet (4 processes, hard cap 2, MySQL) ==="
jobs_reset_my
SFDIR=$(mktemp -d)
cat > "$SFDIR/seed.lua" <<'LUA'
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function() jobs.init()
  for i = 1, 50 do jobs.enqueue("t", {},
      { concurrency_key = "K", concurrency = 2, concurrency_strict = true }) end
  return 0 end)
LUA
cat > "$SFDIR/claim.lua" <<'LUA'
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init()
  local b = jobs.claim({ batch = 10 })   -- leaves them running (no complete)
  for _, j in ipairs(b) do ctx.stdout:write(j.id .. "\n") end
  return 0
end)
LUA
./build/hull "$SFDIR/seed.lua" -d "$DSN" >/dev/null 2>&1
si=1; while [ "$si" -le 4 ]; do ./build/hull "$SFDIR/claim.lua" -d "$DSN" > "$SFDIR/out.$si" 2>/dev/null & si=$((si + 1)); done
wait
stot=$(cat "$SFDIR"/out.* | grep -c . || true)
suniq=$(cat "$SFDIR"/out.* | sort -n | uniq | grep -c . || true)
if [ "$stot" -eq 2 ] && [ "$suniq" -eq 2 ]; then
    echo "PASS: jobs strict fleet - 4 processes claimed exactly 2 (hard cap, MySQL)"
    rm -rf "$SFDIR"
else
    echo "::error jobs strict fleet on MySQL: total=$stot uniq=$suniq (want 2/2)"; exit 1
fi
jobs_reset_my   # clean slate for the following phases

# The TLS + caching_sha2_password phase below is MySQL-8-specific (caching_sha2
# full auth). SKIP_TLS=1 stops here after the plaintext phases - used for the
# plaintext MariaDB run (MariaDB defaults to mysql_native_password; its TLS matrix
# is a later checkpoint).
if [ "${SKIP_TLS:-0}" = 1 ]; then
    echo "PASS: mysql/mariadb plaintext end-to-end (SKIP_TLS set; TLS phase skipped)"
    exit 0
fi

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
