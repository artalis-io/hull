#!/bin/sh
# E2E for hull/ssh over a WebSocket tunnel.
#
# The unit suites drive ssh.connect against a FAKE relay, which proves the
# composition and the exact upgrade bytes but never opens a socket. This puts
# real sockets, real RFC 6455 framing and a real SSH server underneath it.
#
# Two parts, because one of them needs a peer not every host has:
#
#   THE SEAM (always runs). A Python WebSocket-to-TCP relay
#   (tests/fixtures/ws_tcp_shim.py) in front of a trivial TCP listener.
#   Checks that the upgrade happens over a real socket, the caller's headers
#   arrive verbatim, an Access-style 403 is reported as upgrade_refused
#   rather than a manifest denial, and BOTH manifest grants bite.
#
#   THE LIVE SESSION (runs when sshd is available). The same relay in front
#   of an actual OpenSSH sshd: full curve25519 / ed25519 / aes256-gcm
#   handshake, publickey auth, exec, exit status - every byte through the
#   tunnel.
#
# The live session is what proves the feature. The seam checks are what still
# run on a host without sshd, and they are not a consolation prize: the
# tunnel seam is where the bugs are, and they test it directly.
#
# The tunnel is PLAINTEXT WebSocket here (`tls = false`). The TLS leg is
# covered by test_net_stream's 34 cases, including live handshakes against
# badssl.com. It is not covered here because an app.main app resolves its
# trust anchor from the embedded Mozilla bundle only - serve_cli.c honours
# neither --ca-bundle nor --no-ca-bundle - so a self-signed test cert has no
# way to be trusted. Reaching a PUBLIC-CA relay (which is what Cloudflare is)
# works; a private-CA relay from a CLI app does not yet. Recorded in
# docs/ssh_module_design.md section 11b.
#
# Usage: sh tests/e2e_ssh_tunnel.sh
#        HULL_E2E_REQUIRE_SSHD=1 sh tests/e2e_ssh_tunnel.sh
#
# The live session SKIPS when sshd is absent, which is right on a developer
# laptop and wrong in CI: a job that installs openssh-server and then quietly
# tests nothing is worse than a red one. HULL_E2E_REQUIRE_SSHD=1 turns that
# skip into a failure, and ci.yml sets it. Same reasoning as the
# EMBED_PLATFORM check in e2e-htmx-playwright-build.
#
# Requires: build/hull, python3, ssh-keygen. The live session also needs an
# sshd. ssh-keygen is required throughout rather than only for the live half
# because ssh.connect parses the private key BEFORE it dials - correctly, a
# bad key should not cost a connection - so even the seam checks need a real
# one. It ships with openssh-client, which the live half's sshd does not.
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
SHIM="$SRCDIR/tests/fixtures/ws_tcp_shim.py"
PASS=0
FAIL=0
WORK=""
SHIM_PID=""
PEER_PID=""
SSHD_PID=""

if [ ! -x "$HULL" ]; then
    echo "e2e_ssh_tunnel: hull binary not found at $HULL - run 'make' first"
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "e2e_ssh_tunnel: python3 required"
    exit 1
fi
if ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "e2e_ssh_tunnel: ssh-keygen required (openssh-client)"
    exit 1
fi

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1${2:+ - got: $2}"; FAIL=$((FAIL + 1)); }

cleanup() {
    [ -n "$SHIM_PID" ] && kill "$SHIM_PID" 2>/dev/null || true
    [ -n "$PEER_PID" ] && kill "$PEER_PID" 2>/dev/null || true
    [ -n "$SSHD_PID" ] && kill "$SSHD_PID" 2>/dev/null || true
    [ -n "$WORK" ] && rm -rf "$WORK" || true
}
trap cleanup EXIT INT TERM

WORK=$(mktemp -d 2>/dev/null || mktemp -d -t hullssh)

# ONE ed25519 client key for the whole run. ssh.connect parses the key before
# it dials, so every app dir needs a real one - a placeholder fails at
# privatekey.load and never reaches the tunnel the test is about.
ssh-keygen -q -t ed25519 -N '' -f "$WORK/client_key" </dev/null
chmod 600 "$WORK/client_key"

# Assert on a KEY=VALUE line the app printed. Unique variable names: POSIX sh
# has no locals, so a name reused here would clobber the caller's.
assert_line() {
    a_out="$1"; a_key="$2"; a_want="$3"; a_lbl="$4"
    a_got=$(printf '%s\n' "$a_out" | sed -n "s/^${a_key}=\\(.*\\)$/\\1/p" | head -1)
    if [ "$a_got" = "$a_want" ]; then pass "$a_lbl"
    else fail "$a_lbl" "${a_key}=${a_got:-<missing>} (wanted $a_want)"
    fi
}

assert_contains() {
    case "$2" in
        *"$3"*) pass "$1" ;;
        *)      fail "$1" "$(printf '%s' "$2" | head -c 160)" ;;
    esac
}

# A free TCP port. Asked of the kernel rather than guessed, so parallel CI
# jobs on one runner do not collide.
free_port() {
    python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'
}

wait_for_file() {
    _i=0
    while [ ! -f "$1" ]; do
        _i=$((_i + 1))
        [ "$_i" -gt 100 ] && return 1
        sleep 0.1 2>/dev/null || sleep 1
    done
    return 0
}

start_shim() {
    # $1 = listen port, $2 = target port, rest = extra args
    s_listen="$1"; s_target="$2"; shift 2
    rm -f "$WORK/shim-ready" "$WORK/headers.txt"
    python3 "$SHIM" --listen "$s_listen" \
        --target-host 127.0.0.1 --target-port "$s_target" \
        --headers-out "$WORK/headers.txt" \
        --ready-file "$WORK/shim-ready" "$@" 2>"$WORK/shim.log" &
    SHIM_PID=$!
    wait_for_file "$WORK/shim-ready" || { echo "shim did not start"; cat "$WORK/shim.log"; exit 1; }
}

stop_shim() {
    [ -n "$SHIM_PID" ] && kill "$SHIM_PID" 2>/dev/null || true
    SHIM_PID=""
}

# Write app.lua. Ports must be literals: the manifest takes integers, and a
# "$VAR" env ref is only allowed for hosts. So the app is generated per run
# with the ports the kernel just handed us - which also lets the deny cases
# use a deliberately wrong grant.
#
# $1 app dir  $2 ssh host  $3 ssh port  $4 relay host  $5 relay port
# $6 grant-hosts  $7 grant-ports  $8 tunnel-hosts  $9 tunnel-ports
write_app() {
    mkdir -p "$1"
    cp "$WORK/client_key" "$1/client_key"
    chmod 600 "$1/client_key"
    cat > "$1/app.lua" <<LUA
app.manifest({
    modules = { "hull/ssh@1", "hull/fs@1" },
    fs = { read = { "client_key" } },
    ssh = {
        connect = { hosts = { $6 }, ports = { $7 }, users = { "$SSH_LOGIN" } },
        tunnel  = { hosts = { $8 }, ports = { $9 } },
    },
})

local ssh = require("hull.ssh")
local fs  = require("hull.fs")

local TUNNEL = {
    host = "$4",
    port = $5,
    tls  = false,             -- see the header of tests/e2e_ssh_tunnel.sh
    headers = {
        "Cf-Access-Client-Id: e2e.access",
        "Cf-Access-Client-Secret: e2e-secret",
        "Cf-Access-Jump-Destination: $2:$3",
    },
}

app.main(function()
    local key = fs.read("client_key")
    local trust = ssh.memory_store()

    -- First connect: the host key is unknown, and hull/ssh refuses rather
    -- than deciding for us. The reason carries the blob, so accepting it is
    -- an explicit act and the reconnect is a second real tunnel.
    local conn, err = ssh.connect{
        host = "$2", port = $3, user = "$SSH_LOGIN",
        key = key, trust = trust, tunnel = TUNNEL,
    }
    if not conn then
        print("first_code=" .. tostring(err.code))
        if err.code ~= "host_unknown" then
            print("detail=" .. tostring(err.detail or err.status))
            return 1
        end
        print("fingerprint_shape=" .. tostring(err.fingerprint:match("^SHA256:") ~= nil))
        ssh.accept_host(trust, "$2", err.key_blob)
        conn, err = ssh.connect{
            host = "$2", port = $3, user = "$SSH_LOGIN",
            key = key, trust = trust, tunnel = TUNNEL,
        }
    end
    if not conn then
        print("second_code=" .. tostring(err.code))
        print("detail=" .. tostring(err.detail or err.status))
        return 1
    end
    print("connected=yes")
    print("negotiated_kex=" .. tostring(conn:negotiated().kex))
    print("negotiated_cipher=" .. tostring(conn:negotiated().cipher_c2s))

    local r = conn:exec("echo hull-tunnel-ok")
    print("exec_status=" .. tostring(r.status))
    print("exec_stdout=" .. tostring((r.stdout or ""):gsub("%s+$", "")))

    -- A non-zero remote exit is a SUCCESSFUL call with a non-zero status,
    -- not an error. Worth asserting through a tunnel too: a relay that ate
    -- the exit-status message would look like success.
    local r2 = conn:exec("exit 3")
    print("exit3_status=" .. tostring(r2.status))

    conn:close()
    print("done=yes")
    return 0
end)
LUA
}

# Every case talks to a loopback peer, so a run that has not finished in 20s
# is not slow, it is stuck. Bounded here rather than left to the caller's
# budget: a suite that HANGS spends a CI job's whole allowance and reports
# nothing, where one that fails prints which assertion never arrived.
#
# It is not hypothetical. On a cosmo APE on Windows the outbound connect
# never completes (docs/windows_e2e_status.md), and without this the suite
# sat there until the runner killed it.
run_app() {
    if command -v timeout >/dev/null 2>&1; then
        (cd "$1" && timeout 20 "$HULL" --no-sandbox app.lua 2>&1) || true
    else
        (cd "$1" && "$HULL" --no-sandbox app.lua 2>&1) || true
    fi
}

echo "e2e_ssh_tunnel: hull/ssh through a WebSocket relay"

# ── The seam: tunnel plumbing, no SSH server needed ─────────────────────
echo
echo "The tunnel seam (real socket, real frames)"

SSH_LOGIN="${USER:-$(id -un 2>/dev/null || echo tester)}"
PEER_PORT=$(free_port)
RELAY_PORT=$(free_port)

# A TCP peer that says something SSH-shaped and then nothing. Enough to prove
# bytes crossed the tunnel in both directions and reached the SSH transport;
# the handshake then fails, which the live session below does properly.
python3 -c "
import socket
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', $PEER_PORT)); srv.listen(4)
open('$WORK/peer-ready', 'w').write('x')
while True:
    c, _ = srv.accept()
    try:
        c.sendall(b'SSH-2.0-E2EFake\r\n')
        c.recv(65536)
    except OSError:
        pass
    finally:
        c.close()
" &
PEER_PID=$!
wait_for_file "$WORK/peer-ready" || { echo "peer did not start"; exit 1; }

# Headers arrive verbatim, over a real socket.
start_shim "$RELAY_PORT" "$PEER_PORT"
write_app "$WORK/a1" "127.0.0.1" "$PEER_PORT" "127.0.0.1" "$RELAY_PORT" \
    '"127.0.0.1"' "$PEER_PORT" '"127.0.0.1"' "$RELAY_PORT"
OUT=$(run_app "$WORK/a1")
SEEN=$(cat "$WORK/headers.txt" 2>/dev/null || echo "")
stop_shim

assert_contains "seam: the relay saw a WebSocket upgrade" "$SEEN" "Upgrade: websocket"
assert_contains "seam: Cf-Access-Client-Id arrived verbatim" "$SEEN" "Cf-Access-Client-Id: e2e.access"
assert_contains "seam: Cf-Access-Client-Secret arrived verbatim" "$SEEN" "Cf-Access-Client-Secret: e2e-secret"
assert_contains "seam: Cf-Access-Jump-Destination names the target" "$SEEN" "Cf-Access-Jump-Destination: 127.0.0.1:$PEER_PORT"
# It got past the upgrade and into SSH: the failure is a protocol one, not an
# upgrade one and not a manifest denial.
case "$OUT" in
    *first_code=upgrade_*|*first_code=denied*)
        fail "seam: reached the SSH transport through the tunnel" "$OUT" ;;
    *first_code=*)
        pass "seam: reached the SSH transport through the tunnel" ;;
    *)  fail "seam: reached the SSH transport through the tunnel" "$OUT" ;;
esac

# An Access-style 403 from a REAL relay is reported as upgrade_refused.
start_shim "$RELAY_PORT" "$PEER_PORT" --require-header "Cf-Access-Client-Id: nope.access"
write_app "$WORK/a2" "127.0.0.1" "$PEER_PORT" "127.0.0.1" "$RELAY_PORT" \
    '"127.0.0.1"' "$PEER_PORT" '"127.0.0.1"' "$RELAY_PORT"
OUT=$(run_app "$WORK/a2")
stop_shim
assert_line "$OUT" "first_code" "upgrade_refused" "seam: a 403 from the relay is upgrade_refused, not denied"

# The relay grant bites: a relay the manifest does not name is refused,
# and refused BEFORE a socket is opened (the shim is not even running).
write_app "$WORK/a3" "127.0.0.1" "$PEER_PORT" "127.0.0.1" "$RELAY_PORT" \
    '"127.0.0.1"' "$PEER_PORT" '"relay.invalid"' "$RELAY_PORT"
OUT=$(run_app "$WORK/a3")
assert_line "$OUT" "first_code" "denied" "seam: a relay outside ssh.tunnel.hosts is denied"
assert_contains "seam: the denial names the tunnel list" "$OUT" "ssh.tunnel.hosts"

# The destination grant still bites THROUGH a tunnel. This is the one that
# matters: an allowed relay must not become a way to reach any host behind it.
start_shim "$RELAY_PORT" "$PEER_PORT"
write_app "$WORK/a4" "10.9.9.9" "$PEER_PORT" "127.0.0.1" "$RELAY_PORT" \
    '"127.0.0.1"' "$PEER_PORT" '"127.0.0.1"' "$RELAY_PORT"
OUT=$(run_app "$WORK/a4")
stop_shim
assert_line "$OUT" "first_code" "denied" "seam: an allowed relay does not widen the destination"
assert_contains "seam: the denial names the connect list" "$OUT" "ssh.connect.hosts"

kill "$PEER_PID" 2>/dev/null || true
PEER_PID=""

# ── The live session: a real SSH server behind the relay ────────────────
echo
echo "A live session: a real OpenSSH server behind the relay"

SSHD=""
for cand in /usr/sbin/sshd /usr/local/sbin/sshd /opt/homebrew/sbin/sshd; do
    [ -x "$cand" ] && SSHD="$cand" && break
done
[ -z "$SSHD" ] && SSHD=$(command -v sshd 2>/dev/null || true)

if [ -z "$SSHD" ] || ! command -v ssh-keygen >/dev/null 2>&1; then
    if [ "${HULL_E2E_REQUIRE_SSHD:-0}" = "1" ]; then
        fail "live: sshd was required but not found (HULL_E2E_REQUIRE_SSHD=1)"
    else
        echo "  SKIP: no sshd / ssh-keygen here (the seam checks above still ran)"
    fi
else
    SSH_PORT=$(free_port)
    RELAY_PORT=$(free_port)
    mkdir -p "$WORK/sshd"
    ssh-keygen -q -t ed25519 -N '' -f "$WORK/sshd/host_key" </dev/null
    cp "$WORK/client_key.pub" "$WORK/sshd/authorized_keys"
    chmod 600 "$WORK/sshd/host_key" "$WORK/sshd/authorized_keys"

    # A throwaway sshd on a high port, run as the invoking user - which is the
    # only login it can authenticate, and the one we use. StrictModes off
    # because the key material lives in a temp dir, not in ~/.ssh.
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
LogLevel VERBOSE
Subsystem sftp internal-sftp
CFG

    "$SSHD" -f "$WORK/sshd/sshd_config" -D -e >"$WORK/sshd/log" 2>&1 &
    SSHD_PID=$!

    # Wait for the port, not for a sleep.
    _ok=0
    _i=0
    while [ "$_i" -lt 100 ]; do
        if python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(0.2)
sys.exit(0 if s.connect_ex(('127.0.0.1', $SSH_PORT)) == 0 else 1)
" 2>/dev/null; then _ok=1; break; fi
        _i=$((_i + 1))
        sleep 0.1 2>/dev/null || sleep 1
    done

    if [ "$_ok" -ne 1 ]; then
        if [ "${HULL_E2E_REQUIRE_SSHD:-0}" = "1" ]; then
            fail "live: sshd would not start (HULL_E2E_REQUIRE_SSHD=1)"
        else
            echo "  SKIP: sshd would not start on this host"
        fi
        sed 's/^/    sshd: /' "$WORK/sshd/log" 2>/dev/null | head -10
    else
        start_shim "$RELAY_PORT" "$SSH_PORT"
        write_app "$WORK/b1" "127.0.0.1" "$SSH_PORT" "127.0.0.1" "$RELAY_PORT" \
            '"127.0.0.1"' "$SSH_PORT" '"127.0.0.1"' "$RELAY_PORT"
        OUT=$(run_app "$WORK/b1")
        SEEN=$(cat "$WORK/headers.txt" 2>/dev/null || echo "")
        stop_shim

        assert_line "$OUT" "first_code" "host_unknown" "live: an unknown host key is refused, not assumed"
        assert_line "$OUT" "fingerprint_shape" "true" "live: the refusal carries a SHA256 fingerprint"
        assert_line "$OUT" "connected" "yes" "live: connected through the relay after accepting the key"
        assert_line "$OUT" "negotiated_kex" "curve25519-sha256" "live: negotiated curve25519-sha256 through the tunnel"
        assert_line "$OUT" "negotiated_cipher" "aes256-gcm@openssh.com" "live: negotiated aes256-gcm through the tunnel"
        assert_line "$OUT" "exec_status" "0" "live: exec succeeded over the tunnel"
        assert_line "$OUT" "exec_stdout" "hull-tunnel-ok" "live: command output came back through the tunnel"
        assert_line "$OUT" "exit3_status" "3" "live: a non-zero remote exit is a status, not an error"
        assert_line "$OUT" "done" "yes" "live: the session closed cleanly"

        # Two upgrades: one per connect. The host-key accept is a real
        # reconnect through a real relay, not a cached handle.
        UPGRADES=$(grep -c '^GET / HTTP/1.1$' "$WORK/headers.txt" 2>/dev/null || echo 0)
        if [ "$UPGRADES" -ge 2 ]; then
            pass "live: the reconnect after accepting the host key is a second real tunnel"
        else
            fail "live: the reconnect after accepting the host key is a second real tunnel" "$UPGRADES upgrades"
        fi

        if [ "$FAIL" -ne 0 ]; then
            echo "  --- app output ---"
            printf '%s\n' "$OUT" | sed 's/^/    /'
            echo "  --- sshd log (tail) ---"
            tail -20 "$WORK/sshd/log" 2>/dev/null | sed 's/^/    /'
        fi
    fi
fi

echo
echo "e2e_ssh_tunnel: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
