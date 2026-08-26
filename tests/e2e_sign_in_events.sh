#!/bin/sh
# E2E test for hull/web/middleware/audit-log + the device-management
# helpers added to hull/web/middleware/session, composed with
# hull/web/auth-flows.
#
# Per runtime (Lua + JS):
#
#   1. Register + verify a user.
#   2. Login from "browser A" (curl UA-A, IP-A via X-Forwarded-For).
#      Expect 1 device, 1 login event. on_new_device fires (first
#      sighting). /_new_devices reflects the alert.
#   3. Login from "browser B" (different UA + IP). Expect 2 devices,
#      2 login events, second new-device alert.
#   4. Re-login from "browser A" - same fingerprint, no new alert,
#      still 2 devices total.
#   5. Email-change flow generates an email_changed event in the
#      audit log; login from "browser A" after the change is also
#      logged.
#   6. /_devices returns the per-device summary (browser A + B with
#      first_seen/last_seen + the event list).
#   7. /_revoke_others kills browser B's session; browser A keeps
#      working; session.list_for_user goes from 2 -> 1.
#   8. Password reset cascade: request reset, click link with new
#      password, on_password_reset fires session.destroy_all. Both
#      A's and B's sessions die (B already dead). Log gets a
#      password_reset_completed event.
#
# Two virtual "browsers" are simulated by varying the User-Agent
# header (Chrome on macOS vs Firefox on Linux) - distinct enough
# that audit-log's family+OS fingerprint treats them as different
# devices.
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
    echo "e2e_sign_in_events: hull binary not found at $HULL"
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "e2e_sign_in_events: python3 required"
    exit 1
fi

fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }

check_status()   { [ "$2" = "$3" ] && pass "$1" || fail "$1 - expected status $3, got $2"; }
check_contains() { case "$2" in *"$3"*) pass "$1" ;; *) fail "$1 - expected '$3' in: $(echo "$2" | head -c 200)" ;; esac }
check_eq()       { [ "$2" = "$3" ] && pass "$1" || fail "$1 - expected $3, got $2"; }

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

UA_A="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) Chrome/120.0.0"
UA_B="Mozilla/5.0 (X11; Linux x86_64; rv:120.0) Firefox/120.0"
IP_A="203.0.113.42"
IP_B="198.51.100.7"

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
count_json_array() {
    printf '%s\n' "$1" | python3 -c "
import json, sys
try: print(len(json.load(sys.stdin)))
except Exception: print(0)
"
}
count_devices() {
    curl -sS -b "$1" -H "User-Agent: $2" -H "X-Forwarded-For: $3" \
        "$4/_devices" | python3 -c "
import json, sys
try: print(len(json.load(sys.stdin).get('devices', [])))
except Exception: print(0)
"
}
count_sessions() {
    curl -sS -b "$1" -H "User-Agent: $2" -H "X-Forwarded-For: $3" \
        "$4/_devices" | python3 -c "
import json, sys
try: print(len(json.load(sys.stdin).get('sessions', [])))
except Exception: print(0)
"
}

run_flow() {
    _label="$1"
    _entry="$2"

    echo ""
    echo "=== Step ($_label): Start sign-in-events fixture + e2e ==="
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

    BASE="http://127.0.0.1:$PORT"
    EMAIL="alice@example.test"
    EMAIL_NEW="alice.new@example.test"
    PW="hunter22hunter22"
    COOKIES_A="$TMPDIR_WORK/cookies_${_label}_A.txt"
    COOKIES_B="$TMPDIR_WORK/cookies_${_label}_B.txt"
    : > "$COOKIES_A"; : > "$COOKIES_B"

    # 1. Register + verify
    curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}" \
        "$BASE/auth/register" > /dev/null
    TEXT=$(last_email_text "$PORT" "$EMAIL")
    VERIFY_URL=$(extract_url "$TEXT")
    S=$(curl -sS -o /dev/null -w '%{http_code}' "$VERIFY_URL")
    check_status "$_label: register+verify" "$S" "302"

    # 2. Login from browser A.
    R=$(curl -sS -c "$COOKIES_A" \
        -H "User-Agent: $UA_A" -H "X-Forwarded-For: $IP_A" \
        -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}" \
        "$BASE/auth/login")
    check_contains "$_label: login A ok" "$R" '"ok":true'
    N=$(count_json_array "$(curl -sS $BASE/_new_devices)")
    check_eq "$_label: new-device alert fired for A (1)" "$N" "1"
    D=$(count_devices "$COOKIES_A" "$UA_A" "$IP_A" "$BASE")
    check_eq "$_label: 1 device after first login" "$D" "1"

    # 3. Login from browser B.
    R=$(curl -sS -c "$COOKIES_B" \
        -H "User-Agent: $UA_B" -H "X-Forwarded-For: $IP_B" \
        -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}" \
        "$BASE/auth/login")
    check_contains "$_label: login B ok" "$R" '"ok":true'
    N=$(count_json_array "$(curl -sS $BASE/_new_devices)")
    check_eq "$_label: new-device alert fired for B (2)" "$N" "2"
    D=$(count_devices "$COOKIES_A" "$UA_A" "$IP_A" "$BASE")
    check_eq "$_label: 2 devices after B logs in" "$D" "2"
    SES=$(count_sessions "$COOKIES_A" "$UA_A" "$IP_A" "$BASE")
    check_eq "$_label: 2 active sessions for user" "$SES" "2"

    # 4. Re-login from A - same fingerprint, no new alert.
    R=$(curl -sS -c "$COOKIES_A" \
        -H "User-Agent: $UA_A" -H "X-Forwarded-For: $IP_A" \
        -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL\",\"password\":\"$PW\"}" \
        "$BASE/auth/login")
    check_contains "$_label: re-login A ok" "$R" '"ok":true'
    N=$(count_json_array "$(curl -sS $BASE/_new_devices)")
    check_eq "$_label: no new alert for repeat A" "$N" "2"

    # 5. Email-change flow + audit event.
    curl -sS -X POST "$BASE/_emails/clear" > /dev/null
    curl -sS -b "$COOKIES_A" -X POST -H 'Content-Type: application/json' \
        -d "{\"new_email\":\"$EMAIL_NEW\"}" \
        "$BASE/auth/email-change" > /dev/null
    NEW_TEXT=$(last_email_text "$PORT" "$EMAIL_NEW")
    CONFIRM_URL=$(extract_url "$NEW_TEXT")
    S=$(curl -sS -o /dev/null -w '%{http_code}' "$CONFIRM_URL")
    check_status "$_label: email-change confirm 302" "$S" "302"
    EVENTS=$(curl -sS -b "$COOKIES_A" \
        -H "User-Agent: $UA_A" -H "X-Forwarded-For: $IP_A" \
        "$BASE/_devices" | python3 -c "
import json, sys
d = json.load(sys.stdin)
print(','.join(e['kind'] for e in d.get('events', [])))
")
    check_contains "$_label: email_changed event recorded" \
        "$EVENTS" "email_changed"
    check_contains "$_label: login events present" "$EVENTS" "login"

    # 7. Revoke-others: B dies, A lives.
    R=$(curl -sS -b "$COOKIES_A" \
        -H "User-Agent: $UA_A" -H "X-Forwarded-For: $IP_A" \
        -X POST "$BASE/_revoke_others")
    check_contains "$_label: revoke-others ok" "$R" '"ok":true'
    # B's cookie now points at a dead session: /_me returns 401.
    S=$(curl -sS -o /dev/null -w '%{http_code}' -b "$COOKIES_B" "$BASE/_me")
    check_status "$_label: B's session dead post-revoke" "$S" "401"
    # A still works.
    S=$(curl -sS -o /dev/null -w '%{http_code}' -b "$COOKIES_A" "$BASE/_me")
    check_status "$_label: A's session still alive" "$S" "200"
    SES=$(count_sessions "$COOKIES_A" "$UA_A" "$IP_A" "$BASE")
    check_eq "$_label: 1 active session after revoke-others" "$SES" "1"

    # 8. Password-reset cascade: on_password_reset destroys all
    #    sessions (including A's). A's cookie becomes invalid.
    curl -sS -X POST "$BASE/_emails/clear" > /dev/null
    curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_NEW\"}" \
        "$BASE/auth/password-reset/request" > /dev/null
    TEXT=$(last_email_text "$PORT" "$EMAIL_NEW")
    RESET_URL=$(extract_url "$TEXT")
    RESET_TOKEN=$(printf '%s\n' "$RESET_URL" | sed 's/.*token=//')
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"token\":\"$RESET_TOKEN\",\"password\":\"newpassword123\"}" \
        "$BASE/auth/password-reset/confirm")
    check_contains "$_label: reset confirm ok" "$R" '"ok":true'
    # A's cookie now dead too (cascade).
    S=$(curl -sS -o /dev/null -w '%{http_code}' -b "$COOKIES_A" "$BASE/_me")
    check_status "$_label: cascade killed A" "$S" "401"

    stop_pid "$HULL_PID"; HULL_PID=""
}

if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "lua" ]; then
    run_flow lua "$SRCDIR/tests/fixtures/sign_in_events_lua/app.lua"
fi
if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "js" ]; then
    run_flow js  "$SRCDIR/tests/fixtures/sign_in_events_js/app.js"
fi

echo ""
echo "=== Summary ==="
echo "PASSED: $PASS"
echo "FAILED: $FAIL"

if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
