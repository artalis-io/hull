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
cat > "$APPDIR/app.lua" <<'LUA'
app.manifest({ modules = { "hull/db@1", "hull/http-server@1" } })
local db = require("hull.db")
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

if [ "$fail" = 0 ]; then
    echo "PASS: postgres backend end-to-end (select -> connect -> query -> typed decode)"
else
    echo "--- server log ---"; cat "$APPDIR/serve.log"
    exit 1
fi

kill "$SVR" 2>/dev/null || true
SVR=

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
local db = require("hull.db")
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
