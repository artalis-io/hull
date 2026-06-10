#!/bin/sh
# E2E test for hull/web/middleware/oauth.
#
# Architecture:
#   1. Generate fresh test RSA keypair + X.509 cert (or use committed fixtures)
#   2. Start a Python mock OIDC IdP on a random port
#   3. Start a hull dev server with the OIDC client fixture pointed at the IdP
#   4. Walk through the full Authorization Code + PKCE flow with curl:
#        /auth/test/login        -> 302 to IdP /authorize
#        /authorize              -> 302 back to /auth/test/callback?code=...
#        /auth/test/callback     -> token exchange + JWKS verify + on_login
#        /me                     -> assert claims made it through
#   5. Tear down both servers
#
# Tests both Lua and JS clients by default.
#
# Usage: sh tests/e2e_oauth.sh
#        RUNTIME=lua sh tests/e2e_oauth.sh
#        RUNTIME=js  sh tests/e2e_oauth.sh
#
# Requires: build/hull built, curl, python3, openssl in PATH.
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
IDP_DIR="$SRCDIR/tests/fixtures/oauth_idp"
PASS=0
FAIL=0
RUNTIME=${RUNTIME:-all}
IDP_PID=""
HULL_PID=""
TMPDIR_WORK=""

if [ ! -x "$HULL" ]; then
    echo "e2e_oauth: hull binary not found at $HULL — run 'make' first"
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "e2e_oauth: python3 required"
    exit 1
fi
if ! command -v openssl >/dev/null 2>&1; then
    echo "e2e_oauth: openssl required"
    exit 1
fi

fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }

check_status() {
    # $1 = description, $2 = actual status, $3 = expected status
    if [ "$2" = "$3" ]; then pass "$1"
    else fail "$1 — expected status $3, got $2"
    fi
}

check_contains() {
    # $1 = description, $2 = haystack, $3 = needle
    case "$2" in
        *"$3"*) pass "$1" ;;
        *)      fail "$1 — expected '$3' in: $(echo "$2" | head -c 200)" ;;
    esac
}

wait_for_server() {
    # $1 = port, $2 = path (default /health)
    _path="${2:-/health}"
    for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if curl -fs "http://127.0.0.1:$1${_path}" >/dev/null 2>&1; then
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
    stop_pid "$IDP_PID"
    if [ -n "$TMPDIR_WORK" ] && [ -d "$TMPDIR_WORK" ]; then
        rm -rf "$TMPDIR_WORK"
    fi
}
trap cleanup EXIT

TMPDIR_WORK=$(mktemp -d)

# ── Step 1: Start the mock IdP ────────────────────────────────────────
echo ""
echo "=== Step 1: Start mock OIDC IdP ==="

IDP_PORT=$(python3 -c 'import socket,sys
s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
IDP_ISSUER="http://127.0.0.1:$IDP_PORT"

MOCK_IDP_PORT="$IDP_PORT" MOCK_IDP_ISSUER="$IDP_ISSUER" \
  python3 "$IDP_DIR/mock_idp.py" >"$TMPDIR_WORK/idp.log" 2>&1 &
IDP_PID=$!

if ! wait_for_server "$IDP_PORT" "/health"; then
    fail "mock IdP did not start"
    cat "$TMPDIR_WORK/idp.log" || true
    exit 1
fi
pass "mock IdP started on $IDP_ISSUER (pid $IDP_PID)"

# Verify JWKS endpoint returns a parseable doc.
JWKS=$(curl -fs "$IDP_ISSUER/.well-known/jwks.json")
check_contains "JWKS has keys array" "$JWKS" '"keys"'
check_contains "JWKS has x5c"        "$JWKS" '"x5c"'
check_contains "JWKS alg RS256"      "$JWKS" '"RS256"'

# ── Step 2: Run the flow against each runtime ─────────────────────────
run_flow() {
    # $1 = runtime label (lua|js)
    # $2 = source fixture dir (with the template app.{lua,js})
    # $3 = entry filename (app.lua | app.js)
    _label="$1"
    _src="$2"
    _entry_name="$3"

    echo ""
    echo "=== Step 2 ($_label): Materialize fixture + start hull dev ==="

    # Copy the fixture template into a tmpdir and substitute the
    # IdP issuer + state secret. The fixture's top-level can't read
    # env at load time (env_cfg is wired AFTER manifest extraction),
    # so we bake the values into a per-run copy of the app file.
    _stage="$TMPDIR_WORK/fixture_$_label"
    mkdir -p "$_stage"
    cp "$_src/$_entry_name" "$_stage/$_entry_name.tmpl"
    # Use a separator unlikely to appear in URLs / secrets.
    sed -e "s|__MOCK_IDP_ISSUER__|$IDP_ISSUER|g" \
        -e "s|__OAUTH_STATE_SECRET__|0123456789abcdefghij-test-state-secret|g" \
        "$_stage/$_entry_name.tmpl" > "$_stage/$_entry_name"
    rm "$_stage/$_entry_name.tmpl"

    CLIENT_PORT=$(python3 -c 'import socket,sys
s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')

    HULL_LOG="$TMPDIR_WORK/hull_$_label.log"
    "$HULL" dev "$_stage/$_entry_name" -p "$CLIENT_PORT" >"$HULL_LOG" 2>&1 &
    HULL_PID=$!

    if ! wait_for_server "$CLIENT_PORT" "/"; then
        fail "$_label: hull dev did not start on port $CLIENT_PORT"
        cat "$HULL_LOG" || true
        return
    fi
    pass "$_label: hull dev started on :$CLIENT_PORT (pid $HULL_PID)"

    # 1. GET /auth/test/login — expect 302 to IdP authorize with PKCE.
    COOKIES="$TMPDIR_WORK/cookies_$_label.txt"
    : > "$COOKIES"
    LOGIN_RESP=$(curl -sS -i -c "$COOKIES" \
        "http://127.0.0.1:$CLIENT_PORT/auth/test/login")
    LOGIN_STATUS=$(echo "$LOGIN_RESP" | awk 'NR==1{print $2}')
    check_status "$_label: /auth/test/login is 302" "$LOGIN_STATUS" "302"
    LOCATION=$(echo "$LOGIN_RESP" \
        | awk 'BEGIN{IGNORECASE=1} /^[Ll]ocation:/{
            sub(/^[Ll]ocation: */, ""); sub(/\r$/, ""); print; exit}')
    check_contains "$_label: location goes to IdP authorize" "$LOCATION" "$IDP_ISSUER/authorize"
    check_contains "$_label: location has code_challenge"    "$LOCATION" "code_challenge="
    check_contains "$_label: location has PKCE S256"          "$LOCATION" "code_challenge_method=S256"
    check_contains "$_label: location has state"              "$LOCATION" "state="
    check_contains "$_label: location has nonce"              "$LOCATION" "nonce="
    # Cookie should be set on /login.
    grep -q "_oauth_state" "$COOKIES" && pass "$_label: state cookie set" \
        || fail "$_label: state cookie missing"

    # 2. Follow the location to IdP. Don't carry the state cookie -
    #    the IdP is a different origin (different port/host doesn't
    #    matter to curl but conceptually it's external).
    AUTH_RESP=$(curl -sS -i "$LOCATION")
    AUTH_STATUS=$(echo "$AUTH_RESP" | awk 'NR==1{print $2}')
    check_status "$_label: IdP /authorize is 302" "$AUTH_STATUS" "302"
    CALLBACK_URL=$(echo "$AUTH_RESP" \
        | awk 'BEGIN{IGNORECASE=1} /^[Ll]ocation:/{
            sub(/^[Ll]ocation: */, ""); sub(/\r$/, ""); print; exit}')
    check_contains "$_label: callback URL points to client" "$CALLBACK_URL" "/auth/test/callback"
    check_contains "$_label: callback URL has code"          "$CALLBACK_URL" "code="
    check_contains "$_label: callback URL has state"         "$CALLBACK_URL" "state="

    # 3. Hit the callback, carrying the state cookie. Should redirect to /me.
    CB_RESP=$(curl -sS -i -b "$COOKIES" -c "$COOKIES" "$CALLBACK_URL")
    CB_STATUS=$(echo "$CB_RESP" | awk 'NR==1{print $2}')
    check_status "$_label: /callback is 302" "$CB_STATUS" "302"
    CB_LOC=$(echo "$CB_RESP" \
        | awk 'BEGIN{IGNORECASE=1} /^[Ll]ocation:/{
            sub(/^[Ll]ocation: */, ""); sub(/\r$/, ""); print; exit}')
    check_contains "$_label: callback redirects to /me" "$CB_LOC" "/me"

    # 4. /me should now show the verified claims.
    ME_RESP=$(curl -sS -b "$COOKIES" "http://127.0.0.1:$CLIENT_PORT/me")
    check_contains "$_label: /me has sub"   "$ME_RESP" '"sub":"user-123"'
    check_contains "$_label: /me has email" "$ME_RESP" '"email":"alice@example.test"'
    check_contains "$_label: /me has name"  "$ME_RESP" '"name":"Alice Example"'
    check_contains "$_label: /me records provider" "$ME_RESP" '"provider":"test"'

    # 5. Negative: tampered state cookie — flip a char in the body
    # part of the cookie so the HMAC tag no longer matches. Should
    # get a 400 from the callback.
    TAMPER_COOKIES="$TMPDIR_WORK/cookies_tamper_$_label.txt"
    awk '
        $6 == "_oauth_state" {
            # Domain Flag Path Secure Expires Name Value
            v=$7; if (substr(v,1,1)=="a") sub(/^./,"b",v); else sub(/^./,"a",v); $7=v
        }
        { print }
    ' OFS="\t" "$COOKIES" > "$TAMPER_COOKIES"
    TAMPER_RESP=$(curl -sS -i -b "$TAMPER_COOKIES" "$CALLBACK_URL")
    TAMPER_STATUS=$(echo "$TAMPER_RESP" | awk 'NR==1{print $2}')
    check_status "$_label: tampered cookie rejected" "$TAMPER_STATUS" "400"

    # 6. Logout clears the state cookie and redirects.
    LO_RESP=$(curl -sS -i -b "$COOKIES" \
        "http://127.0.0.1:$CLIENT_PORT/auth/logout")
    LO_STATUS=$(echo "$LO_RESP" | awk 'NR==1{print $2}')
    check_status "$_label: /auth/logout is 302" "$LO_STATUS" "302"

    stop_pid "$HULL_PID"; HULL_PID=""
}

if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "lua" ]; then
    run_flow lua "$SRCDIR/tests/fixtures/oauth_client_lua" "app.lua"
fi
if [ "$RUNTIME" = "all" ] || [ "$RUNTIME" = "js" ]; then
    run_flow js "$SRCDIR/tests/fixtures/oauth_client_js" "app.js"
fi

echo ""
echo "=== Summary ==="
echo "PASSED: $PASS"
echo "FAILED: $FAIL"

if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
