#!/bin/sh
# e2e_refusal_codes.sh: the runtime's refusals carry a code, and the stdlib
# reads the code instead of the message (audit 35).
#
# Audit 31 capped each VM at 64 in-flight fetches; past it http.fetch refuses
# ("too many requests in flight"). The stdlib recognised only the older
# refusal messages, so this one read as a remote failure:
#   - hull/web/pwned failed OPEN ("not pwned"): a client holding 64 slow
#     fetches made register / reset / password change accept breached
#     passwords;
#   - hull/web/middleware/outbox counted a failed delivery for every claimed
#     row: after max_attempts the rows were marked failed, never sent.
# A refusal now carries a code (JS `e.code`, Lua a "[hull:<code>] " prefix):
# "busy" for an admission cap, "wait_refused" for a wait the runtime refuses.
#
# For each runtime, with 64 fetches held open on a slow endpoint:
#   - pwned.check raises the busy refusal (the app answers 503 with its
#     code), where it used to answer "not pwned";
#   - outbox.flush reports busy and leaves every row pending (max_attempts
#     = 1, so a counted attempt would have marked it failed);
# and once the held fetches finish, the same flush delivers every row.
#
# Audit 39 legs, same setup:
#   - email.send through an HTTP provider (postmark) re-raises the busy
#     refusal with its code (it was wrapped as delivery_failed);
#   - retry.run re-raises it unchanged, after one call (Lua: it gained a
#     "hull.retry:N: " prefix that hid the code);
#   - a workflow whose step hits the busy refusal on its LAST attempt is
#     handed back without running its compensation, and once the cap frees
#     it completes with nothing compensated (round 38 compensated it, then
#     the hand-back replayed the undone step's memo to 'done').
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PORT="${PORT:-19914}"
MOCK_PORT="${MOCK_PORT:-19915}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: e2e_refusal_codes needs python3 for the mock server"
    exit 0
fi

HULL=$(cd "$(dirname "$HULL")" && pwd)/$(basename "$HULL")
TMPDIR=$(mktemp -d)
SERVER_PID=""
MOCK_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# /hold answers after 4 s; /range/<prefix> answers at once with a suffix
# that matches nothing (a real "not pwned"); POST (an outbox delivery)
# answers 200 at once.
cat > "$TMPDIR/mock.py" <<'EOF'
import sys, time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
class H(BaseHTTPRequestHandler):
    def log_message(self, *a, **k): pass
    def reply(self, body):
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        if self.path.startswith("/hold"):
            time.sleep(4)
        self.reply(b"00000000000000000000000000000000000:1\r\n")
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(n)
        self.reply(b"ok")
ThreadingHTTPServer.request_queue_size = 256
ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
EOF
python3 -I "$TMPDIR/mock.py" "$MOCK_PORT" &
MOCK_PID=$!

M="http://127.0.0.1:$MOCK_PORT"

mkdir -p "$TMPDIR/lua" "$TMPDIR/js"
cat > "$TMPDIR/lua/app.lua" <<EOF
local app = require("hull.app")
app.manifest({
    modules = { "hull/http-server@1", "hull/http-client@1", "hull/web/pwned@1",
                "hull/web/middleware/outbox@1", "hull/json@1", "hull/email@1",
                "hull/retry@1", "hull/jobs@1" },
    hosts = { "127.0.0.1", "api.postmarkapp.com" },
})
local http = require("hull.http-client")
local pwned = require("hull.web.pwned")
local outbox = require("hull.web.middleware.outbox")
outbox.init({ max_attempts = 1 })
local email = require("hull.email")
local retry = require("hull.retry")
local jobs = require("hull.jobs")
local function rcode(e)
    if type(e) == "table" then return e.code or "none" end
    return tostring(e):match("^%[hull:([%w_]+)%] ") or "none"
end
local inited, wf_id, refunded = false, nil, 0
local function jinit()
    if not inited then jobs.init({ backoff = function() return 0 end }); inited = true end
end
jobs.workflow("order", function(c)
    c.step("charge", function() return "ch_1" end,
           { compensate = function() refunded = refunded + 1 end })
    c.step("ship", function() http.async.get("$M/ship"); return "shipped" end)
    return "ok"
end)
app.get("/email", function(req, res)
    local ok, e = pcall(email.send, { provider = "postmark", api_key = "k",
        from = "a@b.co", to = "c@d.co", subject = "s", body = "b" })
    res:json({ code = ok and "sent" or rcode(e) })
end)
app.get("/retry", function(req, res)
    local calls = 0
    local ok, e = pcall(retry.run, function()
        calls = calls + 1
        return http.async.get("$M/r")
    end, { max_attempts = 3 })
    res:json({ code = ok and "ok" or rcode(e), calls = calls })
end)
app.get("/wfstart", function(req, res)
    jinit()
    wf_id = jobs.start("order", {}, { max_attempts = 1 })
    jobs.work({})
    local j = jobs.get(wf_id)
    res:json({ status = j.status, attempts = j.attempts, refunded = refunded })
end)
app.get("/wfwork", function(req, res)
    jinit()
    jobs.work({})
    res:json({ status = jobs.get(wf_id).status, refunded = refunded })
end)
app.get("/hold", function(req, res)
    pcall(http.async.get, "$M/hold")
    res:text("ok")
end)
app.get("/chk", function(req, res)
    local ok, r = pcall(pwned.check, "e2e-a35-Zq8Xk3v-not-listed",
                        { endpoint = "$M/range/" })
    if ok then return res:json({ pwned = r }) end
    res:status(503):json({ code = tostring(r):match("^%[hull:([%w_]+)%] ") or "none" })
end)
app.get("/enq", function(req, res)
    for i = 1, 3 do
        outbox.enqueue({ kind = "webhook", destination = "$M/hook/" .. i,
                         payload = "{}" })
    end
    res:text("enq")
end)
app.get("/flush", function(req, res)
    local r = outbox.flush()
    res:json({ delivered = r.delivered, failed = r.failed, retried = r.retried,
               busy = r.busy == true })
end)
app.get("/rows", function(req, res) res:json(outbox.stats()) end)
EOF

cat > "$TMPDIR/js/app.js" <<EOF
import { app } from "hull:app";
import { httpClient } from "hull:http-client";
import { pwned } from "hull:web:pwned";
import { outbox } from "hull:web:middleware:outbox";
import { email } from "hull:email";
import { retry } from "hull:retry";
import { jobs } from "hull:jobs";
app.manifest({
    modules: ["hull/http-server@1", "hull/http-client@1", "hull/web/pwned@1",
              "hull/web/middleware/outbox@1", "hull/email@1", "hull/retry@1",
              "hull/jobs@1"],
    hosts: ["127.0.0.1", "api.postmarkapp.com"],
});
outbox.init({ maxAttempts: 1 });
const rcode = (e) => (e && e.code) || "none";
let inited = false, wfId = null, refunded = 0;
const jinit = () => { if (!inited) { jobs.init({ backoff: () => 0 }); inited = true; } };
jobs.workflow("order", async (c) => {
    await c.step("charge", () => "ch_1", { compensate: () => { refunded++; } });
    await c.step("ship", async () => { await httpClient.async.get("$M/ship"); return "shipped"; });
    return "ok";
});
app.get("/email", async (req, res) => {
    try {
        await email.send({ provider: "postmark", api_key: "k", from: "a@b.co",
                           to: "c@d.co", subject: "s", body: "b" });
        res.json({ code: "sent" });
    } catch (e) { res.json({ code: rcode(e) }); }
});
app.get("/retry", async (req, res) => {
    let calls = 0;
    try {
        await retry.run(() => { calls++; return httpClient.async.get("$M/r"); },
                        { maxAttempts: 3 });
        res.json({ code: "ok", calls });
    } catch (e) { res.json({ code: rcode(e), calls }); }
});
app.get("/wfstart", async (req, res) => {
    jinit();
    wfId = jobs.start("order", {}, { maxAttempts: 1 });
    await jobs.work({});
    const j = jobs.get(wfId);
    res.json({ status: j.status, attempts: j.attempts, refunded });
});
app.get("/wfwork", async (req, res) => {
    jinit();
    await jobs.work({});
    res.json({ status: jobs.get(wfId).status, refunded });
});
app.get("/hold", async (req, res) => {
    try { await httpClient.async.get("$M/hold"); } catch (e) {}
    res.text("ok");
});
app.get("/chk", async (req, res) => {
    try {
        res.json({ pwned: await pwned.check("e2e-a35-Zq8Xk3v-not-listed",
                                            { endpoint: "$M/range/" }) });
    } catch (e) {
        res.status(503).json({ code: (e && e.code) || "none" });
    }
});
app.get("/enq", (req, res) => {
    for (let i = 1; i <= 3; i++)
        outbox.enqueue({ kind: "webhook", destination: "$M/hook/" + i, payload: "{}" });
    res.text("enq");
});
app.get("/flush", async (req, res) => {
    const r = await outbox.flush();
    res.json({ delivered: r.delivered, failed: r.failed, retried: r.retried,
               busy: r.busy === true });
});
app.get("/rows", (req, res) => res.json(outbox.stats()));
EOF

# jget JSON KEY: the value at KEY, as Python prints it (True / False / 3).
jget() {
    printf '%s' "$1" | python3 -c "import sys, json
try: print(json.load(sys.stdin).get(sys.argv[1]))
except Exception: print('?')" "$2"
}

wait_up() {
    i=0
    while [ $i -lt 50 ]; do
        curl -s -o /dev/null "http://127.0.0.1:$PORT/rows" && return 0
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

run_runtime() {
    rt=$1; entry=$2
    echo "-- $rt --"
    ( cd "$TMPDIR/$rt" && exec "$HULL" "$entry" -p "$PORT" --no-sandbox \
        > "$TMPDIR/$rt.log" 2>&1 ) &
    SERVER_PID=$!
    if ! wait_up; then
        fail "$rt: server did not start" "$(tail -5 "$TMPDIR/$rt.log")"
        return
    fi

    out=$(curl -sS "http://127.0.0.1:$PORT/chk")
    [ "$(jget "$out" pwned)" = "False" ] && pass "$rt: baseline check answers not pwned" \
        || fail "$rt: baseline check" "$out"
    curl -sS -o /dev/null "http://127.0.0.1:$PORT/enq"

    # Hold the VM's 64 fetch slots.
    i=0
    while [ $i -lt 64 ]; do
        curl -s -m 20 -o /dev/null "http://127.0.0.1:$PORT/hold" &
        i=$((i + 1))
    done
    sleep 1.5

    code=$(curl -sS -o "$TMPDIR/chk.out" -w '%{http_code}' "http://127.0.0.1:$PORT/chk")
    out=$(cat "$TMPDIR/chk.out")
    if [ "$code" = "503" ] && [ "$(jget "$out" code)" = "busy" ]; then
        pass "$rt: with 64 fetches held, pwned.check raises the busy refusal"
    else
        fail "$rt: pwned.check under the fetch cap must not fail open" "$code $out"
    fi

    out=$(curl -sS "http://127.0.0.1:$PORT/email")
    [ "$(jget "$out" code)" = "busy" ] \
        && pass "$rt: email.send (postmark) re-raises the busy refusal with its code" \
        || fail "$rt: email provider under the fetch cap" "$out"
    out=$(curl -sS "http://127.0.0.1:$PORT/retry")
    if [ "$(jget "$out" code)" = "busy" ] && [ "$(jget "$out" calls)" = "1" ]; then
        pass "$rt: retry.run re-raises the refusal unchanged, without retrying"
    else
        fail "$rt: retry.run under the fetch cap" "$out"
    fi
    out=$(curl -sS "http://127.0.0.1:$PORT/wfstart")
    if [ "$(jget "$out" status)" = "pending" ] && [ "$(jget "$out" attempts)" = "0" ] \
       && [ "$(jget "$out" refunded)" = "0" ]; then
        pass "$rt: a busy step on the last attempt is handed back, not compensated"
    else
        fail "$rt: workflow under the fetch cap" "$out"
    fi

    out=$(curl -sS "http://127.0.0.1:$PORT/flush")
    if [ "$(jget "$out" busy)" = "True" ] && [ "$(jget "$out" failed)" = "0" ] \
       && [ "$(jget "$out" retried)" = "0" ] && [ "$(jget "$out" delivered)" = "0" ]; then
        pass "$rt: outbox.flush reports busy, no outcome recorded"
    else
        fail "$rt: outbox.flush under the fetch cap" "$out"
    fi
    out=$(curl -sS "http://127.0.0.1:$PORT/rows")
    if [ "$(jget "$out" pending)" = "3" ] && [ "$(jget "$out" failed)" = "0" ]; then
        pass "$rt: every row still pending, no attempt counted"
    else
        fail "$rt: outbox rows after a busy flush" "$out"
    fi

    # Let the held fetches finish; the next flush delivers every row. Poll
    # the server itself until the fetch cap has freed (a slow runner can
    # take well past the 4 s hold to drain 64 requests), up to 30 s.
    i=0
    while [ $i -lt 300 ]; do
        out=$(curl -sS -m 5 "http://127.0.0.1:$PORT/chk" 2>/dev/null || true)
        [ "$(jget "$out" code)" != "busy" ] && [ -n "$out" ] && break
        sleep 0.1; i=$((i + 1))
    done
    out=$(curl -sS "http://127.0.0.1:$PORT/flush")
    [ "$(jget "$out" delivered)" = "3" ] && pass "$rt: once the slots free, the rows are delivered" \
        || fail "$rt: flush after the cap" "$out"
    out=$(curl -sS "http://127.0.0.1:$PORT/chk")
    [ "$(jget "$out" pwned)" = "False" ] && pass "$rt: the check answers again" \
        || fail "$rt: check after the cap" "$out"
    # The hand-back deferred the workflow a second; poll it to its end.
    i=0
    while [ $i -lt 50 ]; do
        out=$(curl -sS "http://127.0.0.1:$PORT/wfwork")
        [ "$(jget "$out" status)" = "pending" ] || break
        sleep 0.2; i=$((i + 1))
    done
    if [ "$(jget "$out" status)" = "done" ] && [ "$(jget "$out" refunded)" = "0" ]; then
        pass "$rt: the handed-back workflow completes with nothing compensated"
    else
        fail "$rt: workflow after the cap" "$out"
    fi

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

echo "=== refusal codes (audit 35, audit 39) ==="
run_runtime lua app.lua
run_runtime js app.js

echo "refusal codes: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
