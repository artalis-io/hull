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
#       reached a handler (a 404): a few hundred exhausted the JS heap. Freed
#       when the response is sent (request_done), even on a connection that
#       stays open.
#   M3  A transaction opened in that drain (`await null; BEGIN; ...; await p`)
#       slipped past the wait check; app.main never checked at all.
#   M6  A timer's queued jobs ran later inside another entry's context.
#   c_db L2  db.batch(async fn) in a worker VM committed at once.
#
# Audit 8:
#   H1  An op made while a connection was active but its request life was
#       not (an Object.prototype `then` getter read by a resume's resolve, an
#       inherited setter while `req` was built) suspended the connection
#       uncounted; it belongs to the run now, and `req` runs no setter.
#   M7  A resumed handler that waits, in a transaction, on an op it made
#       before the BEGIN is checked too.
#   M9  tui.poll in an HTTP handler is refused.
#   L2  A stashed res is compressed for its own request.
#   c_db L1  A wait on a promise Hull does not drive is checked too.
#
# Audit 15:
#   L   A ws client whose handle the app dropped was garbage to the cycle
#       collector (its self-reference was reported as a child) and was
#       closed at the next collection; it stays a root while connected.
#
# Audit 30:
#   H   conn.close() from a ws client's onClose armed the close deadline after
#       the connection was over; the client was freed and the deadline fired
#       5 s later into freed memory (ASan: heap-use-after-free).
#   M   A handler that awaited a promise Hull does not drive (its request was
#       ended) resumed inside the request that settled it, and an op it then
#       made was that request's: it held the other client's response for its
#       whole wait.
#
# Audit 31:
#   H   The wait check ran only for runs an entry waits on. A continuation no
#       entry waits on (`hull.sleep(5).then(async () => ...)` from a
#       synchronous handler) or code of another run (a handler that awaited a
#       promise another request settled) could `const p = hull.sleep(20);
#       BEGIN; INSERT; await p`: the first write was rolled back unseen, the
#       rest autocommitted - and the request whose drain ran that code had its
#       own acknowledged write rolled back with the other run's transaction.
#
# Audit 32:
#   M   A run failed by that check (its stretch of jobs ended holding a
#       transaction) went on running in the same drain: its code after the
#       await wrote in autocommit and its request was answered 200. Its jobs
#       are now dropped and the run ends failed (500).
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

/* Audit 15: a ws client whose handle the app dropped is a GC root while
 * connected - it was collected (and closed) at the next cycle collection. */
const pushConns = [];
app.ws("/push", { onOpen(c) { pushConns.push(c); }, onMessage(c, m) {} });
let dropGot = "none";
app.get("/wsdrop", (req, res) => {
    wsClient.connect("ws://127.0.0.1:" + req.query.port + "/push", {
        onMessage(c, m) { dropGot = m; },
    });
    res.text("started");
});
app.get("/gc", (_req, res) => {
    /* Self-referencing garbage refcounting cannot free: the cycle
     * collector runs, several times over. */
    for (let i = 0; i < 300000; i++) { const o = { i }; o.self = o; }
    res.text("gc");
});
app.get("/pushnow", (_req, res) => {
    let n = 0;
    for (const c of pushConns) { try { c.send("after-gc"); n++; } catch (_) {} }
    res.text(String(n));
});
app.get("/dropgot", (_req, res) => res.text(dropGot));

/* Audit 30 H: the server closes; the client's onClose calls close() on the
 * finished connection and drops it (nothing keeps the object alive). */
app.ws("/closer", { onOpen(c) { c.close(1000); }, onMessage(c, m) {} });
let closeCalls = 0;
app.get("/wsclose", (req, res) => {
    wsClient.connect("ws://127.0.0.1:" + req.query.port + "/closer", {
        onClose(c) { closeCalls++; c.close(); c.close(4000, "again"); },
    });
    res.text("started");
});
app.get("/closecalls", (_req, res) => res.text(String(closeCalls)));

/* Audit 30 M: /ra waits on a promise only /rb settles, then on its own
 * sleep. /rb must not wait for /ra's sleep. */
let raWake = null;
app.get("/ra", async (_req, res) => {
    await new Promise((r) => { raWake = r; });
    await hull.sleep(1500);
    try { res.json({ ok: "a" }); } catch (_e) {}
});
app.get("/rb", async (_req, res) => {
    if (raWake) { raWake(); raWake = null; }
    await null;
    res.json({ ok: "b" });
});

/* Audit 31: an un-awaited chain waits holding a transaction (u1 / u2 must
 * not commit); the control (u3) opens and commits in one stretch. */
let u31 = "none";
app.get("/u31", (_req, res) => {
    hull.sleep(5).then(async () => {
        const p = hull.sleep(20);
        conn.exec("BEGIN");
        conn.exec("INSERT INTO t VALUES ('u1')");
        await p;
        conn.exec("INSERT INTO t VALUES ('u2')");
        try { conn.exec("COMMIT"); u31 = "committed"; } catch (_e) { u31 = "threw"; }
    });
    hull.sleep(5).then(async () => {
        await hull.sleep(5);
        conn.exec("BEGIN");
        conn.exec("INSERT INTO t VALUES ('u3')");
        conn.exec("COMMIT");
    });
    res.text("started");
});
app.get("/u31state", (_req, res) => res.text(u31));
/* ...and another request's code, run in /x31b's drain, opens one and waits:
 * /x31b's own acknowledged write (xb) must survive, xa1 / xa2 must not
 * commit. /y31a's code, run the same way, commits in one stretch (yc). */
let x31Wake = null, y31Wake = null;
app.get("/x31a", async (_req, res) => {
    res.text("a");
    await new Promise((r) => { x31Wake = r; });
    const p = hull.sleep(20);
    conn.exec("BEGIN");
    conn.exec("INSERT INTO t VALUES ('xa1')");
    await p;
    conn.exec("INSERT INTO t VALUES ('xa2')");
    try { conn.exec("COMMIT"); } catch (_e) {}
});
app.get("/x31b", async (_req, res) => {
    if (x31Wake) { x31Wake(); x31Wake = null; }
    await null;
    conn.exec("INSERT INTO t VALUES ('xb')");
    res.text("b");
});
/* ...and the reverse: /r31c is inside a transaction across `await null`
 * when /r31d's code (woken by /r31c) is queued: run there, its write (rd)
 * joined /r31c's transaction and went with its ROLLBACK. */
let r31Wake = null, r31d = "none";
app.get("/r31d", async (_req, res) => {
    res.text("d");
    await new Promise((r) => { r31Wake = r; });
    conn.exec("INSERT INTO t VALUES ('rd')");
    r31d = conn.inTransaction() ? "inside" : "alone";
});
app.get("/r31c", async (_req, res) => {
    conn.exec("BEGIN");
    conn.exec("INSERT INTO t VALUES ('rc1')");
    if (r31Wake) { r31Wake(); r31Wake = null; }
    await null;
    conn.exec("INSERT INTO t VALUES ('rc2')");
    conn.exec("ROLLBACK");
    res.text("c");
});
app.get("/r31state", (_req, res) => res.text(r31d));

/* Audit 32: /f32b holds a transaction and awaits a promise only another
 * run's job settles (/f32a's, woken by /f32b): its stretch ends holding
 * the transaction, which is rolled back and the run failed. Its own code
 * after the await used to run on in the same drain - 'fb2' autocommitted
 * and the request was answered 200 (a stray COMMIT succeeds on Postgres /
 * MySQL). Now it is not continued: 500, and neither write committed. */
let f32Wake = null, f32Wake2 = null, f32After = "none";
const f32Gate = new Promise(r => { f32Wake = r; });
const f32Gate2 = new Promise(r => { f32Wake2 = r; });
app.get("/f32a", async (_req, res) => {
    await f32Gate;
    f32Wake2();
    res.text("a");
});
app.get("/f32b", async (_req, res) => {
    conn.exec("BEGIN");
    conn.exec("INSERT INTO t VALUES ('fb1')");
    f32Wake();
    await f32Gate2;
    f32After = "continued";
    conn.exec("INSERT INTO t VALUES ('fb2')");
    try { conn.exec("COMMIT"); } catch (e) { /* SQLite: no transaction */ }
    res.text("b");
});
app.get("/f32state", (_req, res) => res.text(f32After));
app.get("/y31a", async (_req, res) => {
    res.text("a");
    await new Promise((r) => { y31Wake = r; });
    conn.exec("BEGIN");
    conn.exec("INSERT INTO t VALUES ('yc')");
    conn.exec("COMMIT");
});
app.get("/y31b", (_req, res) => {
    if (y31Wake) { y31Wake(); y31Wake = null; }
    res.text("b");
});

/* Every request passes this, 404s included: ~200 KB on req.ctx. */
app.use("*", "/*", (req, res) => { req.ctx.blob = "x".repeat(200000); return 0; });
/* ...and these another 1 MB: 80 held open would be 96 MB of a 64 MB heap. */
app.use("*", "/big/*", (req, res) => { req.ctx.big = "y".repeat(1000000); return 0; });

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

/* Audit 8 H1: an op made while a resume settles its promise - a `then`
 * getter on Object.prototype, read from db.async's row array by the
 * resolve - suspended the connection uncounted: the response went out at
 * once and the op later completed on a recycled slot. An inherited
 * `remote_addr` setter likewise ran app code while `req` was built.
 * Audit 18: the intrinsics are frozen, so neither accessor can be defined
 * any more - the app records the refusals (/h1-refused), and the resume path
 * itself (db.async settling a handler, beside a keep-alive client) is still
 * exercised by /h1then. */
const h1Refused = {};
for (const k of ["then", "remote_addr"]) {
    try {
        Object.defineProperty(Object.prototype, k, { configurable: true,
            get() { return undefined; }, set(_v) {} });
        h1Refused[k] = false;
    } catch (e) {
        h1Refused[k] = e instanceof TypeError;
    }
}
app.get("/h1-refused", (_req, res) => res.json(h1Refused));
app.get("/h1then", async (_req, res) => {
    const p = conn.async.query("SELECT 1 AS one");
    res.json(await p);
});

/* Audit 8 M7: a resumed handler that opens a transaction and then waits on an
 * op made BEFORE it (no new op in that segment) was not checked: the
 * transaction was rolled back unseen and the rest autocommitted. */
app.get("/txn2", async (_req, res) => {
    const pa = hull.sleep(10), pb = hull.sleep(150);
    await pa;
    conn.exec("BEGIN");
    conn.exec("INSERT INTO t VALUES ('c')");
    await pb;
    conn.exec("INSERT INTO t VALUES ('d')");
    conn.exec("COMMIT");
    res.json({ done: 1 });
});

/* Audit 8 c_db L1: waiting on a promise Hull does not drive, in a
 * transaction: reported (and rolled back) like any other wait. */
let undriven = null;
app.get("/txn3", async (_req, res) => {
    conn.exec("BEGIN");
    conn.exec("INSERT INTO t VALUES ('e')");
    await new Promise((r) => { undriven = r; });
    try { conn.exec("COMMIT"); } catch (_e) {}
});
app.get("/txn3-go", (_req, res) => {
    if (undriven) { undriven(); undriven = null; }
    res.text("ok");
});

/* Audit 8 L2: a `res` stashed by one request and answered from another's
 * handler is compressed for ITS request's Accept-Encoding, not the active
 * one's. */
const waiters = [];
app.get("/l2wait", async (_req, res) => {
    const w = { res, done: false };
    waiters.push(w);
    while (!w.done) await hull.sleep(30);
});
app.get("/l2pub", (_req, res) => {
    const big = { s: "z".repeat(8000) };
    for (const w of waiters.splice(0)) { w.res.json(big); w.done = true; }
    res.text("published");
});

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

echo "=== audit 15: a ws client whose handle was dropped survives a GC ==="
curl -s -m 10 -o /dev/null "$URL/wsdrop?port=$PORT"
sleep 0.5
curl -s -m 30 -o /dev/null "$URL/gc"
curl -s -m 10 -o /dev/null "$URL/pushnow"
sleep 0.5
out=$(curl -s -m 10 "$URL/dropgot")
[ "$out" = after-gc ] && pass "the dropped client still receives after a GC"     || fail "the dropped client still receives after a GC" "$out"

echo "=== audit 30 H: conn.close() inside a ws client's onClose ==="
curl -s -m 10 -o /dev/null "$URL/wsclose?port=$PORT"
sleep 0.5
curl -s -m 30 -o /dev/null "$URL/gc"    # the dropped client is finalized
# Past the 5 s close deadline: armed by the close() in onClose, it fired
# into the freed client (a crash under ASan, maybe a second onClose).
sleep 6
out=$(curl -s -m 10 "$URL/closecalls")
[ "$out" = 1 ] && pass "onClose ran once; close() in it armed no deadline" \
    || fail "onClose ran once; close() in it armed no deadline" "$out"
out=$(curl -s -m 10 "$URL/fast")
[ "$out" = fast ] && pass "the server is still up" || fail "the server is still up" "$out"

echo "=== audit 30 M: an ended run's op is not the request that resumed it ==="
curl -s -m 10 -o "$TMPDIR/ra.out" "$URL/ra" || true
out=$(curl -s -m 10 -w ' %{time_total}' "$URL/rb")
body=${out%% *}
t=${out##* }
if [ "$body" = '{"ok":"b"}' ] && awk "BEGIN{exit !($t < 1.0)}"; then
    pass "/rb answered at once (${t}s), not after /ra's sleep"
else
    fail "/rb answered at once, not after /ra's sleep" "$out"
fi
sleep 1.8   # /ra's sleep resumes, detached, after both requests are over
out=$(curl -s -m 10 "$URL/fast")
[ "$out" = fast ] && pass "the ended run's op resumed harmlessly" || fail "after /ra's op" "$out"

echo "=== audit 31: a transaction held across a wait by code no entry waits on ==="
before=$(grep -c "async code waited while a transaction was open" "$TMPDIR/log" || true)
curl -s -m 10 -o /dev/null "$URL/u31"
sleep 0.5
out=$(curl -s -m 10 "$URL/rows")
case "$out" in
    *'"u1"'*|*'"u2"'*) fail "nothing of the un-awaited chain's transaction is committed" "$out" ;;
    *) pass "nothing of the un-awaited chain's transaction is committed" ;;
esac
case "$out" in
    *'"u3"'*) pass "a transaction opened and committed in one stretch still commits" ;;
    *) fail "a transaction opened and committed in one stretch still commits" "$out" ;;
esac
out=$(curl -s -m 10 "$URL/u31state")
[ "$out" = none ] && pass "the chain is not continued past its wait" \
    || fail "the chain is not continued past its wait" "$out"
curl -s -m 10 -o /dev/null "$URL/x31a"
out=$(curl -s -m 10 "$URL/x31b")
sleep 0.5
rows=$(curl -s -m 10 "$URL/rows")
case "$rows" in
    *'"xb"'*) [ "$out" = b ] && pass "the other request's acknowledged write survives" \
                  || fail "the other request is answered" "$out" ;;
    *) fail "the other request's acknowledged write survives" "$rows" ;;
esac
case "$rows" in
    *'"xa1"'*|*'"xa2"'*) fail "nothing of the ended run's transaction is committed" "$rows" ;;
    *) pass "nothing of the ended run's transaction is committed" ;;
esac
after=$(grep -c "async code waited while a transaction was open" "$TMPDIR/log" || true)
[ "$after" -ge $((before + 2)) ] && pass "both waits are reported" \
    || fail "both waits are reported" "$before -> $after"
curl -s -m 10 -o /dev/null "$URL/y31a"
curl -s -m 10 -o /dev/null "$URL/y31b"
sleep 0.2
out=$(curl -s -m 10 "$URL/rows")
case "$out" in
    *'"yc"'*) pass "another run's transaction committed in one stretch still commits" ;;
    *) fail "another run's transaction committed in one stretch still commits" "$out" ;;
esac

echo "=== audit 31: another run's job queued while a transaction spans await null ==="
curl -s -m 10 -o /dev/null "$URL/r31d"
out=$(curl -s -m 10 -w ' %{http_code}' "$URL/r31c")
[ "$out" = "c 200" ] && pass "the run with the transaction finishes normally" \
    || fail "the run with the transaction finishes normally" "$out"
sleep 0.2
out=$(curl -s -m 10 "$URL/r31state")
[ "$out" = alone ] && pass "the other run's code ran outside the transaction" \
    || fail "the other run's code ran outside the transaction" "$out"
rows=$(curl -s -m 10 "$URL/rows")
case "$rows" in
    *'"rd"'*) pass "the other run's write survives the rollback" ;;
    *) fail "the other run's write survives the rollback" "$rows" ;;
esac
case "$rows" in
    *'"rc1"'*|*'"rc2"'*) fail "the rolled-back transaction committed nothing" "$rows" ;;
    *) pass "the rolled-back transaction committed nothing" ;;
esac

echo "=== audit 32: a run failed for holding a transaction stops running ==="
curl -s -m 10 -o /dev/null "$URL/f32a" || true
code=$(curl -s -m 10 -o /dev/null -w '%{http_code}' "$URL/f32b")
[ "$code" = 500 ] && pass "the failed run is answered 500" \
    || fail "the failed run is answered 500" "$code"
out=$(curl -s -m 10 "$URL/f32state")
[ "$out" = none ] && pass "its code after the await does not run" \
    || fail "its code after the await does not run" "$out"
rows=$(curl -s -m 10 "$URL/rows")
case "$rows" in
    *'"fb1"'*|*'"fb2"'*) fail "neither of its writes is committed" "$rows" ;;
    *) pass "neither of its writes is committed" ;;
esac
grep -q "handler failed: the run waited holding a database transaction" "$TMPDIR/log" \
    && pass "the failure is reported" || fail "the failure is reported" "$(tail -5 "$TMPDIR/log")"
out=$(curl -s -m 10 "$URL/fast")
[ "$out" = fast ] && pass "the server is still up" || fail "the server is still up" "$out"

echo "=== audit 8 H1 / audit 18: the inherited accessors cannot be defined ==="
# Audit 18: the intrinsics are frozen, so the Object.prototype `then` getter
# and `remote_addr` setter the H1 legs planted are refused at load.
out=$(curl -s -m 10 "$URL/h1-refused")
[ "$out" = '{"then":true,"remote_addr":true}' ] \
    && pass "Object.prototype then / remote_addr are refused" \
    || fail "Object.prototype then / remote_addr are refused" "$out"

echo "=== audit 8 H1: a resume settling its promise beside a keep-alive client ==="
# A keep-alive client (one connection, many requests) runs meanwhile: the
# stray op used to complete on a slot it was being served from.
KA_URLS=""
i=0
while [ $i -lt 40 ]; do KA_URLS="$KA_URLS $URL/fast"; i=$((i + 1)); done
# shellcheck disable=SC2086
( curl -s -m 30 $KA_URLS >"$TMPDIR/ka.out" ) &
KA=$!
HT_PIDS=""
for i in 1 2 3; do
    curl -s -m 10 -w ' %{http_code} %{time_total}' "$URL/h1then" >"$TMPDIR/ht.$i" &
    HT_PIDS="$HT_PIDS $!"
    sleep 0.1
done
for p in $HT_PIDS; do wait "$p" || true; done
wait $KA || true
for i in 1 2 3; do
    out=$(cat "$TMPDIR/ht.$i")
    body=${out%% *}
    code=$(printf '%s' "$out" | awk '{print $2}')
    if [ "$body" = '[{"one":1}]' ] && [ "$code" = 200 ]; then
        pass "/h1then #$i answered with its own rows"
    else
        fail "/h1then #$i answered with its own rows" "$out"
    fi
done
nka=$(grep -o fast "$TMPDIR/ka.out" | wc -l | tr -d ' ')
[ "$nka" = 40 ] && pass "the keep-alive client got all 40 of its own responses" \
    || fail "the keep-alive client got its own responses" "$nka: $(cat "$TMPDIR/ka.out")"
sleep 0.4
out=$(curl -s -m 10 "$URL/fast")
[ "$out" = fast ] && pass "the server is still up" || fail "the server is still up" "$out"

echo "=== audit 8 M7: waiting, in a transaction, on an op made before it ==="
before=$(grep -c "waited while a transaction was open" "$TMPDIR/log" || true)
code=$(curl -s -m 10 -o /dev/null -w '%{http_code}' "$URL/txn2")
[ "$code" = 500 ] && pass "the run is failed (500)" || fail "the run is failed (500)" "$code"
out=$(curl -s -m 10 "$URL/rows")
case "$out" in
    *'"c"'*|*'"d"'*) fail "nothing after the wait is committed" "$out" ;;
    *) pass "nothing after the wait is committed" ;;
esac
after=$(grep -c "waited while a transaction was open" "$TMPDIR/log" || true)
[ "$after" -gt "$before" ] && pass "the wait is reported" || fail "the wait is reported" "$before -> $after"

echo "=== audit 8 c_db L1: waiting on an undriven promise in a transaction ==="
before=$after
curl -s -m 10 -o /dev/null "$URL/txn3"
curl -s -m 10 -o /dev/null "$URL/txn3-go"
after=$(grep -c "waited while a transaction was open" "$TMPDIR/log" || true)
[ "$after" -gt "$before" ] && pass "the wait is reported" || fail "the wait is reported" "$before -> $after"
out=$(curl -s -m 10 "$URL/rows")
case "$out" in
    *'"e"'*) fail "nothing of the transaction is committed" "$out" ;;
    *) pass "nothing of the transaction is committed" ;;
esac

echo "=== audit 8 L2: a stashed res answered from another request ==="
curl -s -m 10 -D "$TMPDIR/l2.hdr" -o "$TMPDIR/l2.body" "$URL/l2wait" &
L2=$!
sleep 0.3
out=$(curl -s -m 10 -H 'Accept-Encoding: gzip' "$URL/l2pub")
wait $L2 || true
if [ "$out" = published ] && ! grep -qi '^content-encoding' "$TMPDIR/l2.hdr" \
   && grep -q zzzz "$TMPDIR/l2.body"; then
    pass "not compressed for the publisher's Accept-Encoding"
else
    fail "not compressed for the publisher's Accept-Encoding" "$out $(cat "$TMPDIR/l2.hdr")"
fi

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

# Connections that stay open after their 404: no later request on their slot
# frees the ctx, so it has to go when the response is sent (request_done).
if command -v python3 >/dev/null 2>&1; then
    cat > "$TMPDIR/hold.py" <<'EOF'
import socket, sys
port, n = int(sys.argv[1]), int(sys.argv[2])
crlf = bytes([13, 10])
def get(s, path):
    s.sendall(b"GET " + path + b" HTTP/1.1" + crlf + b"Host: 127.0.0.1" + crlf + crlf)
    data = b""
    while crlf + crlf not in data:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    parts = data.split(b" ")
    return parts[1].decode() if len(parts) > 1 else "none"
held, codes = [], {}
for i in range(n):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    c = get(s, b"/big/no-such-" + str(i).encode())
    codes[c] = codes.get(c, 0) + 1
    held.append(s)
f = socket.create_connection(("127.0.0.1", port), timeout=10)
print(" ".join("%s:%d" % kv for kv in sorted(codes.items())), "fast:" + get(f, b"/fast"))
f.close()
for s in held:
    s.close()
EOF
    out=$(python3 "$TMPDIR/hold.py" "$PORT" 80 2>&1 || true)
    [ "$out" = "404:80 fast:200" ] \
        && pass "80 open connections after a 404 behind a 1.2 MB ctx hold none of it" \
        || fail "80 open connections after a 404 behind a 1.2 MB ctx hold none of it" "$out"
else
    echo "  SKIP: python3 not found (open-connection 404 case)"
fi

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

# Under an ASan build (make debug), a use after free above is reported here.
if grep -q "AddressSanitizer" "$TMPDIR/log"; then
    fail "the server log has no ASan report" "$(grep -m1 -A15 AddressSanitizer "$TMPDIR/log")"
fi
kill "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

echo "=== audit 8 M9: tui.poll in an HTTP handler ==="
# tui.poll's continuation was counted as the op holding the request's
# connection but never suspended it: an empty 200 at once, then res.json
# into a recycled slot. The terminal UI is refused while serving a request.
mkdir -p "$TMPDIR/tui"
cat > "$TMPDIR/tui/app.js" <<'EOF'
import { app } from "hull:app";
import { tui } from "hull:tui";
app.manifest({ tui: true, modules: ["hull/http-server@1", "hull/tui@1?"] });
app.get("/fast", (_req, res) => res.text("fast"));
app.get("/k", async (_req, res) => {
    if (!tui) { res.text("absent"); return; }
    try { const ev = await tui.poll(500); res.json({ ev }); }
    catch (e) { res.text(String(e.message)); }
});
EOF
TPORT=$((PORT + 1))
"$HULL" --no-sandbox -p "$TPORT" "$TMPDIR/tui/app.js" </dev/null >"$TMPDIR/tui.log" 2>&1 &
SERVER_PID=$!
up=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if curl -s -m 10 -o /dev/null "http://127.0.0.1:$TPORT/fast"; then up=1; break; fi
    sleep 0.5
done
if [ "$up" = 1 ]; then
    out=$(curl -s -m 10 "http://127.0.0.1:$TPORT/k")
    case "$out" in
        absent) echo "  SKIP: hull/tui is not in this build" ;;
        *"cannot be used while serving an HTTP request"*) pass "tui.poll is refused in a handler" ;;
        *) fail "tui.poll is refused in a handler" "$out" ;;
    esac
else
    fail "the tui app starts" "$(tail -5 "$TMPDIR/tui.log")"
fi
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
