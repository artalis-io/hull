#!/bin/sh
# E2E test for the hull/web/auth-flows hardening features (round
# 5: resend verify, account lockout, email-change notify+revoke,
# pwned-password check).
#
# Per runtime (Lua + JS):
#
#   1. Register a user with a CLEAN password. Confirm welcome
#      email captured.
#   2. Re-send verify (the original welcome may have been lost):
#      POST /auth/verify/resend → expect ok + new email captured.
#   3. Click verify link, login → ok.
#   4. Logout. Re-attempt login with WRONG password 3 times
#      (matches max_failed_logins). 4th attempt expects 429 +
#      Retry-After. Right password attempt during the window also
#      expects 429 (lockout overrides correct credentials).
#   5. Wait 2s (lockout_duration), then login with correct
#      password → ok (auto-cleared).
#   6. Email-change: from logged-in session, request
#      change to alice.new@... Confirm BOTH emails captured —
#      one to NEW with confirm link, one to OLD with revoke link.
#      Click revoke. Then click confirm — expect 'email change
#      failed' (revoke already deleted the pending row).
#   7. Email-change again, NEW link confirmed → expect 302
#      and login with new email works.
#   8. Pwned-password check: register attempt with "password"
#      (HIBP-known) → 400 with pwned error. Register with a
#      random password → ok.
#
# Uses a Python-spawned HIBP mock on localhost. The mock returns
# the SHA-1 suffix for "password" only; other prefixes get empty
# 200, so they aren't classified as pwned.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS=0
FAIL=0
RUNTIME=${RUNTIME:-all}
HULL_PID=""
HIBP_PID=""
TMPDIR_WORK=""

if [ ! -x "$HULL" ]; then
    echo "e2e_auth_flows_hardening: hull binary not found at $HULL — run 'make' first"
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "e2e_auth_flows_hardening: python3 required"
    exit 1
fi

fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }

check_status() {
    if [ "$2" = "$3" ]; then pass "$1"
    else fail "$1 — expected status $3, got $2"
    fi
}
check_contains() {
    case "$2" in *"$3"*) pass "$1" ;;
                 *) fail "$1 — expected '$3' in: $(echo "$2" | head -c 200)" ;;
    esac
}

wait_for_server() {
    for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if curl -sS -o /dev/null -m 1 "http://127.0.0.1:$1/" 2>/dev/null; then
            return 0
        fi
        sleep 0.3
    done
    return 1
}

stop_pid() {
    if [ -n "$1" ]; then
        kill "$1" 2>/dev/null || true
        wait "$1" 2>/dev/null || true
    fi
}

cleanup() {
    stop_pid "$HULL_PID"
    stop_pid "$HIBP_PID"
    rm -f "$SRCDIR/data.db" "$SRCDIR/data.db-shm" "$SRCDIR/data.db-wal"
    if [ -n "$TMPDIR_WORK" ] && [ -d "$TMPDIR_WORK" ]; then
        rm -rf "$TMPDIR_WORK"
    fi
}
trap cleanup EXIT

TMPDIR_WORK=$(mktemp -d)

# HIBP mock — single canned response for SHA1("password") prefix
# 5BAA6 with suffix 1E4C9B93F3F0682250B6CF8331B7EE68FD8.
HIBP_PY="$TMPDIR_WORK/hibp_mock.py"
cat >"$HIBP_PY" <<'EOF'
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler
PORT = int(sys.argv[1])
CANNED = {"5BAA6": "1E4C9B93F3F0682250B6CF8331B7EE68FD8:99999\r\n"}
class H(BaseHTTPRequestHandler):
    def log_message(self, *a, **k): pass
    def do_GET(self):
        # Expect /range/<prefix>
        parts = self.path.rstrip("/").split("/")
        prefix = parts[-1].upper() if parts else ""
        body = CANNED.get(prefix, "")
        b = body.encode("ascii")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)
HTTPServer(("127.0.0.1", PORT), H).serve_forever()
EOF

last_email_text() {
    curl -sS "http://127.0.0.1:$1/_emails" | python3 -c "
import json, sys
emails = json.load(sys.stdin)
for e in reversed(emails):
    if e.get('to') == '$2':
        print(e.get('text', '')); break
"
}
extract_url() {
    printf '%s\n' "$1" | python3 -c "
import re, sys
m = re.search(r'https?://[^\s]+', sys.stdin.read())
print(m.group(0) if m else '')
"
}

run_flow() {
    _label="$1"
    _entry="$2"

    echo ""
    echo "=== Step ($_label): Start hardening fixture + e2e ==="
    rm -f "$SRCDIR/data.db" "$SRCDIR/data.db-shm" "$SRCDIR/data.db-wal"

    # HIBP mock port — fixed so the fixture can hardcode it.
    # (Hull's env cap isn't wired during a fixture's top-level
    # code; env.get would throw, so we can't pass a per-run port
    # through the environment.) Pick something unusual to avoid
    # collisions in a typical dev box.
    HIBP_PORT=39911
    python3 "$HIBP_PY" "$HIBP_PORT" >/dev/null 2>&1 &
    HIBP_PID=$!
    sleep 0.4

    PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
    HULL_LOG="$TMPDIR_WORK/hull_$_label.log"
    "$HULL" dev "$_entry" -p "$PORT" >"$HULL_LOG" 2>&1 &
    HULL_PID=$!
    if ! wait_for_server "$PORT"; then
        fail "$_label: fixture did not start"
        cat "$HULL_LOG"
        return
    fi
    pass "$_label: fixture up on :$PORT (hibp-mock :$HIBP_PORT)"
    # Each run shares the mock port; second run kills/restarts.

    COOKIES="$TMPDIR_WORK/cookies_$_label.txt"
    : > "$COOKIES"
    BASE="http://127.0.0.1:$PORT"
    EMAIL="alice@example.test"
    EMAIL_NEW="alice.new@example.test"
    PW="hunter22hunter22"

    # 1. Register with a clean password.
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}" \
        "$BASE/auth/register")
    check_contains "$_label: register ok" "$R" '"ok":true'
    TEXT=$(last_email_text "$PORT" "$EMAIL")
    [ -n "$TEXT" ] && pass "$_label: welcome email captured" \
        || fail "$_label: no welcome email captured"

    # 2. Resend verify.
    curl -sS -X POST "$BASE/_emails/clear" > /dev/null
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\"}" "$BASE/auth/verify/resend")
    check_contains "$_label: resend ok" "$R" '"ok":true'
    TEXT=$(last_email_text "$PORT" "$EMAIL")
    VERIFY_URL=$(extract_url "$TEXT")
    check_contains "$_label: resend produced verify URL" \
        "$VERIFY_URL" "/auth/verify?token="
    # 2b. Resend for ALREADY-VERIFIED user (after step 3) is
    # enumeration-safe — covered after verify.

    # 3. Verify + login.
    S=$(curl -sS -o /dev/null -w '%{http_code}' "$VERIFY_URL")
    check_status "$_label: verify 302" "$S" "302"
    # Re-send AFTER verify should still ok-respond but emit no
    # new email (enumeration-safe).
    curl -sS -X POST "$BASE/_emails/clear" > /dev/null
    curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\"}" "$BASE/auth/verify/resend" > /dev/null
    N=$(curl -sS "$BASE/_emails" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')
    if [ "$N" = "0" ]; then pass "$_label: resend post-verify silent"
    else fail "$_label: resend post-verify sent email (count=$N)"
    fi

    # 4. Account lockout — 3 wrong passwords then expect 429.
    for _i in 1 2 3; do
        curl -sS -o /dev/null -X POST -H 'Content-Type: application/json' \
            -d "{\"email\":\"$EMAIL\",\"password\":\"wrong-pw\"}" \
            "$BASE/auth/login"
    done
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"password\":\"wrong-pw\"}" \
        "$BASE/auth/login")
    check_status "$_label: 4th wrong login locks out (429)" "$S" "429"
    # Correct password during lockout window still 429.
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}" \
        "$BASE/auth/login")
    check_status "$_label: correct login during lockout still 429" "$S" "429"
    # Retry-After header is present. `curl -i` prints headers
    # for a normal POST (`-I` would turn it into a HEAD request
    # with the body still attached, which Hull doesn't route).
    H=$(curl -sS -i -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}" \
        "$BASE/auth/login" 2>/dev/null | grep -i '^retry-after' | head -1)
    [ -n "$H" ] && pass "$_label: Retry-After header present" \
        || fail "$_label: Retry-After header missing"

    # 5. Wait out lockout, login succeeds + lockout cleared.
    sleep 2.5
    R=$(curl -sS -c "$COOKIES" -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}" \
        "$BASE/auth/login")
    check_contains "$_label: login after lockout window ok" "$R" '"ok":true'

    # 6. Email-change with notify+revoke. Capture BOTH emails.
    curl -sS -X POST "$BASE/_emails/clear" > /dev/null
    R=$(curl -sS -b "$COOKIES" -X POST -H 'Content-Type: application/json' \
        -d "{\"new_email\":\"$EMAIL_NEW\"}" \
        "$BASE/auth/email-change")
    check_contains "$_label: email-change request ok" "$R" '"ok":true'
    NEW_TEXT=$(last_email_text "$PORT" "$EMAIL_NEW")
    OLD_TEXT=$(last_email_text "$PORT" "$EMAIL")
    CONFIRM_URL=$(extract_url "$NEW_TEXT")
    REVOKE_URL=$(extract_url "$OLD_TEXT")
    check_contains "$_label: NEW address got confirm link" "$CONFIRM_URL" "/email-change/confirm?token="
    check_contains "$_label: OLD address got revoke link" "$REVOKE_URL" "/email-change/revoke?token="

    # 6b. Click revoke.
    R=$(curl -sS "$REVOKE_URL")
    check_contains "$_label: revoke succeeds" "$R" "canceled"
    # 6c. Confirm now fails — pending row deleted.
    S=$(curl -sS -o /dev/null -w '%{http_code}' "$CONFIRM_URL")
    check_status "$_label: confirm post-revoke fails (400)" "$S" "400"

    # 7. Re-request change, this time click confirm.
    curl -sS -X POST "$BASE/_emails/clear" > /dev/null
    curl -sS -b "$COOKIES" -X POST -H 'Content-Type: application/json' \
        -d "{\"new_email\":\"$EMAIL_NEW\"}" \
        "$BASE/auth/email-change" > /dev/null
    NEW_TEXT=$(last_email_text "$PORT" "$EMAIL_NEW")
    CONFIRM_URL=$(extract_url "$NEW_TEXT")
    S=$(curl -sS -o /dev/null -w '%{http_code}' "$CONFIRM_URL")
    check_status "$_label: email-change confirm 302" "$S" "302"
    # Login with new email works.
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_NEW\",\"password\":\"$PW\"}" \
        "$BASE/auth/login")
    check_contains "$_label: login with new email ok" "$R" '"ok":true'

    # 8. Pwned check — try registering with "password".
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d '{"email":"bob@example.test","password":"password"}' \
        "$BASE/auth/register")
    check_contains "$_label: pwned password rejected" "$R" "data breaches"
    # 8b. Clean password registers ok.
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d '{"email":"bob@example.test","password":"freshrandompw99"}' \
        "$BASE/auth/register")
    check_contains "$_label: clean password ok" "$R" '"ok":true'

    stop_pid "$HULL_PID"; HULL_PID=""
    stop_pid "$HIBP_PID"; HIBP_PID=""
}

if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "lua" ]; then
    run_flow lua "$SRCDIR/tests/fixtures/auth_flows_hardening_lua/app.lua"
fi
if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "js" ]; then
    run_flow js "$SRCDIR/tests/fixtures/auth_flows_hardening_js/app.js"
fi

echo ""
echo "=== Summary ==="
echo "PASSED: $PASS"
echo "FAILED: $FAIL"

if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
