#!/bin/sh
# E2E: hull/ssh behaves the same in both runtimes.
#
# Every scenario runs once as a Lua app and once as its JS twin, against the
# same peers, and asserts the same outcome and the same code:
#
#   denied       a host outside ssh.connect.hosts           -> denied
#   timeout      a peer that accepts and never speaks        -> timeout
#   cap          33 connections at once (32 live streams)    -> one too_many_streams
#   txn          a connect inside a database transaction     -> wait_refused
#   cancel       an HTTP request parked in ssh.connect when the
#                server shuts down (Keel cancels the op)     -> the stream is closed
#   teardown     SIGTERM while app.main is parked on a stream -> exits, socket closed
#   live         (with sshd) host_changed for a key that is not the
#                stored one, then host_unknown, accept, exec -> ok
#
# The JS client is a port of the Lua one over the same capability layer
# (src/hull/runtime/js/mod_ssh.c next to runtime/lua/mod_ssh.c); this is what
# holds the two to one behaviour from the outside.
#
# Usage: sh tests/e2e_ssh_parity.sh      (HULL_E2E_REQUIRE_SSHD=1: no sshd fails)
# Requires: build/hull, python3, ssh-keygen, curl. The live leg needs an sshd.
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS=0
FAIL=0
WORK=""
PIDS=""

if [ ! -x "$HULL" ]; then echo "e2e_ssh_parity: build/hull missing - run make"; exit 1; fi
for t in python3 ssh-keygen curl; do
    command -v "$t" >/dev/null 2>&1 || { echo "e2e_ssh_parity: $t required"; exit 1; }
done

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1${2:+ - got: $2}"; FAIL=$((FAIL + 1)); }

cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null || true; done
    [ -n "$WORK" ] && rm -rf "$WORK"
}
trap cleanup EXIT INT TERM
WORK=$(mktemp -d 2>/dev/null || mktemp -d -t hullsshp)

free_port() {
    python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'
}

# KEY=VALUE from an app's output (a print / console.log is an [app] record).
line_of() {
    printf '%s\n' "$1" | sed -n "s/^\\(.*\\[app\\] \\)\\{0,1\\}$2=\\(.*\\)$/\\2/p" | head -1
}
assert_line() {
    _got=$(line_of "$1" "$2")
    if [ "$_got" = "$3" ]; then pass "$4"; else fail "$4" "$2=${_got:-<missing>} (wanted $3)"; fi
}

# run with a bound: no timeout(1) on macOS
run_bounded() {
    _secs=$1; shift
    ( "$@" ) > "$WORK/run.out" 2>&1 &
    _p=$!
    ( sleep "$_secs"; kill -9 "$_p" 2>/dev/null ) >/dev/null 2>&1 &
    _w=$!
    wait "$_p" 2>/dev/null
    RUN_RC=$?
    kill "$_w" 2>/dev/null || true
    wait "$_w" 2>/dev/null || true
    RUN_OUT=$(cat "$WORK/run.out")
}

ssh-keygen -q -t ed25519 -N '' -f "$WORK/client_key" </dev/null
chmod 600 "$WORK/client_key"
SSH_LOGIN="${USER:-$(id -un 2>/dev/null || echo tester)}"

# ── a peer that accepts and never speaks, logging accepts and closes ─────
cat > "$WORK/silent.py" <<'PY'
import socket, sys, threading, time
port, log, ready = int(sys.argv[1]), sys.argv[2], sys.argv[3]
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', port))
srv.listen(128)
lock = threading.Lock()
def note(what):
    with lock:
        with open(log, 'a') as f:
            f.write('%s %.3f\n' % (what, time.time()))
def hold(c):
    try:
        while c.recv(4096):
            pass
    except OSError:
        pass
    note('close')
    c.close()
open(ready, 'w').write('x')
while True:
    c, _ = srv.accept()
    note('accept')
    threading.Thread(target=hold, args=(c,), daemon=True).start()
PY
SILENT_PORT=$(free_port)
python3 "$WORK/silent.py" "$SILENT_PORT" "$WORK/silent.log" "$WORK/silent.ready" &
PIDS="$PIDS $!"
for _ in $(seq 1 50); do [ -f "$WORK/silent.ready" ] && break; sleep 0.1; done
count_events() { _n=$(grep -c "^$1 " "$WORK/silent.log" 2>/dev/null); echo "${_n:-0}"; }

# ── sshd, when there is one ──────────────────────────────────────────────
SSH_PORT=0
SSHD=""
for cand in /usr/sbin/sshd /usr/local/sbin/sshd /opt/homebrew/sbin/sshd; do
    [ -x "$cand" ] && SSHD="$cand" && break
done
if [ -n "$SSHD" ]; then
    SSH_PORT=$(free_port)
    mkdir -p "$WORK/sshd"
    ssh-keygen -q -t ed25519 -N '' -f "$WORK/sshd/host_key" </dev/null
    cp "$WORK/client_key.pub" "$WORK/sshd/authorized_keys"
    chmod 600 "$WORK/sshd/host_key" "$WORK/sshd/authorized_keys"
    cat > "$WORK/sshd/sshd_config" <<CFG
Port $SSH_PORT
ListenAddress 127.0.0.1
HostKey $WORK/sshd/host_key
AuthorizedKeysFile $WORK/sshd/authorized_keys
StrictModes no
UsePAM no
PasswordAuthentication no
KbdInteractiveAuthentication no
PubkeyAuthentication yes
PidFile $WORK/sshd/pid
CFG
    "$SSHD" -f "$WORK/sshd/sshd_config" -D -e >"$WORK/sshd/log" 2>&1 &
    PIDS="$PIDS $!"
    _ok=0
    for _ in $(seq 1 50); do
        if python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(0.2)
sys.exit(0 if s.connect_ex(('127.0.0.1', $SSH_PORT)) == 0 else 1)" 2>/dev/null; then _ok=1; break; fi
        sleep 0.1
    done
    [ "$_ok" -eq 1 ] || SSH_PORT=0
fi

# ── the apps: one per runtime, the scenario chosen by its first argument ─
write_lua() {
    mkdir -p "$1"; cp "$WORK/client_key" "$1/client_key"
    cat > "$1/app.lua" <<LUA
app.manifest({
    modules = { "hull/ssh@1", "hull/fs@1", "hull/db@1", "hull/http-server@1" },
    fs = { read = { "client_key" } },
    ssh = { connect = { hosts = { "127.0.0.1" }, ports = { $SILENT_PORT, $SSH_PORT },
                        users = { "$SSH_LOGIN" } } },
})
local ssh = require("hull.ssh")
local fs  = require("hull.fs")
local function opts(port, t)
    return { host = "127.0.0.1", port = port, user = "$SSH_LOGIN",
             key = fs.read("client_key"), timeout_ms = t, trust = TRUST }
end
-- A refusal is raised as "[hull:<code>] ..."; anything else is (nil, err).
local function code_of(e)
    if type(e) == "string" then return e:match("^%[hull:([%w_]+)%]") or ("raised:" .. e) end
    return e and e.code
end
local function try(o)
    local ok, c, e = pcall(ssh.connect, o)
    if not ok then return nil, code_of(c) end
    if not c then return nil, code_of(e) end
    return c
end

app.main(function(ctx)
    local what = ctx.args[1]
    TRUST = ssh.memory_store()
    if what == "denied" then
        local o = opts($SILENT_PORT, 2000); o.host = "10.9.9.9"
        print("code=" .. tostring(select(2, try(o))))
    elseif what == "timeout" then
        print("code=" .. tostring(select(2, try(opts($SILENT_PORT, 500)))))
    elseif what == "cap" then
        local list = {}
        for i = 1, 33 do list[i] = i end
        local got = hull.map(list, function()
            return tostring(select(2, try(opts($SILENT_PORT, 3000))))
        end, { limit = 40 })
        local n = {}
        for _, c in ipairs(got) do n[c] = (n[c] or 0) + 1 end
        print("refused=" .. tostring(n.too_many_streams or 0))
        print("timeouts=" .. tostring(n.timeout or 0))
    elseif what == "txn" then
        local db = require("hull.db").default()
        db.exec("BEGIN")
        print("code=" .. tostring(select(2, try(opts($SILENT_PORT, 2000)))))
        db.exec("ROLLBACK")
    elseif what == "teardown" then
        print("parking=yes")
        print("code=" .. tostring(select(2, try(opts($SILENT_PORT, 60000)))))
    elseif what == "live" then
        -- A stored key that is not the server's: what a MITM looks like.
        local bogus = string.pack(">s4s4", "ssh-ed25519", string.rep("x", 32))
        ssh.accept_host(TRUST, "127.0.0.1", bogus, $SSH_PORT)
        local c, e = ssh.connect(opts($SSH_PORT, 10000))
        print("changed=" .. tostring(e and e.code))
        print("stored_fp=" .. tostring(e and e.stored_fingerprint ~= nil))
        ssh.forget_host(TRUST, "127.0.0.1", $SSH_PORT)
        c, e = ssh.connect(opts($SSH_PORT, 10000))
        print("unknown=" .. tostring(e and e.code))
        ssh.accept_host(TRUST, "127.0.0.1", e.key_blob, $SSH_PORT)
        c, e = ssh.connect(opts($SSH_PORT, 10000))
        local r = c and c:exec("echo parity-ok")
        print("exec=" .. tostring(r and (r.stdout:gsub("%s+$", "")) or (e and e.code)))
        if c then c:close() end
    end
    -- A scenario ends the process (non-zero skips the serve loop); with no
    -- argument the app serves /hang for the cancel leg.
    return what and 3 or 0
end)

app.get("/ping", function(req, res) res:text("pong") end)
app.get("/hang", function(req, res)
    TRUST = TRUST or ssh.memory_store()
    local _, code = try(opts($SILENT_PORT, 15000))
    res:text(tostring(code))
end)
LUA
}

write_js() {
    mkdir -p "$1"; cp "$WORK/client_key" "$1/client_key"
    cat > "$1/app.js" <<JS
import { app } from "hull:app";
import { ssh } from "hull:ssh";
import { fs } from "hull:fs";
import { db as dbm } from "hull:db";

app.manifest({
    modules: ["hull/ssh@1", "hull/fs@1", "hull/db@1", "hull/http-server@1"],
    fs: { read: ["client_key"] },
    ssh: { connect: { hosts: ["127.0.0.1"], ports: [$SILENT_PORT, $SSH_PORT],
                      users: ["$SSH_LOGIN"] } },
});
let TRUST = ssh.memoryStore();
const opts = (port, t) => ({ host: "127.0.0.1", port, user: "$SSH_LOGIN",
                             key: fs.read("client_key"), timeoutMs: t, trust: TRUST });
// A failure is a rejection carrying .code (a runtime refusal included).
async function code(o) {
    try { await ssh.connect(o); return "connected"; } catch (e) { return String(e && e.code); }
}

app.main(async (ctx) => {
    const what = ctx.args[0];
    if (what === "denied") {
        const o = opts($SILENT_PORT, 2000); o.host = "10.9.9.9";
        console.log("code=" + await code(o));
    } else if (what === "timeout") {
        console.log("code=" + await code(opts($SILENT_PORT, 500)));
    } else if (what === "cap") {
        const list = Array.from({ length: 33 }, (_, i) => i);
        const got = await hull.map(list, () => code(opts($SILENT_PORT, 3000)), { limit: 40 });
        const n = {};
        for (const c of got) n[c] = (n[c] || 0) + 1;
        console.log("refused=" + (n.too_many_streams || 0));
        console.log("timeouts=" + (n.timeout || 0));
    } else if (what === "txn") {
        const db = dbm.default();
        db.exec("BEGIN");
        console.log("code=" + await code(opts($SILENT_PORT, 2000)));
        db.exec("ROLLBACK");
    } else if (what === "teardown") {
        console.log("parking=yes");
        console.log("code=" + await code(opts($SILENT_PORT, 60000)));
    } else if (what === "live") {
        const u32 = (n) => String.fromCharCode(n >>> 24, (n >>> 16) & 255, (n >>> 8) & 255, n & 255);
        const bogus = u32(11) + "ssh-ed25519" + u32(32) + "x".repeat(32);
        await ssh.acceptHost(TRUST, "127.0.0.1", bogus, $SSH_PORT);
        let e = null;
        try { await ssh.connect(opts($SSH_PORT, 10000)); } catch (x) { e = x; }
        console.log("changed=" + String(e && e.code));
        console.log("stored_fp=" + String(!!(e && e.storedFingerprint)));
        await ssh.forgetHost(TRUST, "127.0.0.1", $SSH_PORT);
        e = null;
        try { await ssh.connect(opts($SSH_PORT, 10000)); } catch (x) { e = x; }
        console.log("unknown=" + String(e && e.code));
        await ssh.acceptHost(TRUST, "127.0.0.1", e.keyBlob, $SSH_PORT);
        const c = await ssh.connect(opts($SSH_PORT, 10000));
        const r = await c.exec("echo parity-ok");
        console.log("exec=" + r.stdout.replace(/\s+$/, ""));
        c.close();
    }
    // A scenario ends the process (non-zero skips the serve loop); with no
    // argument the app serves /hang for the cancel leg.
    return what ? 3 : 0;
});

app.get("/ping", (req, res) => res.text("pong"));
app.get("/hang", async (req, res) => {
    res.text(await code(opts($SILENT_PORT, 15000)));
});
JS
}

for rt in lua js; do
    echo
    echo "e2e_ssh_parity: $rt"
    APPDIR="$WORK/app_$rt"
    if [ "$rt" = lua ]; then write_lua "$APPDIR"; else write_js "$APPDIR"; fi
    run_main() { cd "$APPDIR" && exec "$HULL" --no-sandbox "app.$rt" -- "$@"; }

    run_bounded 20 run_main denied
    assert_line "$RUN_OUT" code denied "[$rt] a host outside the grant is denied"

    run_bounded 20 run_main timeout
    assert_line "$RUN_OUT" code timeout "[$rt] a peer that never speaks is a timeout"

    run_bounded 30 run_main cap
    assert_line "$RUN_OUT" refused 1 "[$rt] the 33rd live stream is too_many_streams"
    assert_line "$RUN_OUT" timeouts 32 "[$rt] the other 32 reach the peer and time out"

    run_bounded 20 run_main txn
    assert_line "$RUN_OUT" code wait_refused "[$rt] a connect inside a transaction is wait_refused"

    # teardown: SIGTERM while app.main is parked on a live stream
    : > "$WORK/silent.log"
    (cd "$APPDIR" && exec "$HULL" --no-sandbox "app.$rt" -- teardown) > "$WORK/td.out" 2>&1 &
    TD=$!
    for _ in $(seq 1 50); do [ "$(count_events accept)" -ge 1 ] && break; sleep 0.1; done
    sleep 0.3
    kill -TERM "$TD" 2>/dev/null
    ( sleep 10; kill -9 "$TD" 2>/dev/null ) >/dev/null 2>&1 &
    TW=$!
    wait "$TD" 2>/dev/null; TD_RC=$?
    kill "$TW" 2>/dev/null || true; wait "$TW" 2>/dev/null || true
    if [ "$TD_RC" -ne 137 ] && [ "$TD_RC" -lt 128 -o "$TD_RC" -eq 143 ]; then
        pass "[$rt] SIGTERM with a parked stream exits (rc $TD_RC)"
    else
        fail "[$rt] SIGTERM with a parked stream exits" "rc $TD_RC: $(tail -3 "$WORK/td.out")"
    fi
    for _ in $(seq 1 30); do [ "$(count_events close)" -ge 1 ] && break; sleep 0.1; done
    if [ "$(count_events close)" -ge 1 ]; then pass "[$rt] the stream's socket is closed at teardown"
    else fail "[$rt] the stream's socket is closed at teardown" "$(cat "$WORK/silent.log")"; fi
    if grep -qiE "AddressSanitizer|runtime error:|Assertion|Segmentation" "$WORK/td.out"; then
        fail "[$rt] teardown is clean" "$(grep -iE 'Sanitizer|runtime error|Assertion|Segmentation' "$WORK/td.out" | head -2)"
    else pass "[$rt] teardown is clean"; fi

    # cancel: a request parked in ssh.connect when the server shuts down. Keel
    # cancels the suspended op (the stream's on_cancel), which closes the
    # stream: the peer sees its socket close well before the 15 s connect
    # budget. (A client that merely goes away does not cancel a suspended
    # request in Keel, in either runtime; the connect then ends at its own
    # deadline.)
    HPORT=$(free_port)
    (cd "$APPDIR" && exec "$HULL" --no-sandbox --drain-timeout 100 -p "$HPORT" "app.$rt") \
        > "$WORK/srv_$rt.log" 2>&1 &
    SRV=$!
    PIDS="$PIDS $SRV"
    for _ in $(seq 1 40); do
        curl -s -o /dev/null -m 1 "http://127.0.0.1:$HPORT/ping" && break; sleep 0.25
    done
    : > "$WORK/silent.log"
    curl -s -o /dev/null -m 20 "http://127.0.0.1:$HPORT/hang" >/dev/null 2>&1 &
    CURL_PID=$!
    PIDS="$PIDS $CURL_PID"
    for _ in $(seq 1 50); do [ "$(count_events accept)" -ge 1 ] && break; sleep 0.1; done
    sleep 0.3
    T0=$(date +%s)
    kill -TERM "$SRV" 2>/dev/null
    ( sleep 10; kill -9 "$SRV" 2>/dev/null ) >/dev/null 2>&1 &
    SW=$!
    wait "$SRV" 2>/dev/null; SRV_RC=$?
    kill "$SW" 2>/dev/null || true; wait "$SW" 2>/dev/null || true
    for _ in $(seq 1 50); do [ "$(count_events close)" -ge 1 ] && break; sleep 0.1; done
    T1=$(date +%s)
    if [ "$(count_events accept)" -ge 1 ] && [ "$(count_events close)" -ge 1 ] && [ $((T1 - T0)) -lt 8 ]; then
        pass "[$rt] a cancelled request closes its stream (in $((T1 - T0))s, not at the 15s budget)"
    else
        fail "[$rt] a cancelled request closes its stream" "$(cat "$WORK/silent.log")"
    fi
    if [ "$SRV_RC" -eq 0 ] || [ "$SRV_RC" -eq 143 ]; then
        pass "[$rt] the server shuts down with the parked request (rc $SRV_RC)"
    else fail "[$rt] the server shuts down with the parked request" "rc $SRV_RC"; fi
    kill "$CURL_PID" 2>/dev/null || true
    if grep -qiE "AddressSanitizer|runtime error:|Assertion|Segmentation" "$WORK/srv_$rt.log"; then
        fail "[$rt] the cancel is clean" "$(grep -iE 'Sanitizer|runtime error|Assertion|Segmentation' "$WORK/srv_$rt.log" | head -2)"
    else pass "[$rt] the cancel is clean"; fi

    if [ "$SSH_PORT" -ne 0 ]; then
        run_bounded 40 run_main live
        assert_line "$RUN_OUT" changed host_changed "[$rt] a key other than the stored one is host_changed"
        assert_line "$RUN_OUT" stored_fp true "[$rt] host_changed carries the stored fingerprint"
        assert_line "$RUN_OUT" unknown host_unknown "[$rt] forgotten, the host is unknown again"
        assert_line "$RUN_OUT" exec parity-ok "[$rt] accepted, the command runs"
    elif [ "${HULL_E2E_REQUIRE_SSHD:-0}" = "1" ]; then
        fail "[$rt] live: sshd was required but did not start"
    else
        echo "  SKIP: [$rt] live leg (no sshd)"
    fi
done

echo
echo "e2e_ssh_parity: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
