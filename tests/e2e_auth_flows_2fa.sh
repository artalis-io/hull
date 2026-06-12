#!/bin/sh
# E2E test for hull/web/auth-flows + TOTP 2FA composition.
#
# Walks the full 2FA flow per runtime (Lua + JS):
#
#   1. Register + verify a user (auth-flows happy path).
#   2. Enroll TOTP for the user (debug endpoint returns secret +
#      recovery codes).
#   3. Confirm the TOTP enrollment with a freshly-computed code.
#   4. Attempt login → expect pending_2fa = true + totp_token in
#      the response (no session cookie yet).
#   5. POST /auth/totp-verify with a WRONG code → expect 401
#      AND the pending token still usable (retry-on-typo).
#   6. POST /auth/totp-verify with the right code → expect ok +
#      session cookie + /_me reflects the user.
#   7. Logout.
#   8. Login again, get a new pending token, complete with a
#      RECOVERY code (one-use; the totp module canonicalizes).
#   9. Replay the SAME totp_token (already burned by step 6) →
#      expect rejection ("already used").
#   10. Login → magic-link path: request magic link, click it,
#       expect default HTML form rendered (not on_login). Submit
#       to /auth/totp-verify with the totp_token parsed out of
#       the form to complete login.
#
# Uses tests/fixtures/auth_flows_2fa_{lua,js} as fixtures and the
# same in-process email capture pattern as e2e_auth_flows.sh.
#
# Requires: build/hull, curl, python3.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS=0
FAIL=0
RUNTIME=${RUNTIME:-all}
HULL_PID=""
TMPDIR_WORK=""

if [ ! -x "$HULL" ]; then
    echo "e2e_auth_flows_2fa: hull binary not found at $HULL — run 'make' first"
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "e2e_auth_flows_2fa: python3 required"
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
    rm -f "$SRCDIR/data.db" "$SRCDIR/data.db-shm" "$SRCDIR/data.db-wal"
    if [ -n "$TMPDIR_WORK" ] && [ -d "$TMPDIR_WORK" ]; then
        rm -rf "$TMPDIR_WORK"
    fi
}
trap cleanup EXIT

TMPDIR_WORK=$(mktemp -d)

# Tiny TOTP helper — same shape as tests/e2e_totp.sh's. Keeps the
# Python in this script so the e2e is single-file readable.
TOTP_PY="$TMPDIR_WORK/totp.py"
cat >"$TOTP_PY" <<'EOF'
import base64, hmac, hashlib, struct, sys, time
b32 = sys.argv[1]
off = int(sys.argv[2]) if len(sys.argv) > 2 else 0
key = base64.b32decode(b32 + "=" * (-len(b32) % 8))
step = int(time.time()) // 30 + off
msg = struct.pack(">Q", step)
mac = hmac.new(key, msg, hashlib.sha1).digest()
o = mac[19] & 0xF
p = ((mac[o] & 0x7F) << 24) | (mac[o+1] << 16) | (mac[o+2] << 8) | mac[o+3]
print(str(p % 1000000).zfill(6))
EOF
# Each step is consumed once by the totp module's last_used_step
# guard, so the e2e walks monotonically through offsets:
#   confirm = -1, login = 0, magic-link = +1
# (All within the default ±1 verification window.)
totp_code() { python3 "$TOTP_PY" "$1" "${2:-0}"; }

# Read latest email body for an address (text field).
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
# Parse a totp_token out of a JSON response or HTML form.
extract_totp_token_json() {
    printf '%s\n' "$1" | python3 -c "
import json, sys
try: print(json.loads(sys.stdin.read()).get('totp_token', ''))
except Exception: print('')
"
}
extract_totp_token_html() {
    printf '%s\n' "$1" | python3 -c "
import re, sys
m = re.search(r'name=\"token\" value=\"([^\"]+)\"', sys.stdin.read())
print(m.group(1) if m else '')
"
}
extract_first_recovery() {
    printf '%s\n' "$1" | python3 -c "
import json, sys
print(json.loads(sys.stdin.read())['recovery_codes'][0])
"
}

run_flow() {
    _label="$1"
    _entry="$2"

    echo ""
    echo "=== Step ($_label): Start 2FA fixture + run e2e ==="

    rm -f "$SRCDIR/data.db" "$SRCDIR/data.db-shm" "$SRCDIR/data.db-wal"

    PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
    HULL_LOG="$TMPDIR_WORK/hull_$_label.log"
    "$HULL" dev "$_entry" -p "$PORT" >"$HULL_LOG" 2>&1 &
    HULL_PID=$!
    if ! wait_for_server "$PORT"; then
        fail "$_label: fixture did not start"
        cat "$HULL_LOG"
        return
    fi
    pass "$_label: fixture up on :$PORT"

    COOKIES="$TMPDIR_WORK/cookies_$_label.txt"
    : > "$COOKIES"
    BASE="http://127.0.0.1:$PORT"
    EMAIL="alice@example.test"
    PW="hunter22hunter22"

    # 1. Register + verify.
    curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}" \
        "$BASE/auth/register" > /dev/null
    TEXT=$(last_email_text "$PORT" "$EMAIL")
    VERIFY_URL=$(extract_url "$TEXT")
    S=$(curl -sS -o /dev/null -w '%{http_code}' "$VERIFY_URL")
    check_status "$_label: register+verify ok" "$S" "302"

    # 2. Enroll TOTP.
    ENROLL=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\"}" "$BASE/_totp_enroll")
    SECRET=$(echo "$ENROLL" | python3 -c 'import json,sys; print(json.load(sys.stdin)["secret"])')
    RECOVERY=$(extract_first_recovery "$ENROLL")
    [ -n "$SECRET" ] && pass "$_label: enroll returns secret" \
        || fail "$_label: enroll returned no secret: $ENROLL"

    # 3. Confirm enrollment using a PAST step code so the current
    #    step stays available for the login flow that follows.
    CODE=$(totp_code "$SECRET" -1)
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"code\":\"$CODE\"}" \
        "$BASE/_totp_confirm")
    check_contains "$_label: confirm enrollment ok" "$R" '"ok":true'

    # 4. Login → pending_2fa response.
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}" \
        "$BASE/auth/login")
    check_contains "$_label: login returns pending_2fa" "$R" '"pending_2fa":true'
    TOTP_TOKEN=$(extract_totp_token_json "$R")
    [ -n "$TOTP_TOKEN" ] && pass "$_label: login returns totp_token" \
        || fail "$_label: no totp_token in: $R"

    # 4b. /_me should NOT have a session yet (no cookie was set).
    S=$(curl -sS -o /dev/null -w '%{http_code}' "$BASE/_me")
    check_status "$_label: no session pre-2fa" "$S" "401"

    # 5. Wrong code → 401, token still usable.
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        -d "{\"token\":\"$TOTP_TOKEN\",\"code\":\"000000\"}" \
        "$BASE/auth/totp-verify")
    check_status "$_label: wrong code returns 401" "$S" "401"

    # 6. Right code → ok + session. (Current-step code; confirm
    #    used step-1 so this step is still available.)
    CODE=$(totp_code "$SECRET" 0)
    R=$(curl -sS -c "$COOKIES" -X POST -H 'Content-Type: application/json' \
        -d "{\"token\":\"$TOTP_TOKEN\",\"code\":\"$CODE\"}" \
        "$BASE/auth/totp-verify")
    check_contains "$_label: right code completes login" "$R" '"ok":true'
    R=$(curl -sS -b "$COOKIES" "$BASE/_me")
    check_contains "$_label: /_me reflects session post-2fa" \
        "$R" "\"email\":\"$EMAIL\""

    # 9. (early) Replay the burned totp_token → reject.
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        -d "{\"token\":\"$TOTP_TOKEN\",\"code\":\"$CODE\"}" \
        "$BASE/auth/totp-verify")
    check_status "$_label: totp_token replay rejected" "$S" "400"

    # 7. Logout.
    : > "$COOKIES"
    curl -sS -b "$COOKIES" -c "$COOKIES" -X POST "$BASE/auth/logout" > /dev/null

    # 8. Re-login + recovery code path.
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}" \
        "$BASE/auth/login")
    TOTP_TOKEN2=$(extract_totp_token_json "$R")
    R=$(curl -sS -c "$COOKIES" -X POST -H 'Content-Type: application/json' \
        -d "{\"token\":\"$TOTP_TOKEN2\",\"code\":\"$RECOVERY\"}" \
        "$BASE/auth/totp-verify")
    check_contains "$_label: recovery code accepted" "$R" '"ok":true'

    # 10. Magic-link path with 2FA — click the link, expect default
    #     HTML form. Submit it to /auth/totp-verify with a fresh code.
    : > "$COOKIES"
    curl -sS -X POST "$BASE/_emails/clear" > /dev/null
    curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\"}" "$BASE/auth/magic-link" > /dev/null
    TEXT=$(last_email_text "$PORT" "$EMAIL")
    MAGIC_URL=$(extract_url "$TEXT")
    HTML=$(curl -sS "$MAGIC_URL")
    check_contains "$_label: magic-link click renders TOTP form" \
        "$HTML" '/auth/totp-verify'
    TOTP_TOKEN3=$(extract_totp_token_html "$HTML")
    [ -n "$TOTP_TOKEN3" ] && pass "$_label: magic-link form has totp_token" \
        || fail "$_label: no totp_token in form HTML"
    # Step +1 (login already consumed step 0; magic-link picks the
    # next future step within the ±1 window).
    CODE=$(totp_code "$SECRET" 1)
    R=$(curl -sS -c "$COOKIES" -X POST \
        -d "token=$TOTP_TOKEN3&code=$CODE" \
        -H 'Content-Type: application/x-www-form-urlencoded' \
        "$BASE/auth/totp-verify")
    check_contains "$_label: magic-link 2FA completes" "$R" '"ok":true'

    stop_pid "$HULL_PID"; HULL_PID=""
}

if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "lua" ]; then
    run_flow lua "$SRCDIR/tests/fixtures/auth_flows_2fa_lua/app.lua"
fi
if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "js" ]; then
    run_flow js "$SRCDIR/tests/fixtures/auth_flows_2fa_js/app.js"
fi

echo ""
echo "=== Summary ==="
echo "PASSED: $PASS"
echo "FAILED: $FAIL"

if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
