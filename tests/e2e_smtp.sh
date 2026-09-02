#!/bin/sh
# e2e_smtp.sh - model-2 async SMTP end to end (Lua AND JS bindings) vs a mock peer.
#
# Proves the checkpoint-1/2 behaviors on a live server (the keel async backend, the
# default build), for BOTH runtimes, and asserts equivalent result objects + audit
# records across Lua and JS: ordinary async completion + a registry left empty;
# prompt cancellation after resolution (a stalled peer shuts down well below the
# SMTP timeout); cap-zero resolving immediately and never blocking; exact
# (untruncated) scheduling-failure audit metadata; and a clean shutdown with an op
# in flight (re-run under the ASan debug build). The done_fn drain-vs-drop backend
# parity (keel drains, poll drops, both release once) is unit-covered by
# test_smtp_async; a real poll-backend shutdown run is a separate §15 gate item.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u

HULL="${HULL_BIN:-./build/hull}"
PORT_HTTP=8811
PORT_SMTP=2529
APPDIR="$(mktemp -d)"
FAILS=0
PIDS=""

log()  { printf '%s\n' "$*"; }
pass() { printf '  ok   %s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; FAILS=$((FAILS+1)); }

cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    pkill -f "smtp_mock_$$" 2>/dev/null
    rm -rf "$APPDIR"
}
trap cleanup EXIT INT TERM

command -v python3 >/dev/null 2>&1 || { log "e2e_smtp: python3 not found - skipping"; exit 0; }
[ -x "$HULL" ] || { log "e2e_smtp: $HULL not built - skipping"; exit 0; }

# ── mock SMTP peer (mode: fast | slow) ──────────────────────────────────
cat > "$APPDIR/smtp_mock_$$.py" <<'PY'
import socket, sys, threading, time
port = int(sys.argv[1]); mode = sys.argv[2]
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port)); srv.listen(16)
sys.stderr.write("ready\n"); sys.stderr.flush()
def handle(c):
    if mode == "slow":
        time.sleep(30)
        try: c.sendall(b"220 slow\r\n")
        except Exception: pass
        c.close(); return
    if mode == "brief":
        # Hold the admitted send parked (no greeting) for a beat, then complete the
        # conversation normally - so the caller can RELEASE it cleanly.
        time.sleep(3)
    f = c.makefile("rwb", buffering=0)
    f.write(b"220 mock\r\n")
    while True:
        line = f.readline()
        if not line: break
        u = line.upper()
        if u.startswith(b"DATA"):
            f.write(b"354 go\r\n")
            while True:
                d = f.readline()
                if d == b".\r\n" or not d: break
            f.write(b"250 queued\r\n")
        elif u.startswith(b"QUIT"):
            f.write(b"221 bye\r\n"); break
        else:
            f.write(b"250 ok\r\n")
    c.close()
while True:
    try: cc,_ = srv.accept()
    except Exception: break
    threading.Thread(target=handle, args=(cc,), daemon=True).start()
PY

start_mock() {
    python3 "$APPDIR/smtp_mock_$$.py" "$PORT_SMTP" "$1" 2>"$APPDIR/mock.err" &
    MOCK_PID=$!; PIDS="$PIDS $MOCK_PID"
    for _ in $(seq 1 50); do grep -q ready "$APPDIR/mock.err" 2>/dev/null && return 0; sleep 0.1; done
}
stop_mock() { kill "$MOCK_PID" 2>/dev/null; }

# ── the apps (Lua yields transparently; JS awaits + returns a Promise) ──
LONGSUBJ=$(python3 -c "print('S'*400)")
cat > "$APPDIR/app.lua" <<LUA
local smtp = require("hull.smtp")
app.manifest({ modules = { "hull/smtp@1", "hull/http-server@1" }, hosts = { "127.0.0.1" } })
app.get("/",       function(req, res) res:json({ ready=true }) end)
app.get("/send",   function(req, res) res:json(smtp.send({ host="127.0.0.1", port=$PORT_SMTP, from="a@x", to="b@y", subject="hi", body="hello" })) end)
app.get("/long",   function(req, res) res:json(smtp.send({ host="127.0.0.1", port=$PORT_SMTP, from="a@x", to="b@y", subject="$LONGSUBJ", body="b" })) end)
app.get("/denied", function(req, res) res:json(smtp.send({ host="evil.example.com", port=25, from="a@x", to="b@y", subject="s", body="b" })) end)
LUA
cat > "$APPDIR/app.js" <<JS
import { app } from "hull:app";
import { smtp } from "hull:smtp";
app.manifest({ modules: ["hull/smtp@1", "hull/http-server@1"], hosts: ["127.0.0.1"] });
app.get("/",       (req, res) => res.json({ ready: true }));
app.get("/send",   async (req, res) => res.json(await smtp.send({ host:"127.0.0.1", port:$PORT_SMTP, from:"a@x", to:"b@y", subject:"hi", body:"hello" })));
app.get("/long",   async (req, res) => res.json(await smtp.send({ host:"127.0.0.1", port:$PORT_SMTP, from:"a@x", to:"b@y", subject:"$LONGSUBJ", body:"b" })));
app.get("/denied", async (req, res) => res.json(await smtp.send({ host:"evil.example.com", port:25, from:"a@x", to:"b@y", subject:"s", body:"b" })));
JS

# ── db.async-under-saturation apps: SMTP + a REAL db.async route ─────────
# The /db route exercises a genuine db.async operation (continuation suspend ->
# worker query -> completion dispatch; the JS path also drives last_async_cont),
# so a bounded DB completion while SMTP is saturated proves the admission cap left
# a pool worker for db/compute - the integration a generic pool callback cannot.
cat > "$APPDIR/app_db.lua" <<LUA
local smtp = require("hull.smtp")
local db = require("hull.db").default()
app.manifest({ modules = { "hull/smtp@1", "hull/db@1", "hull/http-server@1" }, hosts = { "127.0.0.1" } })
app.get("/",     function(req, res) res:json({ ready=true }) end)
app.get("/send", function(req, res) res:json(smtp.send({ host="127.0.0.1", port=$PORT_SMTP, from="a@x", to="b@y", subject="hi", body="hello" })) end)
app.get("/db",   function(req, res)
    local rows = db.async.query("SELECT 1 + 1 as result")
    res:json({ ok=true, result = rows[1].result })
end)
LUA
cat > "$APPDIR/app_db.js" <<JS
import { app } from "hull:app";
import { smtp } from "hull:smtp";
import { db as dbModule } from "hull:db";
const db = dbModule.default();
app.manifest({ modules: ["hull/smtp@1", "hull/db@1", "hull/http-server@1"], hosts: ["127.0.0.1"] });
app.get("/",     (req, res) => res.json({ ready: true }));
app.get("/send", async (req, res) => res.json(await smtp.send({ host:"127.0.0.1", port:$PORT_SMTP, from:"a@x", to:"b@y", subject:"hi", body:"hello" })));
app.get("/db",   async (req, res) => {
    const rows = await db.async.query("SELECT 1 + 1 as result");
    res.json({ ok: true, result: rows[0].result });
});
JS

# start_hull <appfile> [extra-args...] ; log -> $APPDIR/hull.log
start_hull() {
    APP="$1"; shift
    "$HULL" "$APP" -p "$PORT_HTTP" --audit "$@" >"$APPDIR/hull.log" 2>&1 &
    HULL_PID=$!; PIDS="$PIDS $HULL_PID"
    for _ in $(seq 1 100); do
        curl -s "http://127.0.0.1:$PORT_HTTP/" 2>/dev/null | grep -q ready && break
        kill -0 "$HULL_PID" 2>/dev/null || return 1
        sleep 0.2
    done
}

# longest subject across all cap_reached audits (the /long send, not /send's "hi")
subj_len_from_audit() {
    python3 - "$APPDIR/hull.log" <<'PY'
import json,sys
best=0
for line in open(sys.argv[1]):
    if '"cap":"smtp.send"' in line and "cap_reached" in line:
        best=max(best, len(json.loads(line[line.index("{"):])["subject"]))
print(best)
PY
}

# canonicalise a JSON result object (sorted keys) so Lua/JS key-order differences
# (semantically identical) don't register as a difference.
norm() { python3 -c "import json,sys; print(json.dumps(json.loads(sys.stdin.read()), sort_keys=True))"; }

# ── per-runtime suite; records results into $APPDIR/res.<rt> for parity ──
run_suite() { # <label> <appfile>
    RT="$1"; APP="$2"
    log "--- runtime: $RT ---"

    # 1. ordinary completion + registry empty
    start_mock fast; start_hull "$APP"
    R_SEND=$(curl -s --max-time 10 "http://127.0.0.1:$PORT_HTTP/send")
    R_DENIED=$(curl -s --max-time 10 "http://127.0.0.1:$PORT_HTTP/denied")
    case "$R_SEND" in *'"ok":true'*) pass "$RT async success: $R_SEND" ;;
                       *) fail "$RT async success (got: $R_SEND)" ;; esac
    case "$R_DENIED" in *'host_not_allowed'*) pass "$RT host denial before submit: $R_DENIED" ;;
                        *) fail "$RT host denial (got: $R_DENIED)" ;; esac
    kill -INT "$HULL_PID" 2>/dev/null; wait "$HULL_PID" 2>/dev/null
    if grep -q '"terminal":"cancelled"' "$APPDIR/hull.log"; then
        fail "$RT registry not empty after completion"
    else
        pass "$RT registry empty after ordinary completion"
    fi
    stop_mock

    # 2. cap-zero: immediate, never synchronous, schedule:cap_reached
    start_mock fast; start_hull "$APP" --workers 1
    T0=$(python3 -c "import time;print(time.time())")
    R_CAP=$(curl -s --max-time 8 "http://127.0.0.1:$PORT_HTTP/send")
    T1=$(python3 -c "import time;print(time.time())")
    DT=$(python3 -c "print(int(($T1-$T0)*1000))")
    case "$R_CAP" in *'"error":"connect_failed"'*) pass "$RT cap-zero connect_failed: $R_CAP" ;;
                     *) fail "$RT cap-zero result (got: $R_CAP)" ;; esac
    [ "$DT" -lt 2000 ] && pass "$RT cap-zero immediate (${DT}ms)" || fail "$RT cap-zero slow (${DT}ms)"
    grep -q '"schedule":"cap_reached"' "$APPDIR/hull.log" \
        && pass "$RT audit schedule:cap_reached" || fail "$RT audit missing schedule:cap_reached"

    # 3. exact audit: 400-char subject on cap_reached
    curl -s --max-time 8 "http://127.0.0.1:$PORT_HTTP/long" >/dev/null
    SLEN=$(subj_len_from_audit)
    [ "$SLEN" = "400" ] && pass "$RT audit subject exact (len $SLEN)" || fail "$RT audit subject len=$SLEN (want 400)"
    kill -INT "$HULL_PID" 2>/dev/null; wait "$HULL_PID" 2>/dev/null; stop_mock

    # 4. TRUE saturation (W=2 -> cap 1): one admitted send is held on the slow
    #    peer (occupying the single slot); a concurrent send overflows and must
    #    resolve promptly as connect_failed / schedule:cap_reached. This is the
    #    observable binding parity for real saturation (cap 1, not the cap-0
    #    degenerate case above), asserted identically for Lua and JS.
    start_mock slow; start_hull "$APP" --workers 2
    # Held send: reserves the one slot and stalls in the greeting read on the slow
    # peer. Backgrounded; its own result is irrelevant (we tear it down after).
    curl -s --max-time 15 "http://127.0.0.1:$PORT_HTTP/send" >/dev/null 2>&1 &
    HELD_CURL=$!
    sleep 1   # let the held send reserve the slot + connect (>> reserve+connect)
    T0=$(python3 -c "import time;print(time.time())")
    R_SAT=$(curl -s --max-time 8 "http://127.0.0.1:$PORT_HTTP/send")
    T1=$(python3 -c "import time;print(time.time())")
    DT_SAT=$(python3 -c "print(int(($T1-$T0)*1000))")
    case "$R_SAT" in *'"error":"connect_failed"'*) pass "$RT saturation overflow connect_failed: $R_SAT" ;;
                     *) fail "$RT saturation overflow (got: $R_SAT)" ;; esac
    [ "$DT_SAT" -lt 2000 ] && pass "$RT saturation overflow prompt (${DT_SAT}ms)" \
                           || fail "$RT saturation overflow slow (${DT_SAT}ms)"
    grep -q '"schedule":"cap_reached"' "$APPDIR/hull.log" \
        && pass "$RT saturation audit schedule:cap_reached" \
        || fail "$RT saturation audit missing schedule:cap_reached"
    kill -INT "$HULL_PID" 2>/dev/null; wait "$HULL_PID" 2>/dev/null
    kill "$HELD_CURL" 2>/dev/null; stop_mock

    # record canonicalised results for cross-runtime equivalence
    { printf '%s' "$R_SEND"   | norm;
      printf '%s' "$R_DENIED" | norm;
      printf '%s' "$R_CAP"    | norm;
      printf '%s' "$R_SAT"    | norm; } >"$APPDIR/res.$RT"
}

# ── db.async under SMTP saturation (per runtime; JS drives last_async_cont) ──
# A generic pool callback proves worker headroom but not DB submission +
# continuation suspend/complete. This leg runs a REAL db.async while an admitted
# SMTP send is parked (W=2 -> cap 1), requires bounded DB completion, confirms the
# overflow SMTP still caps, then (part 1) releases SMTP and proves admission +
# registry return to zero, and (part 2) repeats under shutdown.
db_saturation_leg() { # <rt> <appfile>
    RT="$1"; APP="$2"
    log "--- db.async under saturation: $RT ---"

    # Part 1 - release path: the "brief" peer holds the admitted send parked, then
    # completes it normally so the slot can be released cleanly.
    start_mock brief; start_hull "$APP" --workers 2 -d "$APPDIR/sat_$RT.db"
    curl -s --max-time 15 "http://127.0.0.1:$PORT_HTTP/send" >/dev/null 2>&1 &
    HELD=$!
    sleep 1                                   # admitted send now parked on the peer
    D0=$(python3 -c "import time;print(time.time())")
    R_DB=$(curl -s --max-time 8 "http://127.0.0.1:$PORT_HTTP/db")
    D1=$(python3 -c "import time;print(time.time())"); DT_DB=$(python3 -c "print(int(($D1-$D0)*1000))")
    case "$R_DB" in *'"result":2'*) pass "$RT db.async completes while SMTP saturated: $R_DB" ;;
                    *) fail "$RT db.async under saturation (got: $R_DB)" ;; esac
    [ "$DT_DB" -lt 4000 ] && pass "$RT db.async bounded (${DT_DB}ms)" || fail "$RT db.async slow (${DT_DB}ms)"
    R_OV=$(curl -s --max-time 8 "http://127.0.0.1:$PORT_HTTP/send")
    case "$R_OV" in *'"error":"connect_failed"'*) pass "$RT overflow still cap_reached: $R_OV" ;;
                    *) fail "$RT overflow during db leg (got: $R_OV)" ;; esac
    wait "$HELD" 2>/dev/null                  # release: the held send completes normally
    # Slot reusable after release: a follow-up send is ADMITTED and completes ok
    # (not an immediate cap_reached), proving admission inflight returned to zero.
    R_AFTER=$(curl -s --max-time 10 "http://127.0.0.1:$PORT_HTTP/send")
    case "$R_AFTER" in *'"ok":true'*) pass "$RT admission freed after release (slot reusable)" ;;
                       *) fail "$RT slot not reusable after release (got: $R_AFTER)" ;; esac
    kill -INT "$HULL_PID" 2>/dev/null; wait "$HULL_PID" 2>/dev/null
    if grep -q '"terminal":"cancelled"' "$APPDIR/hull.log"; then
        fail "$RT registry not empty after release (terminal:cancelled present)"
    else
        pass "$RT registry + inflight zero after release"
    fi
    stop_mock

    # Part 2 - during shutdown: the admitted send is still parked (slow peer) and a
    # db.async has just completed; the two-pass shutdown must not hang.
    start_mock slow; start_hull "$APP" --workers 2 -d "$APPDIR/sat2_$RT.db"
    curl -s --max-time 20 "http://127.0.0.1:$PORT_HTTP/send" >/dev/null 2>&1 &
    HELD2=$!
    sleep 1
    R_DB2=$(curl -s --max-time 8 "http://127.0.0.1:$PORT_HTTP/db")
    case "$R_DB2" in *'"result":2'*) pass "$RT db.async ok before shutdown: $R_DB2" ;;
                     *) fail "$RT db.async before shutdown (got: $R_DB2)" ;; esac
    S0=$(python3 -c "import time;print(time.time())")
    kill -INT "$HULL_PID"
    for _ in $(seq 1 200); do kill -0 "$HULL_PID" 2>/dev/null || break; sleep 0.1; done
    S1=$(python3 -c "import time;print(time.time())"); DT_SD=$(python3 -c "print(int($S1-$S0))")
    if kill -0 "$HULL_PID" 2>/dev/null; then fail "$RT shutdown hung (saturated + db)"; kill -9 "$HULL_PID" 2>/dev/null
    elif [ "$DT_SD" -lt 15 ]; then pass "$RT shutdown-while-saturated bounded (~${DT_SD}s)"
    else fail "$RT shutdown-while-saturated slow (~${DT_SD}s)"; fi
    kill "$HELD2" 2>/dev/null; stop_mock
}

run_suite lua "$APPDIR/app.lua"
run_suite js  "$APPDIR/app.js"

# ── Lua/JS equivalence: identical result objects ────────────────────────
log "--- Lua/JS parity ---"
if diff "$APPDIR/res.lua" "$APPDIR/res.js" >/dev/null 2>&1; then
    pass "Lua and JS return byte-identical result objects"
else
    fail "Lua/JS result objects differ:"; diff "$APPDIR/res.lua" "$APPDIR/res.js"
fi

# ── db.async under saturation (both runtimes) ───────────────────────────
db_saturation_leg lua "$APPDIR/app_db.lua"
db_saturation_leg js  "$APPDIR/app_db.js"

# ── prompt cancellation (runtime-agnostic C path; run once via Lua) ─────
log "--- prompt cancellation ---"
start_mock slow; start_hull "$APPDIR/app.lua"
( curl -s --max-time 45 "http://127.0.0.1:$PORT_HTTP/send" >/dev/null 2>&1 & )
sleep 1.5
T0=$(python3 -c "import time;print(time.time())")
kill -INT "$HULL_PID"
for _ in $(seq 1 200); do kill -0 "$HULL_PID" 2>/dev/null || break; sleep 0.1; done
T1=$(python3 -c "import time;print(time.time())"); DT=$(python3 -c "print(int($T1-$T0))")
if kill -0 "$HULL_PID" 2>/dev/null; then fail "shutdown hung with op in flight"; kill -9 "$HULL_PID" 2>/dev/null
elif [ "$DT" -lt 15 ]; then pass "prompt cancellation: shutdown ~${DT}s (<< 30s SMTP timeout)"
else fail "shutdown ~${DT}s (not well below the 30s timeout)"; fi
stop_mock

# ── clean shutdown with an op in flight, under ASan (both runtimes) ─────
DBG="${HULL_DEBUG_BIN:-}"
if [ -n "$DBG" ] && [ -x "$DBG" ]; then
    log "--- clean shutdown under ASan ---"
    for pair in "lua $APPDIR/app.lua" "js $APPDIR/app.js"; do
        set -- $pair; RT="$1"; APP="$2"
        start_mock slow
        "$DBG" "$APP" -p "$PORT_HTTP" --audit >"$APPDIR/asan.log" 2>&1 &
        HULL_PID=$!; PIDS="$PIDS $HULL_PID"
        for _ in $(seq 1 120); do curl -s "http://127.0.0.1:$PORT_HTTP/" 2>/dev/null | grep -q ready && break; sleep 0.2; done
        ( curl -s --max-time 45 "http://127.0.0.1:$PORT_HTTP/send" >/dev/null 2>&1 & )
        sleep 1.5; kill -INT "$HULL_PID"
        for _ in $(seq 1 200); do kill -0 "$HULL_PID" 2>/dev/null || break; sleep 0.1; done
        kill -9 "$HULL_PID" 2>/dev/null
        if grep -qiE "runtime error|AddressSanitizer|use-after-free|double-free|SUMMARY: .*Sanitizer" "$APPDIR/asan.log"; then
            fail "$RT ASan error on shutdown-with-op-in-flight"; grep -iE "Sanitizer|use-after" "$APPDIR/asan.log" | head
        else
            pass "$RT clean shutdown under ASan (op in flight)"
        fi
        stop_mock
    done
else
    log "  skip clean-shutdown-under-ASan (set HULL_DEBUG_BIN=./build/hull from 'make debug')"
fi

log ""
if [ "$FAILS" -eq 0 ]; then log "e2e_smtp: PASS"; exit 0
else log "e2e_smtp: $FAILS FAILED"; exit 1; fi
