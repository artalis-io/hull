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

# POST /auth/verify {token, <field>: <password>} for the token in a verify
# URL (audit 5: GET never consumes; the POST does). Echoes the response
# body followed by a line holding the HTTP status.
verify_post() {
    _base="$1"; _url="$2"; _field="$3"; _pw="$4"
    _tok=$(printf '%s\n' "$_url" | sed 's/.*token=//')
    curl -sS -w '\n%{http_code}' -X POST -H 'Content-Type: application/json' \
        -d "{\"token\":\"$_tok\",\"$_field\":\"$_pw\"}" \
        "$_base/auth/verify"
}
# The status line a `curl -w '\n%{http_code}'` appended / the body before it.
resp_status() { printf '%s\n' "$1" | tail -n 1; }
resp_body()   { printf '%s\n' "$1" | sed '$d'; }

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
# A mailed single-use link (magic link, email-change confirm / revoke): its GET
# shows a page and consumes nothing (mail scanners prefetch links); the page's
# form POSTs the token back, from the app's own page (its Origin: auth-flows
# refuses a cross-site POST - login CSRF). Extra curl options go after the URL.
link_post() {
    _lurl="$1"; shift
    _ltok=$(printf '%s\n' "$_lurl" | sed 's/.*token=//')
    curl -sS "$@" -X POST -H "Origin: ${_lurl%%/auth/*}" \
        -H 'Content-Type: application/x-www-form-urlencoded' \
        --data "token=$_ltok" "${_lurl%%\?*}"
}
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

    # 1. Register.
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

    # 3. Verify (audit 5). A GET - a mail scanner prefetching the link -
    #    renders the form and consumes nothing; a wrong password is a 401
    #    that leaves the token usable; the right one verifies and keeps it.
    R=$(curl -sS -w '\n%{http_code}' "$VERIFY_URL")
    check_status "$_label: verify GET renders the form" "$(resp_status "$R")" "200"
    check_contains "$_label: verify form offers new_password" \
        "$(resp_body "$R")" 'name="new_password"'
    R=$(curl -sS -w '\n%{http_code}' "$VERIFY_URL")
    check_status "$_label: scanner GET did not consume the token" "$(resp_status "$R")" "200"
    R=$(verify_post "$BASE" "$VERIFY_URL" password "wrongpassword99")
    check_status "$_label: verify with wrong password is 401" "$(resp_status "$R")" "401"
    check_contains "$_label: 401 offers new_password" "$(resp_body "$R")" 'new_password'
    R=$(verify_post "$BASE" "$VERIFY_URL" password "$PW1")
    check_status "$_label: token still usable after 401; verify ok" "$(resp_status "$R")" "200"

    # 4. Login after verify
    R=$(curl -sS -c "$COOKIES" -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_A\",\"password\":\"$PW1\"}" \
        "$BASE/auth/login")
    check_contains "$_label: login post-verify ok" "$R" '"ok":true'
    # 4b. /_me reflects session
    R=$(curl -sS -b "$COOKIES" "$BASE/_me")
    check_contains "$_label: /_me reflects logged-in email" \
        "$R" "\"email\":\"$EMAIL_A\""

    # 4c. Login CSRF (audit 8): a form POSTed to /login from another site
    #     would sign the victim in to the attacker's account. Refused when
    #     Sec-Fetch-Site says cross-site or same-site, when there is no
    #     provenance header (a form, not JSON), and for a foreign Origin;
    #     the app's own form (its Origin) signs in.
    _form="email=$EMAIL_A&password=$PW1"
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST -H 'Sec-Fetch-Site: cross-site' \
        -H 'Content-Type: application/x-www-form-urlencoded' --data "$_form" "$BASE/auth/login")
    check_status "$_label: cross-site login form refused" "$S" "403"
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST -H 'Sec-Fetch-Site: same-site' \
        -H 'Content-Type: application/x-www-form-urlencoded' --data "$_form" "$BASE/auth/login")
    check_status "$_label: same-site login form refused" "$S" "403"
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
        -H 'Content-Type: application/x-www-form-urlencoded' --data "$_form" "$BASE/auth/login")
    check_status "$_label: login form without Origin refused" "$S" "403"
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST -H 'Origin: https://evil.example' \
        -H 'Content-Type: application/x-www-form-urlencoded' --data "$_form" "$BASE/auth/login")
    check_status "$_label: login form from a foreign Origin refused" "$S" "403"
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: text/plain; x=application/json' \
        --data "{\"email\":\"$EMAIL_A\",\"password\":\"$PW1\"}" "$BASE/auth/login")
    check_status "$_label: text/plain 'JSON' login refused" "$S" "403"
    R=$(curl -sS -X POST -H "Origin: $BASE" \
        -H 'Content-Type: application/x-www-form-urlencoded' --data "$_form" "$BASE/auth/login")
    check_contains "$_label: same-origin login form ok" "$R" '"ok":true'

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

    # 11. Click magic link in a FRESH cookie jar (simulate new device). The
    #     GET (a mail scanner's prefetch) renders a sign-in form and signs
    #     nobody in; its POST does, once. A cross-site POST is refused.
    : > "$COOKIES"
    R=$(curl -sS -c "$COOKIES" "$MAGIC_URL")
    check_contains "$_label: magic-link GET renders a sign-in form" \
        "$R" 'action="/auth/magic-link/consume"'
    S=$(curl -sS -o /dev/null -w '%{http_code}' -b "$COOKIES" "$BASE/_me")
    check_status "$_label: magic-link GET does not sign in" "$S" "401"
    S=$(link_post "$MAGIC_URL" -o /dev/null -w '%{http_code}' \
        -H 'Sec-Fetch-Site: cross-site')
    check_status "$_label: cross-site magic-link POST refused" "$S" "403"
    S=$(link_post "$MAGIC_URL" -o /dev/null -w '%{http_code}' -c "$COOKIES")
    check_status "$_label: magic-link consume returns 200" "$S" "200"
    R=$(curl -sS -b "$COOKIES" "$BASE/_me")
    check_contains "$_label: magic-link grants session" \
        "$R" "\"email\":\"$EMAIL_A\""
    S=$(link_post "$MAGIC_URL" -o /dev/null -w '%{http_code}')
    check_status "$_label: magic-link replay rejected" "$S" "400"

    # 12. Email-change request (logged in via the magic-link session)
    curl -sS -X POST "$BASE/_emails/clear" > /dev/null
    # The session alone does not move the account (audit 8): the current
    # password is required, and a wrong one refused.
    S=$(curl -sS -o /dev/null -w '%{http_code}' -b "$COOKIES" -X POST \
        -H 'Content-Type: application/json' -d "{\"new_email\":\"$EMAIL_B\"}" \
        "$BASE/auth/email-change")
    check_status "$_label: email-change without the password refused" "$S" "401"
    S=$(curl -sS -o /dev/null -w '%{http_code}' -b "$COOKIES" -X POST \
        -H 'Content-Type: application/json' \
        -d "{\"new_email\":\"$EMAIL_B\",\"password\":\"wrongpassword99\"}" \
        "$BASE/auth/email-change")
    check_status "$_label: email-change with a wrong password refused" "$S" "401"
    R=$(curl -sS -b "$COOKIES" -X POST -H 'Content-Type: application/json' \
        -d "{\"new_email\":\"$EMAIL_B\",\"password\":\"$PW2\"}" \
        "$BASE/auth/email-change")
    check_contains "$_label: email-change request ok" "$R" '"ok":true'
    TEXT=$(last_email_text "$PORT" "$EMAIL_B")
    EC_URL=$(extract_url "$TEXT")
    check_contains "$_label: email-change email sent to NEW address" \
        "$EC_URL" "/auth/email-change/confirm?token="

    # 13. Click email-change confirm: the GET only shows the form.
    R=$(curl -sS "$EC_URL")
    check_contains "$_label: email-change GET renders a confirm form" \
        "$R" 'action="/auth/email-change/confirm"'
    S=$(link_post "$EC_URL" -o /dev/null -w '%{http_code}')
    check_status "$_label: email-change confirm 303" "$S" "303"

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

    # 16. Replay verify token from step 3 → reject (GET and POST)
    S=$(curl -sS -o /dev/null -w '%{http_code}' "$VERIFY_URL")
    check_status "$_label: verify token replay rejected (GET)" "$S" "400"
    R=$(verify_post "$BASE" "$VERIFY_URL" password "$PW2")
    check_status "$_label: verify token replay rejected (POST)" "$(resp_status "$R")" "400"

    # 17. Pre-registration hijack: someone registers the owner's address
    #     with a password of their own. The owner, who does not know it,
    #     verifies by setting a new password: the registrant's password no
    #     longer logs in, the owner's does.
    EMAIL_C="carol@example.test"
    PW_C="carolsownpassword1"
    curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_C\",\"password\":\"$PW1\"}" \
        "$BASE/auth/register" > /dev/null
    TEXT=$(last_email_text "$PORT" "$EMAIL_C")
    HIJACK_URL=$(extract_url "$TEXT")
    R=$(verify_post "$BASE" "$HIJACK_URL" new_password "$PW_C")
    check_status "$_label: verify with new_password ok" "$(resp_status "$R")" "200"
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_C\",\"password\":\"$PW1\"}" \
        "$BASE/auth/login")
    check_status "$_label: pre-registrant password replaced" "$S" "401"
    R=$(curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_C\",\"password\":\"$PW_C\"}" \
        "$BASE/auth/login")
    check_contains "$_label: owner logs in with the new password" "$R" '"ok":true'

    # 18. A magic link to an unverified account that has a password goes
    #     through the same verify step instead of signing in.
    EMAIL_D="dave@example.test"
    curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_D\",\"password\":\"$PW1\"}" \
        "$BASE/auth/register" > /dev/null
    curl -sS -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_D\"}" "$BASE/auth/magic-link" > /dev/null
    TEXT=$(last_email_text "$PORT" "$EMAIL_D")
    DMAGIC_URL=$(extract_url "$TEXT")
    : > "$COOKIES"
    R=$(link_post "$DMAGIC_URL" -c "$COOKIES")
    check_contains "$_label: magic link to unverified account renders verify form" \
        "$R" 'name="new_password"'
    S=$(curl -sS -o /dev/null -w '%{http_code}' -b "$COOKIES" "$BASE/_me")
    check_status "$_label: ...and does not sign in" "$S" "401"
    # (The fixture's user_get omits password_hash, so step 18 also proves the
    # gate reads the hash through user_find_by_email - audit 6 M1 - and its
    # email_verified is a raw 0 / 1, which Lua used to read as true. The JS
    # fixture's is a string "0", which JS used to read as true - audit 7.)

    # 18b. The deferred mail is sent AFTER the response (audit 8): with a
    #      magic-link template that takes 300 ms, the request for an existing
    #      account still answers at once (JS attached its timer to the
    #      request, so the response waited for the render - only for
    #      existing accounts), and the mail still goes out.
    curl -sS -X POST "$BASE/_slow_templates" > /dev/null
    curl -sS -X POST "$BASE/_emails/clear" > /dev/null
    T=$(curl -sS -o /dev/null -w '%{time_total}' -X POST -H 'Content-Type: application/json' \
        -d "{\"email\":\"$EMAIL_C\"}" "$BASE/auth/magic-link")
    if python3 -c "import sys; sys.exit(0 if float('$T') < 0.2 else 1)"; then
        pass "$_label: magic-link answers before the (slow) mail render"
    else
        fail "$_label: magic-link waited for the mail render (${T}s)"
    fi
    sleep 0.8
    TEXT=$(last_email_text "$PORT" "$EMAIL_C")
    check_contains "$_label: ...and the mail is sent after it" \
        "$TEXT" "/auth/magic-link/consume?token="

    # 19. Audit 6: logout refuses a cross-site POST (a forged form would
    #     still sign the victim out through the clearing Set-Cookie), and
    #     init refuses require_verified_email = false without
    #     on_password_reset.
    S=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
        -H 'Sec-Fetch-Site: cross-site' "$BASE/auth/logout")
    check_status "$_label: cross-site logout refused" "$S" "403"
    R=$(curl -sS "$BASE/_init_refuses_unverified_login")
    check_contains "$_label: unverified login without on_password_reset refused" \
        "$R" '"refused":true'

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
