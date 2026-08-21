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

echo "=== building hull with HL_ENABLE_POSTGRES=1 ==="
make HL_ENABLE_POSTGRES=1 >/dev/null

echo "=== starting postgres (scram-sha-256 auth) ==="
docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
docker run -d --name "$CONTAINER" \
    -e POSTGRES_USER=hull -e POSTGRES_DB=hulldb \
    -e POSTGRES_PASSWORD=s3cretpw \
    -p "${PGPORT}:5432" "${PG_IMAGE:-postgres:16-alpine}" >/dev/null

echo "=== waiting for postgres (real TCP + SCRAM connection, as the app connects) ==="
# postgres:16 runs initdb on first start, briefly bringing PG up on a local
# unix socket before restarting the real networked instance. A default
# (unix-socket) `pg_isready` can pass on that transient init instance, so the
# hull app's later TCP connect races the restart and is refused ("failed to open
# database connection"). Gate on a REAL connection instead: TCP (-h 127.0.0.1) +
# SCRAM auth + a query against the target db -- exactly what the app does --
# mirroring the proven e2e_mysql.sh readiness check.
ready=0
i=0
while [ "$i" -lt 60 ]; do
    if docker exec -e PGPASSWORD=s3cretpw "$CONTAINER" \
           psql -h 127.0.0.1 -U hull -d hulldb -tAc 'SELECT 1' >/dev/null 2>&1; then
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

DYN_OUT=$(./build/hull "$APPDIR_DYN/app.lua" -- "$DSN" 2>&1 || true)
echo "$DYN_OUT" | grep -qE "allow_count=3" || { echo "::error db.open allowed host did not query"; echo "$DYN_OUT"; exit 1; }
echo "$DYN_OUT" | grep -qE "deny_host=denied" || { echo "::error db.open out-of-CIDR host not rejected"; echo "$DYN_OUT"; exit 1; }
echo "$DYN_OUT" | grep -qE "deny_scheme=denied" || { echo "::error db.open disallowed scheme not rejected"; echo "$DYN_OUT"; exit 1; }
echo "$DYN_OUT" | grep -qE "pg_has_udf=false" || { echo "::error postgres connection exposes a udf sub-object (should be gated off)"; echo "$DYN_OUT"; exit 1; }
echo "PASS: db.open dynamic connections (CIDR host allow + out-of-CIDR deny + scheme deny)"
rm -rf "$APPDIR_DYN"

# ── hull/jobs SKIP LOCKED concurrency (Phase 2 correctness gate) ───────
# 200 jobs, 4 parallel claimer processes against real Postgres. The claim uses
# UPDATE ... WHERE id IN (SELECT ... FOR UPDATE SKIP LOCKED) RETURNING, so each
# job must be claimed by exactly one process. See docs/jobs_design.md.
echo "=== jobs: SKIP LOCKED concurrency on Postgres ==="
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
    echo "PASS: jobs SKIP LOCKED - 200 claimed exactly once by 4 processes (Postgres)"
    rm -rf "$JOBSDIR"
else
    echo "::error jobs concurrency: total=$jtotal uniq=$juniq (want 200/200)"; exit 1
fi

# ── hull/jobs durable execution + observability on Postgres ────────────
# Regression coverage for two Postgres-specific bugs the concurrency gate above
# missed (it never uses the enqueue id): (1) jobs.enqueue relied on db.last_id,
# unsupported on PG - jobs.start returned -1 and workflows/await broke; fixed to
# INSERT ... RETURNING id. (2) attempt-history ms columns were INTEGER (int4) -
# ms timestamps (~1.78e12) overflow on PG; fixed to BIGINT.
echo "=== jobs: durable execution + observability on Postgres ==="
WFDIR=$(mktemp -d)
cat > "$WFDIR/wf.lua" <<'LUA'
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/jobs@1" } })
app.main(function(ctx)
  jobs.init({ history = true, backoff = function() return 0 end })
  local threw = false
  jobs.workflow("wf", function(w)
    local a = w.step("charge", function() return { amt = 10 } end)
    local ts = w.now(); local uid = w.uuid()   -- deterministic primitives (memoized)
    if not threw then threw = true; error("retry once") end
    return { amt = a.amt, ts = ts, uid = uid }
  end)
  local wid = jobs.start("wf", {})
  jobs.run_worker({ drain = true, poll_ms = 1 })
  local st = jobs.workflow_status(wid); local r = jobs.await(wid)
  local hist = jobs.history(wid); local m = jobs.metrics()
  local ms_ok = false
  for _, h in ipairs(hist) do if h.finished_ms and h.finished_ms > 1000000000000 then ms_ok = true end end
  ctx.stdout:write(("PGWF wid_valid=%s status=%s amt=%s attempts=%d hist_ms=%s run_p50=%s\n"):format(
    (type(wid) == "number" and wid > 0) and "yes" or "no", st and st.status or "nil",
    tostring(r and r.result and r.result.amt), #hist, ms_ok and "yes" or "no",
    (m.latency and m.latency.run_ms and m.latency.run_ms.p50 ~= nil) and "yes" or "no"))
  return 0
end)
LUA
wfout=$(./build/hull "$WFDIR/wf.lua" -d "$DSN" 2>/dev/null)
case "$wfout" in
    *"PGWF wid_valid=yes status=done amt=10 attempts=2 hist_ms=yes run_p50=yes"*)
        echo "PASS: jobs workflows + deterministic primitives + attempt history + metrics (Postgres)"
        rm -rf "$WFDIR" ;;
    *)  echo "::error jobs durable/observability on PG: $wfout"; exit 1 ;;
esac

# ── hull/jobs durable events + strict concurrency on Postgres ─────────
# Events (log + subscription cursor-lease drain) and strict per-key concurrency
# are otherwise only CI-tested on SQLite (e2e_jobs.sh). Their SQL has real
# backend divergence - the lease CAS uses RETURNING on PG/SQLite vs an affected-
# row count on MySQL, and the strict counter reserve races SKIP LOCKED - so pin
# them on real Postgres here. Each phase drops the jobs tables first (via psql,
# to bypass the _hull_* namespace guard) for a clean, deterministic slate.
jobs_reset_pg() {
    docker exec "$CONTAINER" psql -U hull -d hulldb -q -c \
      "DROP TABLE IF EXISTS _hull_jobs, _hull_job_events, _hull_job_subscriptions, _hull_job_concurrency, _hull_job_history CASCADE" \
      >/dev/null 2>&1 || true
}

echo "=== jobs: durable events + subscriptions on Postgres ==="
jobs_reset_pg
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
  -- Subscription cursor lease drain: the RETURNING-vs-rowcount CAS path.
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
        echo "PASS: jobs durable events + subscription lease drain (Postgres)"
        rm -rf "$EVDIR" ;;
    *)  echo "::error jobs events on PG: $evout"; exit 1 ;;
esac

echo "=== jobs: strict per-key concurrency on Postgres ==="
jobs_reset_pg
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
        echo "PASS: jobs strict per-key concurrency (hard cap + slot release, Postgres)"
        rm -rf "$STDIR" ;;
    *)  echo "::error jobs strict on PG: $stout"; exit 1 ;;
esac

echo "=== jobs: strict concurrency fleet (4 processes, hard cap 2, Postgres) ==="
jobs_reset_pg
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
    echo "PASS: jobs strict fleet - 4 processes claimed exactly 2 (hard cap, Postgres)"
    rm -rf "$SFDIR"
else
    echo "::error jobs strict fleet on PG: total=$stot uniq=$suniq (want 2/2)"; exit 1
fi
# Leave a clean slate: these phases seed strict/pending jobs + a live concurrency
# counter that would otherwise skew the downstream jobs phases (rowcount, the
# Phase-4 latency/reconnect waits).
jobs_reset_pg

# ── db.exec affected-row count (regression guard) ─────────────────────
# The pg backend must surface the CommandComplete row count (like sqlite3_changes
# / MySQL affected_rows); it previously discarded it and returned 0, which broke
# every rowcount-based caller (jobs.cancel/retry/reap, session/ratelimit cleanup).
RCDIR=$(mktemp -d)
cat > "$RCDIR/rc.lua" <<'LUA'
local db = require("hull.db").default()
local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/db@1", "hull/jobs@1" } })
app.main(function(ctx)
  db.exec("DROP TABLE IF EXISTS rc_t")
  db.exec("CREATE TABLE rc_t (id INTEGER PRIMARY KEY, n INTEGER)")
  db.exec("INSERT INTO rc_t (id,n) VALUES (1,0),(2,0),(3,0)")
  local upd = db.exec("UPDATE rc_t SET n=1 WHERE id<=2")       -- 2
  local del = db.exec("DELETE FROM rc_t WHERE id=3")            -- 1
  local no  = db.exec("UPDATE rc_t SET n=9 WHERE id=99")        -- 0
  jobs.init()
  local jid = jobs.enqueue("x", {}, { delay = 3600 })          -- pending
  local ok  = jobs.cancel(jid)                                 -- true (was false on PG)
  ctx.stdout:write(("PGRC upd=%s del=%s no=%s cancel=%s\n"):format(
    tostring(upd), tostring(del), tostring(no), tostring(ok)))
  return 0
end)
LUA
rcout="$(./build/hull "$RCDIR/rc.lua" -d "$DSN" 2>/dev/null)" || true
case "$rcout" in
    *"PGRC upd=2 del=1 no=0 cancel=true"*)
        echo "PASS: db.exec returns the affected-row count on Postgres (jobs.cancel etc. correct)"
        rm -rf "$RCDIR" ;;
    *)  echo "::error db.exec affected-row count on PG: $rcout"; exit 1 ;;
esac

# ── conn.wait_notify: low-latency LISTEN/NOTIFY (Phase 4.2) ────────────
# A waiter app parks on conn.wait_notify (yielding on the db.async worker pool);
# a SECOND connection issues pg_notify; the waiter must wake with notified=true
# well under its timeout - proving the yielding worker-pool wait + cross-
# connection NOTIFY delivery. Latency-only: a follow-up wait with no NOTIFY
# times out to false (the poll-fallback shape). Runs for both runtimes.
check_wait_notify() {
    _label="$1"; _ext="$2"; _waiter="$3"; _notifier="$4"
    WND=$(mktemp -d)
    printf '%s\n' "$_waiter"   > "$WND/waiter.$_ext"
    printf '%s\n' "$_notifier" > "$WND/notifier.$_ext"
    ./build/hull "$WND/waiter.$_ext" -d "$DSN" > "$WND/w.out" 2>/dev/null &
    _wpid=$!
    sleep 3   # let the waiter connect + LISTEN + park before we notify
    ./build/hull "$WND/notifier.$_ext" -d "$DSN" >/dev/null 2>&1 || true
    wait "$_wpid" || true
    _out="$(cat "$WND/w.out")"
    case "$_out" in
        *"WN notified=true"*"WN2 timeout_false=true"*)
            echo "PASS: $_label conn.wait_notify wakes on NOTIFY + times out to false" ;;
        *)  echo "::error $_label wait_notify on PG: $_out"; exit 1 ;;
    esac
    rm -rf "$WND"
}

LUA_WN_WAITER='local db = require("hull.db").default()
local t = require("hull.time")
app.manifest({ modules = { "hull/db@1", "hull/time@1" } })
app.main(function(ctx)
  local a = t.now_ms()
  local got = db.wait_notify("hull_jobs", 10000)
  ctx.stdout:write(("WN notified=%s waited_ms=%d\n"):format(tostring(got), t.now_ms()-a))
  local none = db.wait_notify("hull_jobs", 300)
  ctx.stdout:write(("WN2 timeout_false=%s\n"):format(tostring(none == false)))
  return 0
end)'
LUA_WN_NOTIFIER='local db = require("hull.db").default()
app.manifest({ modules = { "hull/db@1" } })
app.main(function() db.exec("SELECT pg_notify(?, ?)", { "hull_jobs", "" }) return 0 end)'

JS_WN_WAITER='import { app } from "hull:app";
import { db as dbm } from "hull:db";
import { time } from "hull:time";
app.manifest({ modules: ["hull/db@1", "hull/time@1"] });
app.main(async (ctx) => {
  const db = dbm.default();
  const a = time.nowMs();
  const got = await db.waitNotify("hull_jobs", 10000);
  ctx.stdout.write(`WN notified=${got} waited_ms=${time.nowMs()-a}\n`);
  const none = await db.waitNotify("hull_jobs", 300);
  ctx.stdout.write(`WN2 timeout_false=${none === false}\n`);
  return 0;
});'
JS_WN_NOTIFIER='import { app } from "hull:app";
import { db as dbm } from "hull:db";
app.manifest({ modules: ["hull/db@1"] });
app.main((ctx) => { dbm.default().exec("SELECT pg_notify(?, ?)", ["hull_jobs", ""]); return 0; });'

echo "=== conn.wait_notify LISTEN/NOTIFY round trip ==="
check_wait_notify "lua" "lua" "$LUA_WN_WAITER" "$LUA_WN_NOTIFIER"
check_wait_notify "js"  "js"  "$JS_WN_WAITER"  "$JS_WN_NOTIFIER"

# ── jobs low-latency pickup via NOTIFY (Phase 4.3) ────────────────────
# A run_worker parked idle with a LONG poll (8s) must wake and process a job
# enqueued by a SECOND process in WELL under that poll - proving enqueue's
# pg_notify + run_worker's idle wait_notify. On a non-NOTIFY backend the same
# path degrades to polling (covered green by e2e_jobs on SQLite). Latency, never
# correctness: the poll timeout is the safety net.
check_notify_latency() {
    _label="$1"; _ext="$2"; _worker="$3"; _enq="$4"
    LD=$(mktemp -d)
    printf '%s\n' "$_worker" > "$LD/worker.$_ext"
    printf '%s\n' "$_enq"    > "$LD/enq.$_ext"
    ./build/hull "$LD/worker.$_ext" -d "$DSN" > "$LD/w.out" 2>/dev/null &
    _wpid=$!
    sleep 3   # let the worker init + park idle on wait_notify
    ./build/hull "$LD/enq.$_ext" -d "$DSN" >/dev/null 2>&1 || true
    wait "$_wpid" || true
    _out="$(cat "$LD/w.out")"
    _ms=$(printf '%s' "$_out" | sed -n 's/.*elapsed_ms=\([0-9][0-9]*\).*/\1/p' | head -1)
    if [ -n "$_ms" ] && [ "$_ms" -lt 5000 ]; then
        echo "PASS: $_label run_worker woke on enqueue NOTIFY (${_ms}ms << 8000 poll)"
    else
        echo "::error $_label notify latency: $_out"; exit 1
    fi
    rm -rf "$LD"
}

LUA_LAT_WORKER='local jobs = require("hull.jobs")
local t = require("hull.time")
app.manifest({ modules = { "hull/db@1", "hull/jobs@1", "hull/time@1" } })
app.main(function(ctx)
  jobs.init()
  local a = t.now_ms()
  jobs.handler("ping", function(j)
    ctx.stdout:write(("WORKED elapsed_ms=%d\n"):format(t.now_ms()-a))
    jobs.stop()
    return {}
  end)
  jobs.run_worker({ poll_ms = 8000 })
  return 0
end)'
LUA_LAT_ENQ='local jobs = require("hull.jobs")
app.manifest({ modules = { "hull/db@1", "hull/jobs@1" } })
app.main(function() jobs.init() jobs.enqueue("ping", {}) return 0 end)'

JS_LAT_WORKER='import { app } from "hull:app";
import { jobs } from "hull:jobs";
import { time } from "hull:time";
app.manifest({ modules: ["hull/db@1", "hull/jobs@1", "hull/time@1"] });
app.main(async (ctx) => {
  jobs.init();
  const a = time.nowMs();
  jobs.handler("ping", (j) => { ctx.stdout.write(`WORKED elapsed_ms=${time.nowMs()-a}\n`); jobs.stop(); return {}; });
  await jobs.runWorker({ pollMs: 8000 });
  return 0;
});'
JS_LAT_ENQ='import { app } from "hull:app";
import { jobs } from "hull:jobs";
app.manifest({ modules: ["hull/db@1", "hull/jobs@1"] });
app.main((ctx) => { jobs.init(); jobs.enqueue("ping", {}); return 0; });'

echo "=== jobs low-latency pickup (run_worker wakes on enqueue NOTIFY) ==="
check_notify_latency "lua" "lua" "$LUA_LAT_WORKER" "$LUA_LAT_ENQ"
check_notify_latency "js"  "js"  "$JS_LAT_WORKER"  "$JS_LAT_ENQ"

# ── LISTEN reconnect after a dropped connection (Phase 4, R2) ──────────
# Kill the worker's LISTEN backend mid-run; the worker must reopen its
# connection, re-issue LISTEN, and catch a SUBSEQUENT NOTIFY (RECOVERED=true).
# Without the reconnect (hl_worker_db_invalidate on a -1 wait) it would stay on
# the dead connection and spin/return false forever, never waking. The reconnect
# lives in the C worker layer (db_work_fn), shared by both runtimes, so one
# runtime proves it.
check_reconnect() {
    _label="$1"; _ext="$2"; _worker="$3"
    RD=$(mktemp -d)
    printf '%s\n' "$_worker" > "$RD/worker.$_ext"
    ./build/hull "$RD/worker.$_ext" -d "$DSN" > "$RD/w.out" 2>/dev/null &
    _wpid=$!
    sleep 3   # worker LISTENing on its first wait
    docker exec "$CONTAINER" psql -U hull -d hulldb -tAc \
        "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query = 'LISTEN hull_jobs'" \
        >/dev/null 2>&1 || true
    sleep 3   # worker hits the dead conn -> invalidate -> reopen -> re-LISTEN
    docker exec "$CONTAINER" psql -U hull -d hulldb -tAc "NOTIFY hull_jobs" >/dev/null 2>&1 || true
    wait "$_wpid" || true
    case "$(cat "$RD/w.out")" in
        *"RECOVERED=true"*)
            echo "PASS: $_label wait_notify reconnects after a dropped LISTEN connection" ;;
        *)  echo "::error $_label reconnect: $(cat "$RD/w.out")"; exit 1 ;;
    esac
    rm -rf "$RD"
}

LUA_RC_WORKER='local db = require("hull.db").default()
app.manifest({ modules = { "hull/db@1" } })
app.main(function(ctx)
  local ok = false
  for _ = 1, 12 do
    if db.wait_notify("hull_jobs", 1500) then ok = true break end
  end
  ctx.stdout:write(("RECOVERED=%s\n"):format(tostring(ok)))
  return 0
end)'

echo "=== LISTEN reconnect after a dropped connection ==="
check_reconnect "lua" "lua" "$LUA_RC_WORKER"

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
./build/hull -d "$DSN_TLS" -p "$PORT" "$APPDIR_TLS/app.lua" >"$APPDIR_TLS/serve.log" 2>&1 &
SVR=$!
wait_for_server "$APPDIR_TLS/serve.log" || exit 1

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
