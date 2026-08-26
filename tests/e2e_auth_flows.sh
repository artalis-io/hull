#!/bin/sh
# E2E test for hull/web/auth-flows.
#
# Walks the full lifecycle for each runtime (Lua + JS):
#
#   1. Register an account → assert welcome email captured with a
#      valid verify link.
#   2. Login before verifying → assert 403 (require_verified_email
#      default).
#   3. Click verify link → assert 302.
#   4. Login after verify → assert session cookie set + /_me reflects.
#   5. Logout → assert /_me back to 401.
#   6. Password-reset request → assert reset email captured.
#   7. Submit new password against reset token → assert success.
#   8. Login with new password → assert ok.
#   9. Replay the reset token → assert reject (single-use).
#   10. Magic-link request → assert magic email + link.
#   11. Click magic link → assert 200 + session set.
#   12. Email-change request (logged in) → assert email captured
#       on NEW address.
#   13. Click email-change confirm → assert email swapped in /_me.
#   14. Login with old email → assert fail.
#   15. Login with new email → assert ok.
#   16. Replay verify token → assert reject (single-use across flows).
#
# All emails are captured in-process by the fixture's email_send
# callback and read back via GET /_emails (debug endpoint, fixture-
# only). No SMTP / smtp4dev / mailpit dependency in CI.
#
# Usage: sh tests/e2e_auth_flows.sh
#        RUNTIME=lua sh tests/e2e_auth_flows.sh
#        RUNTIME=js  sh tests/e2e_auth_flows.sh
#
# Requires: build/hull, curl, python3 (for JSON parsing + free
# port discovery).
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
    echo "e2e_auth_flows: hull binary not found at $HULL - run 'make' first"
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "e2e_auth_flows: python3 required"
    exit 1
fi

fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }

check_status() {
    # $1 = label, $2 = actual, $3 = expected
    if [ "$2" = "$3" ]; then pass "$1"
    else fail "$1 - expected status $3, got $2"
    fi
}

check_contains() {
    case "$2" in *"$3"*) pass "$1" ;;
                 *) fail "$1 - expected '$3' in: $(echo "$2" | head -c 200)" ;;
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
    # hull's default DB lives in the cwd; tests use it briefly then
    # leave nothing behind.
    rm -f "$SRCDIR/data.db" "$SRCDIR/data.db-shm" "$SRCDIR/data.db-wal"
    if [ -n "$TMPDIR_WORK" ] && [ -d "$TMPDIR_WORK" ]; then
        rm -rf "$TMPDIR_WORK"
    fi
}
trap cleanup EXIT

TMPDIR_WORK=$(mktemp -d)

# Extract the latest captured email matching a `to:` address.
# Echoes the email's `text` field; empty string if no match.
last_email_text() {
    _port="$1"; _to="$2"
    curl -sS "http://127.0.0.1:$_port/_emails" | python3 -c "
import json, sys
emails = json.load(sys.stdin)
for e in reversed(emails):
    if e.get('to') == '$_to':
        print(e.get('text', '')); break
"
}

# Pull the first URL out of a captured email body.
extract_url() {
    printf '%s\n' "$1" | python3 -c "
import re, sys
text = sys.stdin.read()
m = re.search(r'https?://[^\s]+', text)
print(m.group(0) if m else '')
"
}

run_flow() {
    _label="$1"
    _entry="$2"

    echo ""
    echo "=== Step ($_label): Start fixture + run auth-flows e2e ==="

    # Clean any leftover DB in the cwd hull picks up by default.
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
    EMAIL_A="alice@example.test"
    EMAIL_B="alice.new@example.test"
    PW1="hunter22hunter22"
    PW2="newpassword12345"

    # 1. Register
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_A\",\"password\":\"$PW1\"}" \
        "$BASE/auth/register")
    check_contains "$_label: register returns ok" "$R" '"ok":true'

    # 1b. Welcome email captured with a verify link
    TEXT=$(last_email_text "$PORT" "$EMAIL_A")
    VERIFY_URL=$(extract_url "$TEXT")
    check_contains "$_label: welcome email contains verify URL" \
        "$VERIFY_URL" "/auth/verify?token="

    # 2. Login before verify → 403
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_A\",\"password\":\"$PW1\"}" \
        "$BASE/auth/login")
    check_status "$_label: login pre-verify is 403" "$S" "403"

    # 3. Click verify link
    S=$(curl -sS -o /dev/null -w '%{http_code}' "$VERIFY_URL")
    check_status "$_label: verify returns 302" "$S" "302"

    # 4. Login after verify
    R=$(curl -sS -c "$COOKIES" -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_A\",\"password\":\"$PW1\"}" \
        "$BASE/auth/login")
    check_contains "$_label: login post-verify ok" "$R" '"ok":true'
    # 4b. /_me reflects session
    R=$(curl -sS -b "$COOKIES" "$BASE/_me")
    check_contains "$_label: /_me reflects logged-in email" \
        "$R" "\"email\":\"$EMAIL_A\""

    # 5. Logout
    R=$(curl -sS -b "$COOKIES" -c "$COOKIES" -X POST "$BASE/auth/logout")
    check_contains "$_label: logout ok" "$R" '"ok":true'
    S=$(curl -sS -o /dev/null -w '%{http_code}' -b "$COOKIES" "$BASE/_me")
    check_status "$_label: /_me after logout is 401" "$S" "401"

    # 6. Password-reset request
    curl -sS -X POST "$BASE/_emails/clear" > /dev/null
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_A\"}" \
        "$BASE/auth/password-reset/request")
    check_contains "$_label: password-reset request ok" "$R" '"ok":true'
    TEXT=$(last_email_text "$PORT" "$EMAIL_A")
    RESET_URL=$(extract_url "$TEXT")
    check_contains "$_label: reset email contains reset link" \
        "$RESET_URL" "/auth/password-reset/confirm?token="

    # 7. Submit new password
    RESET_TOKEN=$(printf '%s\n' "$RESET_URL" | sed 's/.*token=//')
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"token\":\"$RESET_TOKEN\",\"password\":\"$PW2\"}" \
        "$BASE/auth/password-reset/confirm")
    check_contains "$_label: password-reset confirm ok" "$R" '"ok":true'

    # 8. Login with the NEW password
    R=$(curl -sS -c "$COOKIES" -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_A\",\"password\":\"$PW2\"}" \
        "$BASE/auth/login")
    check_contains "$_label: login with new password ok" "$R" '"ok":true'

    # 9. Replay reset token → reject
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"token\":\"$RESET_TOKEN\",\"password\":\"$PW1\"}" \
        "$BASE/auth/password-reset/confirm")
    check_contains "$_label: reset token replay rejected" "$R" '"error":'

    # 10. Magic-link request (logged in user, separate browser)
    curl -sS -X POST "$BASE/_emails/clear" > /dev/null
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_A\"}" \
        "$BASE/auth/magic-link")
    check_contains "$_label: magic-link request ok" "$R" '"ok":true'
    TEXT=$(last_email_text "$PORT" "$EMAIL_A")
    MAGIC_URL=$(extract_url "$TEXT")
    check_contains "$_label: magic-link email contains URL" \
        "$MAGIC_URL" "/auth/magic-link/consume?token="

    # 11. Click magic link in a FRESH cookie jar (simulate new device)
    : > "$COOKIES"
    S=$(curl -sS -o /dev/null -w '%{http_code}' -c "$COOKIES" "$MAGIC_URL")
    check_status "$_label: magic-link consume returns 200" "$S" "200"
    R=$(curl -sS -b "$COOKIES" "$BASE/_me")
    check_contains "$_label: magic-link grants session" \
        "$R" "\"email\":\"$EMAIL_A\""

    # 12. Email-change request (logged in via the magic-link session)
    curl -sS -X POST "$BASE/_emails/clear" > /dev/null
    R=$(curl -sS -b "$COOKIES" -X POST -H 'Content-Type: application/json' \
        -d "{\"new_email\":\"$EMAIL_B\"}" \
        "$BASE/auth/email-change")
    check_contains "$_label: email-change request ok" "$R" '"ok":true'
    TEXT=$(last_email_text "$PORT" "$EMAIL_B")
    EC_URL=$(extract_url "$TEXT")
    check_contains "$_label: email-change email sent to NEW address" \
        "$EC_URL" "/auth/email-change/confirm?token="

    # 13. Click email-change confirm
    S=$(curl -sS -o /dev/null -w '%{http_code}' "$EC_URL")
    check_status "$_label: email-change confirm 302" "$S" "302"

    # 14. Login with OLD email → fail
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_A\",\"password\":\"$PW2\"}" \
        "$BASE/auth/login")
    check_status "$_label: login with old email rejected" "$S" "401"

    # 15. Login with NEW email → ok
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_B\",\"password\":\"$PW2\"}" \
        "$BASE/auth/login")
    check_contains "$_label: login with new email ok" "$R" '"ok":true'

    # 16. Replay verify token from step 3 → reject
    S=$(curl -sS -o /dev/null -w '%{http_code}' "$VERIFY_URL")
    check_status "$_label: verify token replay rejected" "$S" "400"

    stop_pid "$HULL_PID"; HULL_PID=""
}

if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "lua" ]; then
    run_flow lua "$SRCDIR/tests/fixtures/auth_flows_lua/app.lua"
fi
if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "js" ]; then
    run_flow js "$SRCDIR/tests/fixtures/auth_flows_js/app.js"
fi

echo ""
echo "=== Summary ==="
echo "PASSED: $PASS"
echo "FAILED: $FAIL"

if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
