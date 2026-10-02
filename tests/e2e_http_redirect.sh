#!/bin/sh
# e2e_http_redirect.sh: a redirect cannot take http.fetch past manifest.hosts.
#
# The host allowlist used to be checked against the first URL only; the
# redirect chain followed every Location after it, so an allowed host could
# send http.fetch to a cloud metadata endpoint or an internal service and the
# body came back to the app. Here the app may reach 127.0.0.1 only. Its own
# /redir route redirects to http://localhost:<port>/target - the same server,
# under a name the allowlist does not hold - so the target's body reaching
# the app means the second hop was not checked. A redirect to an ALLOWED host
# must still be followed. The async client is used: a blocking request to
# the same server would stall the loop that has to answer it. The sync client
# takes the same hook (test_http.c covers the check itself).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PORT="${PORT:-19893}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

TMPDIR=$(mktemp -d)
SERVER_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

cat > "$TMPDIR/app.lua" <<EOF
app.manifest({
    modules = { "hull/http-server@1", "hull/http-client@1" },
    hosts = { "127.0.0.1" },
})
local http = require("hull.http-client")
local BASE = "http://127.0.0.1:$PORT"

app.get("/target", function(_req, res) res:text("secret") end)
app.get("/redir-out", function(_req, res)
    res:redirect("http://localhost:$PORT/target", 302)
end)
app.get("/redir-in", function(_req, res) res:redirect(BASE .. "/target", 302) end)

-- A failed fetch returns nil plus an error (or raises): either is "error=".
local function report(res, ok, r, e)
    if not ok then return res:text("error=" .. tostring(r)) end
    if not r then return res:text("error=" .. tostring(e)) end
    res:text("status=" .. tostring(r.status) .. " body=" .. tostring(r.body))
end
app.get("/async-out", function(_req, res)
    report(res, pcall(http.async.get, BASE .. "/redir-out"))
end)
app.get("/async-in", function(_req, res)
    report(res, pcall(http.async.get, BASE .. "/redir-in"))
end)
EOF

"$HULL" --no-sandbox -p "$PORT" "$TMPDIR/app.lua" >"$TMPDIR/log" 2>&1 &
SERVER_PID=$!
up=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if curl -s -o /dev/null "http://127.0.0.1:$PORT/target"; then up=1; break; fi
    sleep 0.5
done
[ "$up" = 1 ] || { echo "FAIL: server did not start"; cat "$TMPDIR/log"; exit 1; }

echo "=== http.fetch redirects ==="
out=$(curl -s "http://127.0.0.1:$PORT/async-out")
case "$out" in
    *secret*) fail "a redirect to a host outside manifest.hosts is not followed" "$out" ;;
    *error=*) pass "a redirect to a host outside manifest.hosts is not followed" ;;
    *)        fail "a redirect to a host outside manifest.hosts is not followed" "$out" ;;
esac
grep -q "redirect to a host outside manifest.hosts refused" "$TMPDIR/log" \
    && pass "the refusal is logged" \
    || fail "the refusal is logged" "$(tail -3 "$TMPDIR/log")"

out=$(curl -s "http://127.0.0.1:$PORT/async-in")
case "$out" in
    *"body=secret"*) pass "a redirect to an allowed host is still followed (async)" ;;
    *) fail "a redirect to an allowed host is still followed (async)" "$out" ;;
esac

echo ""
echo "e2e_http_redirect: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
