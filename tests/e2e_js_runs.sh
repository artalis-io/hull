#!/bin/sh
# e2e_js_runs.sh: a JS entry's run owns every op its own code starts.
#
# Audit 7 (JS runtime) against a real server:
#   H1  A handler's code after its first `await` ran in dispatch's final job
#       drain with no request active, so an op started there was detached and
#       never waited for: the response went out early and empty, and the
#       handler's later res.json wrote into a recycled connection slot (a
#       concurrent client crashed the server). Ops the handler starts while
#       another has the connection suspended now run detached but belong to
#       the run, which keeps the request waiting until it completes - so
#       several Hull ops can also be awaited at once (Promise.all / race).
#   H2  req.ctx stored by middleware leaked for every request that never
#       reached a handler (a 404): a few hundred exhausted the JS heap.
#   M3  A transaction opened in that drain (`await null; BEGIN; ...; await p`)
#       slipped past the wait check; app.main never checked at all.
#   M6  A timer's queued jobs ran later inside another entry's context.
#   c_db L2  db.batch(async fn) in a worker VM committed at once.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PORT="${PORT:-19742}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

TMPDIR=$(mktemp -d)
SERVER_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

mkdir -p "$TMPDIR/srv" "$TMPDIR/cli"
cat > "$TMPDIR/srv/app.js" <<'EOF'
import { app } from "hull:app";
import { db } from "hull:db";
import { worker } from "hull:worker";
import { wsServer } from "hull:web:ws-server";
import { wsClient } from "hull:web:ws-client";
app.manifest({ hosts: ["127.0.0.1"],
               modules: ["hull/http-server@1", "hull/timers@1", "hull/db@1",
                         "hull/worker@1", "hull/web/ws-server@1",
                         "hull/web/ws-client@1"] });

const conn = db.default();
conn.exec("CREATE TABLE IF NOT EXISTS t (v TEXT)");

/* M3: a ws-server handler and a ws-client callback that wait holding a
 * transaction (opened after an `await null`) are failed, not continued. */
let wsSeen = "none";
app.ws("/ws", {
    async onMessage(c, m) {
        if (m === "ping") { c.send("pong"); return; }
        const p = hull.sleep(10);
        await null;
        conn.exec("BEGIN");
        conn.exec("INSERT INTO t VALUES ('ws')");
        await p;
        conn.exec("COMMIT");
        c.send("committed");
    },
});
app.get("/wsgo", (req, res) => {
    const url = "ws://127.0.0.1:" + req.query.port + "/ws";
    wsClient.connect(url, {
        onOpen(c) { c.send("txn"); },
        onMessage(c, m) { wsSeen = "server:" + m; },
    });
    wsClient.connect(url, {
        async onOpen(c) {
            const p = hull.sleep(10);
            await null;
            conn.exec("BEGIN");
            conn.exec("INSERT INTO t VALUES ('wc')");
            await p;
            conn.exec("COMMIT");
            c.send("ping");
        },
        onMessage(c, m) { wsSeen = "client:" + m; },
    });
    wsClient.connect(url, {                     /* the control */
        onOpen(c) { c.send("ping"); },
        onMessage(c, m) { wsControl = m; },
    });
    res.text("started");
});
let wsControl = "none";
app.get("/wsseen", (_req, res) => res.text(wsSeen + "/" + wsControl));

/* Every request passes this, 404s included: ~200 KB on req.ctx. */
app.use("*", "/*", (req, res) => { req.ctx.blob = "x".repeat(200000); return 0; });

app.get("/fast", (_req, res) => res.text("fast"));

app.get("/h1", async (_req, res) => {
    const u = hull.sleep(50);
    await null;
    const p = hull.sleep(300);
    await u;
    await p;
    res.json({ ok: 1 });
});

app.get("/all", async (_req, res) => {
    const t0 = Date.now();
    const r = await Promise.all([hull.sleep(200).then(() => 1),
                                 hull.sleep(100).then(() => 2),
                                 hull.sleep(150).then(() => 3)]);
    res.json({ r, ms: Date.now() - t0 });
});

app.get("/race", async (_req, res) => {
    const r = await Promise.race([hull.sleep(30).then(() => "a"),
                                  hull.sleep(400).then(() => "b")]);
    res.json({ r });
});

app.get("/txn", async (_req, res) => {
    const p = hull.sleep(20);
    await null;
    conn.exec("BEGIN");
    conn.exec("INSERT INTO t VALUES ('a')");
    await p;
    conn.exec("INSERT INTO t VALUES ('b')");
    conn.exec("COMMIT");
    res.json({ done: 1 });
});

app.get("/rows", (_req, res) => res.json(conn.query("SELECT v FROM t ORDER BY v")));

let ticks = 0;
app.every(100, async () => {
    const a = hull.sleep(10);
    await null;
    const b = hull.sleep(120);
    await a;
    await b;
    ticks++;
});
app.get("/ticks", (_req, res) => res.json({ ticks }));

app.get("/wbatch", async (_req, res) => {
    const r = await worker.dispatch(() => {
        try {
            db.batch(async () => { db.exec("INSERT INTO t VALUES ('w')"); });
            return "committed";
        } catch (e) { return "refused"; }
    });
    res.json({ r });
});
EOF

"$HULL" --no-sandbox -p "$PORT" -d "$TMPDIR/srv/app.db" "$TMPDIR/srv/app.js" \
    >"$TMPDIR/log" 2>&1 &
SERVER_PID=$!
up=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    if curl -s -m 10 -o /dev/null "http://127.0.0.1:$PORT/fast"; then up=1; break; fi
    sleep 0.5
done
[ "$up" = 1 ] || { echo "FAIL: server did not start"; cat "$TMPDIR/log"; exit 1; }
URL="http://127.0.0.1:$PORT"

echo "=== H1: an op started after the first await belongs to the request ==="
# A concurrent client keeps the connection pool busy meanwhile: the late
# res.json used to land in its slot (and the server died with SIGSEGV).
( for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
      curl -s -m 10 "$URL/fast" >>"$TMPDIR/fast.out"; echo >>"$TMPDIR/fast.out"
  done ) &
BG=$!
H1_PIDS=""
for i in 1 2 3 4; do
    curl -s -m 10 -w ' %{http_code} %{time_total}' "$URL/h1" >"$TMPDIR/h1.$i" &
    H1_PIDS="$H1_PIDS $!"
done
wait $BG
for p in $H1_PIDS; do wait "$p" || true; done
for i in 1 2 3 4; do
    out=$(cat "$TMPDIR/h1.$i")
    body=${out%% *}
    t=$(printf '%s' "$out" | awk '{print $3}')
    if [ "$body" = '{"ok":1}' ] && awk "BEGIN{exit !($t >= 0.28)}"; then
        pass "/h1 #$i answered {\"ok\":1} after the last op (${t}s)"
    else
        fail "/h1 #$i answered after its last op" "$out"
    fi
done
nfast=$(grep -c '^fast$' "$TMPDIR/fast.out" || true)
[ "$nfast" = 20 ] && pass "the concurrent client got all 20 of its own responses" \
    || fail "the concurrent client got its own responses" "$(cat "$TMPDIR/fast.out")"
out=$(curl -s -m 10 "$URL/fast")
[ "$out" = fast ] && pass "the server is still up" || fail "the server is still up" "$out"

echo "=== several Hull ops awaited at once ==="
out=$(curl -s -m 10 "$URL/all")
ms=$(printf '%s' "$out" | sed -n 's/.*"ms":\([0-9]*\).*/\1/p')
case "$out" in
    '{"r":[1,2,3],'*) if [ -n "$ms" ] && [ "$ms" -ge 190 ] && [ "$ms" -lt 450 ]; then
                          pass "Promise.all over three sleeps ($ms ms)"
                      else fail "Promise.all over three sleeps" "$out"; fi ;;
    *) fail "Promise.all over three sleeps" "$out" ;;
esac
out=$(curl -s -m 10 "$URL/race")
[ "$out" = '{"r":"a"}' ] && pass "Promise.race answers with the first" || fail "Promise.race" "$out"
sleep 0.6   # the losing sleep resumes after its request is over
out=$(curl -s -m 10 "$URL/fast")
[ "$out" = fast ] && pass "the race's loser resumed harmlessly" || fail "after the race" "$out"

echo "=== M3: a transaction opened after the first await, then waited across ==="
code=$(curl -s -m 10 -o /dev/null -w '%{http_code}' "$URL/txn")
[ "$code" = 500 ] && pass "the run is failed (500)" || fail "the run is failed (500)" "$code"
out=$(curl -s -m 10 "$URL/rows")
case "$out" in
    *'"a"'*|*'"b"'*) fail "nothing of the transaction is committed" "$out" ;;
    *) pass "nothing of the transaction is committed" ;;
esac
grep -q "waited while a transaction was open" "$TMPDIR/log" \
    && pass "the wait is reported" || fail "the wait is reported" "$(tail -5 "$TMPDIR/log")"

echo "=== M3: ws-server handler and ws-client callback waiting in a transaction ==="
curl -s -m 10 -o /dev/null "$URL/wsgo?port=$PORT"
sleep 1.5
out=$(curl -s -m 10 "$URL/wsseen")
[ "$out" = none/pong ] && pass "neither callback went on (the control got its pong)" \
    || fail "neither callback went on (the control got its pong)" "$out"
out=$(curl -s -m 10 "$URL/rows")
case "$out" in
    *'"ws"'*|*'"wc"'*) fail "nothing of either transaction is committed" "$out" ;;
    *) pass "nothing of either transaction is committed" ;;
esac

echo "=== H2: middleware ctx of requests that never reach a handler ==="
i=0
while [ $i -lt 400 ]; do
    curl -s -m 10 -o /dev/null "$URL/no-such-path-$i"
    i=$((i + 1))
done
code=$(curl -s -m 10 -o "$TMPDIR/after404" -w '%{http_code}' "$URL/fast")
[ "$code" = 200 ] && [ "$(cat "$TMPDIR/after404")" = fast ] \
    && pass "400 x 404 behind a 200 KB ctx leave the heap usable" \
    || fail "400 x 404 behind a 200 KB ctx leave the heap usable" "$code $(cat "$TMPDIR/after404")"

echo "=== M6: an async timer's queued jobs are its own ==="
out=$(curl -s -m 10 "$URL/ticks")
n=$(printf '%s' "$out" | sed -n 's/.*"ticks":\([0-9]*\).*/\1/p')
[ -n "$n" ] && [ "$n" -ge 3 ] && pass "the timer keeps completing ($n runs)" \
    || fail "the timer keeps completing" "$out"

echo "=== c_db L2: db.batch(async fn) in a worker VM ==="
out=$(curl -s -m 10 "$URL/wbatch")
[ "$out" = '{"r":"refused"}' ] && pass "refused before BEGIN" || fail "refused before BEGIN" "$out"
out=$(curl -s -m 10 "$URL/rows")
case "$out" in
    *'"w"'*) fail "nothing committed" "$out" ;;
    *) pass "nothing committed" ;;
esac

kill "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

echo "=== M3: async app.main waiting in a transaction ==="
cat > "$TMPDIR/cli/app.js" <<'EOF'
import { app } from "hull:app";
import { db } from "hull:db";
app.manifest({ modules: ["hull/db@1"] });
app.main(async () => {
    const c = db.default();
    c.exec("CREATE TABLE IF NOT EXISTS m (v TEXT)");
    const p = hull.sleep(20);
    await null;
    c.exec("BEGIN");
    c.exec("INSERT INTO m VALUES ('x')");
    await p;
    c.exec("COMMIT");
    console.log("committed");
    return 0;
});
EOF
"$HULL" --no-sandbox -d "$TMPDIR/cli/app.db" "$TMPDIR/cli/app.js" >"$TMPDIR/cli.log" 2>&1 || true
if grep -q "waited holding a database transaction" "$TMPDIR/cli.log" \
   && ! grep -q committed "$TMPDIR/cli.log"; then
    pass "app.main fails instead of committing in autocommit"
else
    fail "app.main fails instead of committing in autocommit" "$(cat "$TMPDIR/cli.log")"
fi

echo ""
echo "e2e_js_runs: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
