#!/bin/sh
# e2e_smtp.sh - model-2 async SMTP (Lua binding) end to end against a mock peer.
#
# Proves the checkpoint-1 behaviors on a live server (the keel async backend, i.e.
# the default build): ordinary async completion + a registry left empty; prompt
# cancellation after resolution (a stalled peer shuts down well below the SMTP
# timeout); cap-zero resolving immediately and never blocking; exact (untruncated)
# scheduling-failure audit metadata; and a clean shutdown with an op in flight
# (re-run under the ASan debug build). The done_fn drain-vs-drop backend parity
# (keel drains, poll drops, both release once) is unit-covered by test_smtp_async.
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

if ! command -v python3 >/dev/null 2>&1; then
    log "e2e_smtp: python3 not found - skipping"; exit 0
fi
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
        time.sleep(30)                      # post-resolution stall (never greets in time)
        try: c.sendall(b"220 slow\r\n")
        except Exception: pass
        c.close(); return
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

start_mock() { # mode
    python3 "$APPDIR/smtp_mock_$$.py" "$PORT_SMTP" "$1" 2>"$APPDIR/mock.err" &
    MOCK_PID=$!; PIDS="$PIDS $MOCK_PID"
    # wait for "ready"
    for _ in $(seq 1 50); do grep -q ready "$APPDIR/mock.err" 2>/dev/null && return 0; sleep 0.1; done
}
stop_mock() { kill "$MOCK_PID" 2>/dev/null; }

# ── the app (a long subject exercises the exact-audit path) ─────────────
LONGSUBJ=$(python3 -c "print('S'*400)")
cat > "$APPDIR/app.lua" <<LUA
local smtp = require("hull.smtp")
app.manifest({ modules = { "hull/smtp@1", "hull/http-server@1" }, hosts = { "127.0.0.1" } })
app.get("/send", function(req, res)
    res:json(smtp.send({ host="127.0.0.1", port=$PORT_SMTP, from="a@x", to="b@y",
                         subject="hi", body="hello" }))
end)
app.get("/long", function(req, res)
    res:json(smtp.send({ host="127.0.0.1", port=$PORT_SMTP, from="a@x", to="b@y",
                         subject="$LONGSUBJ", body="b" }))
end)
LUA

start_hull() { # extra-args... ; writes audit+log to $APPDIR/hull.log
    "$HULL" "$APPDIR/app.lua" -p "$PORT_HTTP" --audit "$@" >"$APPDIR/hull.log" 2>&1 &
    HULL_PID=$!; PIDS="$PIDS $HULL_PID"
    for _ in $(seq 1 60); do
        curl -s -o /dev/null "http://127.0.0.1:$PORT_HTTP/" 2>/dev/null && break
        kill -0 "$HULL_PID" 2>/dev/null || return 1
        sleep 0.2
    done
}

# ════════════════════════════════════════════════════════════════════════
# 1. Ordinary async completion + registry empty after completion.
# ════════════════════════════════════════════════════════════════════════
start_mock fast; start_hull
OUT=$(curl -s --max-time 10 "http://127.0.0.1:$PORT_HTTP/send")
case "$OUT" in *'"ok":true'*) pass "async success yields+resumes: $OUT" ;;
                *) fail "async success (got: $OUT)" ;; esac
# graceful shutdown; the completed op must NOT be swept (registry was empty), so
# NO terminal:cancelled audit appears.
kill -INT "$HULL_PID"; wait "$HULL_PID" 2>/dev/null
if grep -q '"terminal":"cancelled"' "$APPDIR/hull.log"; then
    fail "registry not empty after completion (unexpected cancelled audit)"
else
    pass "registry empty after ordinary completion (no cancelled audit at shutdown)"
fi
# exactly one completion audit for the send
N=$(grep -c '"cap":"smtp.send"' "$APPDIR/hull.log")
[ "$N" = "1" ] && pass "exactly one audit event ($N)" || fail "audit count = $N (want 1)"
stop_mock

# ════════════════════════════════════════════════════════════════════════
# 2. cap-zero (--workers 1 => admission cap 0): immediate, never synchronous.
# ════════════════════════════════════════════════════════════════════════
start_mock fast; start_hull --workers 1
T0=$(python3 -c "import time;print(time.time())")
OUT=$(curl -s --max-time 8 "http://127.0.0.1:$PORT_HTTP/send")
T1=$(python3 -c "import time;print(time.time())")
DT=$(python3 -c "print(int(($T1-$T0)*1000))")
case "$OUT" in *'"error":"connect_failed"'*) pass "cap-zero resolves connect_failed: $OUT" ;;
                *) fail "cap-zero result (got: $OUT)" ;; esac
[ "$DT" -lt 2000 ] && pass "cap-zero immediate (${DT}ms, never blocks)" || fail "cap-zero slow (${DT}ms)"
if grep -q '"schedule":"cap_reached"' "$APPDIR/hull.log"; then
    pass "cap-zero audit schedule:cap_reached"
else
    fail "cap-zero audit missing schedule:cap_reached"
fi
kill -INT "$HULL_PID"; wait "$HULL_PID" 2>/dev/null; stop_mock

# ════════════════════════════════════════════════════════════════════════
# 3. Exact (untruncated) audit metadata: 400-char subject on cap_reached.
# ════════════════════════════════════════════════════════════════════════
start_mock fast; start_hull --workers 1
curl -s --max-time 8 "http://127.0.0.1:$PORT_HTTP/long" >/dev/null
SLEN=$(python3 - "$APPDIR/hull.log" <<'PY'
import json,sys
for line in open(sys.argv[1]):
    if '"cap":"smtp.send"' in line and "cap_reached" in line:
        print(len(json.loads(line[line.index("{"):])["subject"])); break
PY
)
[ "$SLEN" = "400" ] && pass "audit subject preserved exactly (len $SLEN)" || fail "audit subject len=$SLEN (want 400, truncation?)"
kill -INT "$HULL_PID"; wait "$HULL_PID" 2>/dev/null; stop_mock

# ════════════════════════════════════════════════════════════════════════
# 4. Prompt cancellation after resolution: a stalled peer + SIGINT shuts down
#    well below the 30s SMTP timeout.
# ════════════════════════════════════════════════════════════════════════
start_mock slow; start_hull
( curl -s --max-time 45 "http://127.0.0.1:$PORT_HTTP/send" >/dev/null 2>&1 & )
sleep 1.5   # the op is now parked reading the (30s-stalled) greeting
T0=$(python3 -c "import time;print(time.time())")
kill -INT "$HULL_PID"
# bounded wait for exit
for _ in $(seq 1 200); do kill -0 "$HULL_PID" 2>/dev/null || break; sleep 0.1; done
T1=$(python3 -c "import time;print(time.time())")
DT=$(python3 -c "print(int($T1-$T0))")
if kill -0 "$HULL_PID" 2>/dev/null; then
    fail "shutdown hung with op in flight (>20s)"; kill -9 "$HULL_PID" 2>/dev/null
elif [ "$DT" -lt 15 ]; then
    pass "prompt cancellation: shutdown in ~${DT}s (<< 30s SMTP timeout)"
else
    fail "shutdown ~${DT}s (not well below the 30s timeout)"
fi
stop_mock

# ════════════════════════════════════════════════════════════════════════
# 5. Clean shutdown with an op in flight, under ASan (if a debug hull exists).
# ════════════════════════════════════════════════════════════════════════
DBG="${HULL_DEBUG_BIN:-}"
if [ -n "$DBG" ] && [ -x "$DBG" ]; then
    start_mock slow
    "$DBG" "$APPDIR/app.lua" -p "$PORT_HTTP" --audit >"$APPDIR/asan.log" 2>&1 &
    HULL_PID=$!; PIDS="$PIDS $HULL_PID"
    for _ in $(seq 1 80); do curl -s -o /dev/null "http://127.0.0.1:$PORT_HTTP/" 2>/dev/null && break; sleep 0.2; done
    ( curl -s --max-time 45 "http://127.0.0.1:$PORT_HTTP/send" >/dev/null 2>&1 & )
    sleep 1.5
    kill -INT "$HULL_PID"
    for _ in $(seq 1 200); do kill -0 "$HULL_PID" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$HULL_PID" 2>/dev/null
    if grep -qiE "runtime error|AddressSanitizer|heap-use-after-free|double-free|LeakSanitizer|SUMMARY: .*Sanitizer" "$APPDIR/asan.log"; then
        fail "ASan reported an error on shutdown-with-op-in-flight"
        grep -iE "Sanitizer|use-after|double-free" "$APPDIR/asan.log" | head
    else
        pass "clean shutdown under ASan (op in flight)"
    fi
    stop_mock
else
    log "  skip clean-shutdown-under-ASan (set HULL_DEBUG_BIN=./build/hull from 'make debug')"
fi

# ════════════════════════════════════════════════════════════════════════
log ""
if [ "$FAILS" -eq 0 ]; then log "e2e_smtp: PASS"; exit 0
else log "e2e_smtp: $FAILS FAILED"; exit 1; fi
