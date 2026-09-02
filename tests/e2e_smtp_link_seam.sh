#!/usr/bin/env bash
# e2e_smtp_link_seam.sh - the SMTP/HTTP-client feature composition boundary.
#
# The model-2 async SMTP objects (cap/smtp_async + transport/worker/admission/
# submit/in-flight/op/tls) follow the SAME feature gate as cap/smtp.c (the send +
# the audit writer): all live in libhull_feature-http.a, none in the base. A
# regression that leaves any of them base-resident whole-archives them into a
# non-SMTP app whose base then dangles hl_smtp_audit_complete (the audit writer
# lives only in the feature). This test pins BOTH directions:
#
#   1. Compute/non-HTTP app: composes NO real SMTP object and no undefined kl_*.
#   2. SMTP app: composes the whole coherent set - the async layer AND the audit
#      writer resolve.
#
# REVERT PROOF (manual, documented): remove cap_smtp_async.o et al. from
# FEATURE_SMTP_OBJS in mk/features/http.mk (back into the base) and rebuild - the
# compute app below fails to link with `undefined reference to hl_smtp_audit_complete`.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$ROOT/build/hull"
pass() { echo "  ok: $1"; }
fail() { echo "FAIL: $1"; exit 1; }

command -v nm >/dev/null 2>&1 || { echo "PASS: e2e_smtp_link_seam (nm absent; skipped)"; exit 0; }
[ -x "$HULL" ] || { echo "SKIP: build/hull missing (run make first)"; exit 0; }
# hull build needs a platform lib (embedded, or the local fallback build/libhull_platform.a).
[ -f "$ROOT/build/libhull_platform.a" ] || make -C "$ROOT" platform >/dev/null 2>&1 || \
    { echo "SKIP: no platform lib + no system cc to build one"; exit 0; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ── a compute (non-HTTP, non-SMTP) app.main app ────────────────────────
mkdir -p "$WORK/pc"
printf 'app.manifest({ modules = {} })\napp.main(function() return 7 end)\n' > "$WORK/pc/app.lua"
out=$("$HULL" build --no-verify-platform --compiler=system "$WORK/pc" -o "$WORK/pc/app" 2>&1) \
    || { echo "$out"; fail "compute app must build (no dangling SMTP audit reference)"; }
rc=0; "$WORK/pc/app" >/dev/null 2>&1 || rc=$?
[ "$rc" = 7 ] || fail "compute app should exit 7, got $rc"
# No REAL SMTP capability object linked (the send/transport/worker/audit machinery).
real=$(nm "$WORK/pc/app" 2>/dev/null | grep -cE ' [Tt] _?hl_smtp_(execute|audit_complete|format_message|transport|wop|submit|admission|inflight)' || true)
[ "$real" = 0 ] || fail "compute app links real SMTP objects (got $real), boundary broken"
# A fully-linked executable has no undefined symbols; assert zero undefined kl_*.
undef=$(nm "$WORK/pc/app" 2>/dev/null | grep -cE ' U _?kl_' || true)
[ "$undef" = 0 ] || fail "compute app has $undef undefined kl_* symbols"
pass "compute app: zero real SMTP objects, zero undefined kl_*, runs (exit 7)"

# ── an SMTP app (declares hull/smtp + calls smtp.send) ─────────────────
mkdir -p "$WORK/mail"
cat > "$WORK/mail/app.lua" <<'LUA'
local smtp = require("hull.smtp")
app.manifest({ modules = { "hull/smtp@1" }, hosts = { "127.0.0.1" } })
app.main(function()
    local r = smtp.send({ host = "127.0.0.1", port = 2525, from = "a@x",
                          to = "b@y", subject = "s", body = "b" })
    return r.ok and 0 or 3
end)
LUA
out=$("$HULL" build --no-verify-platform --compiler=system "$WORK/mail" -o "$WORK/mail/app" 2>&1) \
    || { echo "$out"; fail "SMTP app must compose the whole SMTP feature and link"; }
a=$(nm "$WORK/mail/app" 2>/dev/null | grep -cE ' [Tt] _?hl_smtp_audit_complete' || true)
s=$(nm "$WORK/mail/app" 2>/dev/null | grep -cE ' [Tt] _?hl_smtp_server_ctx_init' || true)
e=$(nm "$WORK/mail/app" 2>/dev/null | grep -cE ' [Tt] _?hl_smtp_execute' || true)
[ "$a" -ge 1 ] || fail "SMTP app missing the audit writer (hl_smtp_audit_complete)"
[ "$s" -ge 1 ] || fail "SMTP app missing the async server ctx (hl_smtp_server_ctx_init)"
[ "$e" -ge 1 ] || fail "SMTP app missing the transport execute (hl_smtp_execute)"
# STRONG-impl proof: the three hl_smtp_server_* resolved to cap/smtp_async.o's STRONG
# definitions (composed via the http feature), NOT the base http_weakstub.o weak no-op
# stubs. macOS nm -m marks a weak def "weak external"; GNU nm marks it "W".
if nm -m "$WORK/mail/app" >/dev/null 2>&1; then
    weak=$(nm -m "$WORK/mail/app" 2>/dev/null | grep -cE 'weak external _?hl_smtp_server_' || true)
else
    weak=$(nm "$WORK/mail/app" 2>/dev/null | grep -cE ' W _?hl_smtp_server_' || true)
fi
[ "$weak" = 0 ] || fail "SMTP app resolved hl_smtp_server_* to WEAK no-op stubs (got $weak), not the strong impls"
pass "SMTP app: async objects + audit writer resolve to STRONG impls (not weak stubs)"

# ── the COMPOSED binary completes a real async SMTP send ───────────────
# Build + RUN a server app whose handler does an async smtp.send against a one-shot
# mock peer. This proves the strong async SMTP objects are not merely linked but
# EXECUTE end to end in the composed binary.
if command -v python3 >/dev/null 2>&1 && command -v curl >/dev/null 2>&1; then
    read -r SMTP_PORT HTTP_PORT <<EOF2
$(python3 -c 'import socket
def p():
    s=socket.socket(); s.bind(("127.0.0.1",0)); n=s.getsockname()[1]; s.close(); return n
print(p(), p())')
EOF2
    cat > "$WORK/mock.py" <<PY
import socket,sys
port=int(sys.argv[1])
srv=socket.socket(); srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
srv.bind(("127.0.0.1",port)); srv.listen(1)
sys.stderr.write("ready\n"); sys.stderr.flush()
c,_=srv.accept(); f=c.makefile("rwb",buffering=0); f.write(b"220 mock\r\n")
indata=False
while True:
    line=f.readline()
    if not line: break
    if indata:
        if line==b".\r\n": f.write(b"250 ok\r\n"); indata=False
        continue
    u=line.upper()
    if u.startswith(b"DATA"): f.write(b"354 go\r\n"); indata=True
    elif u.startswith(b"QUIT"): f.write(b"221 bye\r\n"); break
    else: f.write(b"250 ok\r\n")
c.close()
PY
    python3 "$WORK/mock.py" "$SMTP_PORT" 2>"$WORK/mock.err" &
    MOCK=$!
    for _ in $(seq 1 50); do grep -q ready "$WORK/mock.err" 2>/dev/null && break; sleep 0.1; done
    mkdir -p "$WORK/srv"
    cat > "$WORK/srv/app.lua" <<LUA
local smtp = require("hull.smtp")
app.manifest({ modules = { "hull/smtp@1", "hull/http-server@1" }, hosts = { "127.0.0.1" } })
app.get("/", function(req, res) res:json({ ready = true }) end)
app.get("/send", function(req, res)
    res:json(smtp.send({ host = "127.0.0.1", port = $SMTP_PORT, from = "a@x",
                         to = "b@y", subject = "s", body = "b" }))
end)
LUA
    out=$("$HULL" build --no-verify-platform --compiler=system "$WORK/srv" -o "$WORK/srv/app" 2>&1) \
        || { echo "$out"; kill "$MOCK" 2>/dev/null; fail "server+SMTP app must compose + link"; }
    "$WORK/srv/app" -p "$HTTP_PORT" >/dev/null 2>&1 &
    SRV=$!
    for _ in $(seq 1 60); do curl -s "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null | grep -q ready && break; sleep 0.2; done
    R=$(curl -s --max-time 10 "http://127.0.0.1:$HTTP_PORT/send")
    kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null
    kill "$MOCK" 2>/dev/null; wait "$MOCK" 2>/dev/null || true
    case "$R" in
        *'"ok":true'*) pass "composed binary completes a real async SMTP send (strong impls execute): $R" ;;
        *) fail "composed binary async SMTP send did not succeed (got: $R)" ;;
    esac
else
    echo "  skip: real-async-send leg (python3/curl absent)"
fi

echo "PASS: e2e_smtp_link_seam"
