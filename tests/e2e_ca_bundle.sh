#!/bin/sh
# E2E test for the embedded CA bundle.
#
# Verifies:
#   1. `hull doctor` reports CA bundle status (system + embedded)
#   2. `hull doctor --json` includes ca_bundle{system, embedded, embedded_label}
#   3. The embedded bundle test binary passes (signed PEM, parseable by mbedTLS)
#   4. A hull app can hit a real HTTPS endpoint using the embedded bundle
#      when the system CA store is unavailable
#
# Usage: sh tests/e2e_ca_bundle.sh
#        make e2e-ca-bundle
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HULL="$SRCDIR/build/hull"
PASS=0
FAIL=0
WORKDIR=""
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
    if [ -n "$WORKDIR" ] && [ -d "$WORKDIR" ]; then
        rm -rf "$WORKDIR"
    fi
}
trap cleanup EXIT INT TERM

# Wait up to 10s for $1 = port to accept connections.
wait_for_port() {
    port="$1"
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if curl -fsS --max-time 1 -o /dev/null "http://127.0.0.1:$port/health" 2>/dev/null \
           || nc -z 127.0.0.1 "$port" 2>/dev/null; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

assert() {
    msg="$1"; shift
    if "$@"; then
        echo "  ok  $msg"
        PASS=$((PASS + 1))
    else
        echo "  FAIL $msg"
        FAIL=$((FAIL + 1))
    fi
}

if [ ! -x "$HULL" ]; then
    echo "FAIL: $HULL not found - run 'make' first"
    exit 1
fi

echo "── hull doctor reports CA bundle ──"

DOCTOR_OUT=$("$HULL" doctor 2>&1 || true)
DOCTOR_JSON=$("$HULL" doctor --json 2>&1 || true)

echo "$DOCTOR_OUT" | grep -q "CA bundle"
assert "human output has 'CA bundle' section" [ $? -eq 0 ]

echo "$DOCTOR_JSON" | grep -q '"ca_bundle"'
assert "JSON output has ca_bundle field" [ $? -eq 0 ]

# sh_json emits compact JSON ("embedded":true, no space after colon)
# since the post-v0.2.0 doctor.c migration. Accept both forms so the
# test stays robust if formatting ever changes back.
echo "$DOCTOR_JSON" | grep -Eq '"embedded": ?true'
assert "JSON reports embedded: true (this build)" [ $? -eq 0 ]

echo ""
echo "── unit-test binary passes ──"

if [ -x "$SRCDIR/build/test_cacert" ]; then
    "$SRCDIR/build/test_cacert" >/dev/null 2>&1
    assert "test_cacert exits 0" [ $? -eq 0 ]
else
    echo "  skip test_cacert binary not built (run: make build/test_cacert)"
fi

echo ""
echo "── HTTPS over embedded bundle (real network) ──"

# Skip if no network access (CI sandboxes etc.).
if ! curl -fsSL --max-time 5 https://example.com >/dev/null 2>&1; then
    echo "  skip no network access to https://example.com - skipping live HTTPS test"
    echo ""
    echo "── Summary ──"
    echo "  Passed: $PASS"
    echo "  Failed: $FAIL"
    [ "$FAIL" -eq 0 ] && exit 0 || exit 1
fi

WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/hull_e2e_ca.XXXXXX")
mkdir -p "$WORKDIR/myapp"
cat > "$WORKDIR/myapp/app.lua" << 'EOF'
-- app.get is installed by "hull/http-server@1"; http.* by
-- "hull/http-client@1". Declare both in the manifest along with the
-- per-call host allowlist.
app.manifest({
    modules = { "hull/http-server@1", "hull/http-client@1" },
    hosts   = { "example.com" },
})

local http = require("hull.http-client")

app.get("/probe", function(req, res)
    -- A failed fetch returns nil (the reason is in the --audit record);
    -- pcall catches a raise so the reason reaches the test either way.
    local ok, resp = pcall(http.async.request, "GET", "https://example.com/")
    if not ok then
        res:status(502)
        res:json({ error = "raised: " .. tostring(resp) })
        return
    end
    if not resp or not resp.status then
        res:status(502)
        res:json({ error = "no response (see the http.fetch audit record)" })
        return
    end
    -- We don't care about the body content - just that the TLS handshake worked.
    res:json({ ok = true, status = resp.status, len = #(resp.body or "") })
end)
EOF

# Below 32768 (Linux client source ports are 32768-60999); RANDOM is not
# POSIX sh, the pid is.
PORT=$((31500 + $$ % 200))

# Hide the system bundle so hull is forced to use the embedded one.
# Easiest portable trick: pass an empty file as --ca-bundle override... no, we
# want the embedded path. Best is to clear by relying on the auto-detect
# missing system path. We can't really clear /etc/ssl in this test runner, so
# we use --ca-bundle override + a path to the vendored bundle as a sanity check
# AND we trust the unit test above for the actual embedded-only path.

cd "$WORKDIR/myapp"
"$HULL" "$WORKDIR/myapp/app.lua" -p "$PORT" -d "$WORKDIR/data.db" \
    --ca-bundle "$SRCDIR/vendor/cacert/cacert.pem" \
    > "$WORKDIR/server.log" 2>&1 &
SERVER_PID=$!
wait_for_port "$PORT" || echo "  (port $PORT never opened)"

BODY=$(curl -sS --max-time 30 "http://127.0.0.1:$PORT/probe" 2>&1 || echo "FAIL: curl exit $?")
if ! echo "$BODY" | grep -q '"ok":true'; then
    echo "  response: $BODY"
    echo "  --- server.log ---"
    sed 's/^/  | /' "$WORKDIR/server.log"
    echo "  --- end server.log ---"
fi
echo "$BODY" | grep -q '"ok":true'
assert "hull app fetched https://example.com via --ca-bundle" [ $? -eq 0 ]

# Confirm the log says we used the override path
grep -q "using CA bundle (override)" "$WORKDIR/server.log"
assert "log shows --ca-bundle override was honored" [ $? -eq 0 ]

kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

# Now do a second run with no override, no system fallback. We can't
# actually hide /etc/ssl on a real machine, but we CAN observe the log
# line that says "using CA bundle: /etc/ssl/...". If the system path is
# present, hull uses it; if not (e.g., a stripped-down container), hull
# uses the embedded one. Either case is correct; just confirm a TLS-capable
# context was created.

# --audit: a failed fetch's reason ("TLS handshake failed", a DNS or
# connect error) is only in its http.fetch audit record.
"$HULL" "$WORKDIR/myapp/app.lua" -p $((PORT + 1)) -d "$WORKDIR/data2.db" \
    --audit > "$WORKDIR/server2.log" 2>&1 &
SERVER_PID=$!
wait_for_port $((PORT + 1)) || echo "  (port $((PORT + 1)) never opened)"

BODY=$(curl -sS --max-time 30 "http://127.0.0.1:$((PORT + 1))/probe" 2>&1 || echo "FAIL: curl exit $?")
if echo "$BODY" | grep -q '"ok":true'; then
    assert "hull app fetched HTTPS via default CA resolution" true
else
    echo "  response: $BODY"
    echo "  --- server2.log ---"
    sed 's/^/  | /' "$WORKDIR/server2.log"
    echo "  --- end server2.log ---"
    # The probe depends on a third party: example.com's chain (Cloudflare,
    # anchored at "SSL.com TLS ECC Root CA 2022", cross-signed by Comodo
    # "AAA Certificate Services") must chain to a root in whichever store
    # the resolver picked. The vendored Mozilla bundle has it (the fetch
    # above passed); a host's system store need not. That is the host's
    # store, not Hull - but only when an independent verifier agrees. So:
    # a TLS handshake failure against a SYSTEM store that OpenSSL also
    # cannot verify example.com against (with -partial_chain, as lenient
    # as mbedTLS's trust of any listed CA) is a SKIP naming the store;
    # anything else - DNS, timeout, a 500, the embedded bundle, or a store
    # OpenSSL does verify against - stays a FAIL.
    SYS_STORE=$(sed -n 's/.*using CA bundle: \(.*\)$/\1/p' "$WORKDIR/server2.log" | head -n 1)
    TLS_FAIL=0
    grep -q '"cap":"http.fetch".*"error":"TLS handshake failed"' "$WORKDIR/server2.log" && TLS_FAIL=1
    STORE_VERIFIES=unknown
    # The verifier must be real OpenSSL: Apple's LibreSSL openssl (and
    # Apple's curl) also trust the default store / keychain whatever
    # -CAfile says, so they would vouch for any file.
    OSSL=""
    for c in openssl /opt/homebrew/opt/openssl@3/bin/openssl \
             /usr/local/opt/openssl@3/bin/openssl; do
        if "$c" version 2>/dev/null | grep -q '^OpenSSL '; then OSSL="$c"; break; fi
    done
    if [ -n "$SYS_STORE" ] && [ -r "$SYS_STORE" ] && [ -n "$OSSL" ]; then
        # Only a "Verify return code:" line decides: a connect failure
        # leaves the answer unknown.
        # -no-CAstore is OpenSSL 3; 1.1.1 knows only -no-CApath.
        echo | "$OSSL" s_client -connect example.com:443 -servername example.com \
            -CAfile "$SYS_STORE" -no-CApath -no-CAstore -partial_chain >"$WORKDIR/openssl.log" 2>&1
        if grep -qi "unknown option\|usage" "$WORKDIR/openssl.log"; then
            echo | "$OSSL" s_client -connect example.com:443 -servername example.com \
                -CAfile "$SYS_STORE" -no-CApath -partial_chain >"$WORKDIR/openssl.log" 2>&1
        fi
        VRC=$(sed -n 's/.*Verify return code: \([0-9][0-9]*\).*/\1/p' "$WORKDIR/openssl.log" | tail -n 1)
        if [ "$VRC" = 0 ]; then
            STORE_VERIFIES=yes
        elif [ -n "$VRC" ]; then
            STORE_VERIFIES=no
        fi
        echo "  --- $("$OSSL" version) s_client -CAfile $SYS_STORE (verify: $STORE_VERIFIES) ---"
        if [ "$STORE_VERIFIES" = unknown ]; then
            tail -n 15 "$WORKDIR/openssl.log" | sed 's/^/  | /'
        else
            grep -E '^ *[0-9]+ s:|^ *i:|[Vv]erif' "$WORKDIR/openssl.log" | sed 's/^/  | /'
        fi
        for r in "SSL.com TLS ECC Root CA 2022" "AAA Certificate Services"; do
            if grep -q "$r" "$SYS_STORE"; then echo "  | $SYS_STORE has: $r"
            else echo "  | $SYS_STORE lacks: $r"; fi
        done
    elif [ -n "$SYS_STORE" ]; then
        echo "  (no OpenSSL to check $SYS_STORE independently - not skipping)"
    fi
    if [ "$TLS_FAIL" -eq 1 ] && [ "$STORE_VERIFIES" = no ]; then
        echo "  skip default CA resolution picked the system store $SYS_STORE, which"
        echo "       cannot verify example.com's current chain (OpenSSL agrees);"
        echo "       the same fetch verified against the vendored bundle above"
    else
        assert "hull app fetched HTTPS via default CA resolution" false
    fi
fi

# At least one of system / embedded must have been chosen
grep -qE "using CA bundle|using embedded CA bundle" "$WORKDIR/server2.log"
assert "log shows a CA bundle source was used" [ $? -eq 0 ]

kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

echo ""
echo "── Summary ──"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
