#!/bin/sh
# e2e_http_stream.sh: http-client's async.stream and hull/sse-client, both
# runtimes, against a local Hull server.
#
# A stream resolves with the status and headers as soon as they arrive and
# then hands the body over chunk by chunk while the request is still running
# (cap/http_stream.c). Covered here, from app.main in Lua and in JS:
#   events    the first SSE event arrives well before the last is sent
#   sse       hull/sse-client parses the events (name, id, data)
#   big       a 300 000-byte body arrives whole through the reads
#   idle      a server that goes quiet past idle_timeout_ms ends the stream
#             with code "idle", after the event it did send
#   headtime  no headers within timeout_ms is code "timeout"
#   overflow  a reader that falls buffer_bytes behind gets what was buffered,
#             then code "overflow"
#   toolarge  a body past max_bytes is code "too_large"
#   close     a read after close() is code "closed"
#   denied    a host manifest.hosts does not admit is refused coded "denied"
#   leftopen  app.main returning with a stream still open exits cleanly
#   life      an upstream that heartbeats every 200 ms (so idle never fires)
#             still ends the stream at life_ms, coded "life", after the
#             buffered events (audit 52: the whole stream had no bound below
#             24 h, so a relay whose client left held its in-flight slot)
#   lifehead  life_ms also bounds the wait for the headers
#   lifeclamp a life_ms above 24 h is clamped, not refused
#   lifebad   a life_ms of 0, or not an integer (Lua) / number (JS), is refused
# and from a request handler in each runtime (the request's connection is
# suspended while the handler waits on the stream): /proxy relays the events.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -e

HULL="${HULL:-build/hull}"
PORT="${PORT:-19931}"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1 (got: $2)"; }

HULL=$(cd "$(dirname "$HULL")" && pwd)/$(basename "$HULL")
TMPDIR=$(mktemp -d)
PIDS=""
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null || true; done
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

ORIGIN=$PORT
PROXY_LUA=$((PORT + 1))
PROXY_JS=$((PORT + 2))

mkdir -p "$TMPDIR/origin" "$TMPDIR/clua" "$TMPDIR/cjs" "$TMPDIR/plua" "$TMPDIR/pjs"

cat > "$TMPDIR/origin/app.lua" <<'EOF'
app.manifest({ modules = { "hull/http-server@1", "hull/web/sse@1" } })
app.sse("/events", function(req, stream)
    for i = 1, 3 do
        stream:event("tick", "n=" .. i, tostring(i))
        if i < 3 then hull.sleep(400) end
    end
    stream:close()
end)
app.sse("/quiet", function(req, stream)
    stream:event("first", "hello", "1")
    hull.sleep(3000)
    stream:close()
end)
app.sse("/heartbeat", function(req, stream)
    for i = 1, 30 do
        stream:event("beat", "b" .. i, tostring(i))
        hull.sleep(200)
    end
    stream:close()
end)
app.get("/big", function(req, res) res:text(string.rep("x", 300000)) end)
app.get("/small", function(req, res) res:text("hello") end)
app.get("/slowhead", function(req, res)
    hull.sleep(2000)
    res:text("late")
end)
EOF

cat > "$TMPDIR/clua/app.lua" <<'EOF'
app.manifest({ modules = { "hull/http-client@1", "hull/sse-client@1", "hull/time@1" },
               hosts = { "127.0.0.1" } })
local http = require("hull.http-client")
local sse = require("hull.sse-client")
local time = require("hull.time")
local function drain(res)
    local n = 0
    while true do
        local c, e, code = res.read()
        if not c then return n, e and code or "end" end
        n = n + #c
    end
end
app.main(function(ctx)
    local case, base = ctx.args[1], "http://127.0.0.1:" .. ctx.args[2]
    local out
    if case == "events" then
        local res = assert(http.async.stream("GET", base .. "/events"))
        local t0, first = time.now_ms(), nil
        while true do
            local c = res.read()
            if not c then break end
            first = first or time.now_ms()
        end
        out = res.status .. " " .. res.headers["content-type"] .. " " ..
              ((time.now_ms() - first) >= 600 and "progressive" or "batched")
    elseif case == "sse" then
        local res = assert(http.async.stream("GET", base .. "/events"))
        local parts = {}
        for ev in sse.events(res) do parts[#parts + 1] = ev.event .. "/" .. ev.id .. "/" .. ev.data end
        out = table.concat(parts, ",")
    elseif case == "big" then
        local n, why = drain(assert(http.async.stream("GET", base .. "/big")))
        out = n .. " " .. why
    elseif case == "idle" then
        local res = assert(http.async.stream("GET", base .. "/quiet", { idle_timeout_ms = 500 }))
        local n, why = drain(res)
        out = (n > 0 and "data" or "nodata") .. " " .. why
    elseif case == "headtime" then
        local res, err, code = http.async.stream("GET", base .. "/slowhead", { timeout_ms = 300 })
        out = tostring(res) .. " " .. tostring(code)
    elseif case == "overflow" then
        local res = assert(http.async.stream("GET", base .. "/big", { buffer_bytes = 16384 }))
        hull.sleep(500)
        local n, why = drain(res)
        out = (n <= 16384 and "bounded" or ("unbounded:" .. n)) .. " " .. why
    elseif case == "toolarge" then
        local n, why = drain(assert(http.async.stream("GET", base .. "/big", { max_bytes = 1000 })))
        out = why
    elseif case == "close" then
        local res = assert(http.async.stream("GET", base .. "/events"))
        res.read()
        res.close()
        local c, e, code = res.read()
        out = tostring(c) .. " " .. tostring(code)
    elseif case == "denied" then
        local ok, e = pcall(http.async.stream, "GET", "http://denied.example/x")
        out = tostring(ok) .. " " .. (tostring(e):match("^%[hull:([%w_]+)%]") or tostring(e))
    elseif case == "leftopen" then
        local res = assert(http.async.stream("GET", base .. "/quiet"))
        res.read()
        out = "returning"
    elseif case == "noclose" then
        -- stream_audit52.noclose: a stream read to its end gives its
        -- in-flight slot and buffer back without close().
        local n = 0
        for _ = 1, 100 do
            local res = assert(http.async.stream("GET", base .. "/small"))
            while res.read() do end
            n = n + 1
        end
        out = n
    elseif case == "noclosehold" then
        -- stream_audit52.noclosehold: the off-heap hold goes with the end
        -- (four 16 MiB buffers fill the 64 MiB heap limit).
        local n = 0
        for _ = 1, 10 do
            local res = assert(http.async.stream("GET", base .. "/small",
                                                 { buffer_bytes = 16 * 1024 * 1024 }))
            while res.read() do end
            n = n + 1
        end
        out = n
    elseif case == "endstate" then
        -- stream_audit52.endstate: after the end, read stays nil and close
        -- is a no-op.
        local res = assert(http.async.stream("GET", base .. "/small"))
        local body = res.read()
        local a, b = res.read(), res.read()
        res.close()
        out = body .. " " .. tostring(a) .. " " .. tostring(b) .. " " .. tostring(res.read())
    elseif case == "dropped" then
        -- stream_audit52.dropped: at the in-flight cap, streams the app
        -- dropped go with one collection.
        for _ = 1, 80 do
            local res = assert(http.async.stream("GET", base .. "/quiet",
                                                 { buffer_bytes = 16384 }))
            res = nil
        end
        out = "ok"
    elseif case == "stropt" then
        -- stream_audit52.stropt: a numeric string is not an integer option.
        local ok = pcall(http.async.stream, "GET", base .. "/small",
                         { buffer_bytes = "65536" })
        local ok2 = pcall(http.async.stream, "GET", base .. "/small",
                          { idle_timeout_ms = 1.5 })
        out = tostring(ok) .. " " .. tostring(ok2)
    elseif case == "life" then
        local t0 = time.now_ms()
        local res = assert(http.async.stream("GET", base .. "/heartbeat", { life_ms = 1500 }))
        local n, why = drain(res)
        local dt = time.now_ms() - t0
        out = (n > 0 and "data" or "nodata") .. " " .. why .. " " ..
              ((dt >= 1300 and dt < 4000) and "bounded" or ("dt=" .. dt))
    elseif case == "lifehead" then
        local res, err, code = http.async.stream("GET", base .. "/slowhead", { life_ms = 300 })
        out = tostring(res) .. " " .. tostring(code)
    elseif case == "lifeclamp" then
        local res = assert(http.async.stream("GET", base .. "/heartbeat", { life_ms = 1 << 40 }))
        local c = res.read()
        res.close()
        out = c and "ok" or "nochunk"
    elseif case == "lifebad" then
        local parts = {}
        for _, v in ipairs({ 0, -5, 1.5, "x" }) do
            local ok = pcall(http.async.stream, "GET", base .. "/heartbeat", { life_ms = v })
            parts[#parts + 1] = ok and "accepted" or "refused"
        end
        out = table.concat(parts, ",")
    end
    ctx.stdout:write(case .. ":" .. tostring(out) .. "\n")
    return 0
end)
EOF

cat > "$TMPDIR/cjs/app.js" <<'EOF'
import { app } from "hull:app";
import { httpClient } from "hull:http-client";
import { sseClient } from "hull:sse-client";
app.manifest({ modules: ["hull/http-client@1", "hull/sse-client@1"], hosts: ["127.0.0.1"] });
const stream = httpClient.async.stream;
async function drain(res) {
    let n = 0;
    try {
        for await (const c of res) n += c.byteLength;
        return [n, "end"];
    } catch (e) {
        return [n, e.code || String(e)];
    }
}
app.main(async (ctx) => {
    const [kase, port] = ctx.args;
    const base = `http://127.0.0.1:${port}`;
    let out;
    if (kase === "events") {
        const res = await stream("GET", base + "/events");
        let first = null, c;
        while ((c = await res.read()) !== null) if (first === null) first = Date.now();
        out = `${res.status} ${res.headers["content-type"]} ` +
              (Date.now() - first >= 600 ? "progressive" : "batched");
    } else if (kase === "sse") {
        const res = await stream("GET", base + "/events");
        const parts = [];
        for await (const ev of sseClient.events(res)) parts.push(`${ev.event}/${ev.id}/${ev.data}`);
        out = parts.join(",");
    } else if (kase === "big") {
        const [n, why] = await drain(await stream("GET", base + "/big"));
        out = `${n} ${why}`;
    } else if (kase === "idle") {
        const [n, why] = await drain(await stream("GET", base + "/quiet", { idleTimeoutMs: 500 }));
        out = `${n > 0 ? "data" : "nodata"} ${why}`;
    } else if (kase === "headtime") {
        try { await stream("GET", base + "/slowhead", { timeoutMs: 300 }); out = "resolved"; }
        catch (e) { out = `rejected ${e.code}`; }
    } else if (kase === "overflow") {
        const res = await stream("GET", base + "/big", { bufferBytes: 16384 });
        await hull.sleep(500);
        const [n, why] = await drain(res);
        out = `${n <= 16384 ? "bounded" : "unbounded:" + n} ${why}`;
    } else if (kase === "toolarge") {
        const [, why] = await drain(await stream("GET", base + "/big", { maxBytes: 1000 }));
        out = why;
    } else if (kase === "close") {
        const res = await stream("GET", base + "/events");
        await res.read();
        res.close();
        try { await res.read(); out = "read"; } catch (e) { out = `threw ${e.code}`; }
    } else if (kase === "denied") {
        try { await stream("GET", "http://denied.example/x"); out = "resolved"; }
        catch (e) { out = `threw ${e.code}`; }
    } else if (kase === "leftopen") {
        const res = await stream("GET", base + "/quiet");
        await res.read();
        out = "returning";
    } else if (kase === "life") {
        const t0 = Date.now();
        const [n, why] = await drain(await stream("GET", base + "/heartbeat", { lifeMs: 1500 }));
        const dt = Date.now() - t0;
        out = `${n > 0 ? "data" : "nodata"} ${why} ` +
              (dt >= 1300 && dt < 4000 ? "bounded" : `dt=${dt}`);
    } else if (kase === "lifehead") {
        try { await stream("GET", base + "/slowhead", { lifeMs: 300 }); out = "resolved"; }
        catch (e) { out = `rejected ${e.code}`; }
    } else if (kase === "lifeclamp") {
        const res = await stream("GET", base + "/heartbeat", { lifeMs: 2 ** 40 });
        const c = await res.read();
        res.close();
        out = c ? "ok" : "nochunk";
    } else if (kase === "lifebad") {
        const parts = [];
        for (const v of [0, -5, "x", NaN]) {
            try {
                const res = await stream("GET", base + "/heartbeat", { lifeMs: v });
                res.close();
                parts.push("accepted");
            } catch (e) { parts.push("refused"); }
        }
        out = parts.join(",");
    }
    ctx.stdout.write(`${kase}:${out}\n`);
    return 0;
});
EOF

cat > "$TMPDIR/plua/app.lua" <<'EOF'
app.manifest({ modules = { "hull/http-server@1", "hull/http-client@1", "hull/sse-client@1",
                           "hull/env@1" },
               hosts = { "127.0.0.1" }, env = { "ORIGIN_PORT" } })
local http = require("hull.http-client")
local sse = require("hull.sse-client")
local env = require("hull.env")
app.get("/proxy", function(req, res)
    local s = assert(http.async.stream("GET",
        "http://127.0.0.1:" .. env.get("ORIGIN_PORT") .. "/events"))
    local parts = {}
    for ev in sse.events(s) do parts[#parts + 1] = ev.data end
    res:text(table.concat(parts, ","))
end)
EOF

cat > "$TMPDIR/pjs/app.js" <<'EOF'
import { app } from "hull:app";
import { httpClient } from "hull:http-client";
import { sseClient } from "hull:sse-client";
import { env } from "hull:env";
app.manifest({ modules: ["hull/http-server@1", "hull/http-client@1", "hull/sse-client@1",
                         "hull/env@1"],
               hosts: ["127.0.0.1"], env: ["ORIGIN_PORT"] });
app.get("/proxy", async (req, res) => {
    const s = await httpClient.async.stream("GET",
        `http://127.0.0.1:${env.get("ORIGIN_PORT")}/events`);
    const parts = [];
    for await (const ev of sseClient.events(s)) parts.push(ev.data);
    res.text(parts.join(","));
});
EOF

wait_port() {
    n=0
    until curl -s -o /dev/null "http://127.0.0.1:$1/" 2>/dev/null || [ $n -ge 50 ]; do
        sleep 0.1; n=$((n + 1))
    done
}

(cd "$TMPDIR/origin" && exec "$HULL" -p "$ORIGIN" app.lua) > "$TMPDIR/origin.log" 2>&1 &
PIDS="$PIDS $!"
wait_port "$ORIGIN"

expect() {   # runtime case expected [name]
    rt=$1; kase=$2; want=$3; name=${4:-$2}
    if [ "$rt" = lua ]; then dir="$TMPDIR/clua"; entry=app.lua; else dir="$TMPDIR/cjs"; entry=app.js; fi
    rc=0
    got=$(cd "$dir" && "$HULL" "$entry" -- "$kase" "$ORIGIN" 2>"$TMPDIR/err.txt") || rc=$?
    got=$(printf '%s\n' "$got" | grep "^$kase:" | head -1)
    if [ "$rc" -eq 0 ] && [ "$got" = "$kase:$want" ]; then
        pass "[$rt] $name"
    else
        fail "[$rt] $name" "rc=$rc '$got' $(tail -3 "$TMPDIR/err.txt" | tr '\n' ' ')"
    fi
}

for rt in lua js; do
    echo "== http stream ($rt)"
    expect "$rt" events   "200 text/event-stream progressive"
    expect "$rt" sse      "tick/1/n=1,tick/2/n=2,tick/3/n=3"
    expect "$rt" big      "300000 end"
    expect "$rt" idle     "data idle"
    if [ "$rt" = lua ]; then expect lua headtime "nil timeout"
    else expect js headtime "rejected timeout"; fi
    expect "$rt" overflow "bounded overflow"
    expect "$rt" toolarge "too_large"
    if [ "$rt" = lua ]; then expect lua close "nil closed"
    else expect js close "threw closed"; fi
    if [ "$rt" = lua ]; then expect lua denied "false denied"
    else expect js denied "threw denied"; fi
    expect "$rt" leftopen "returning"
    if [ "$rt" = lua ]; then
        expect lua noclose     "100"
        expect lua noclosehold "10"
        expect lua endstate    "hello nil nil nil"
        expect lua dropped     "ok"
        expect lua stropt      "false false"
    fi
    expect "$rt" life      "data life bounded"      "stream_audit52.life_short_ends_stream"
    if [ "$rt" = lua ]; then expect lua lifehead "nil life" "stream_audit52.life_bounds_head"
    else expect js lifehead "rejected life" "stream_audit52.life_bounds_head"; fi
    expect "$rt" lifeclamp "ok"                     "stream_audit52.life_above_max_clamps"
    expect "$rt" lifebad   "refused,refused,refused,refused" "stream_audit52.life_invalid_refused"
done

echo "== http stream inside a request handler"
(cd "$TMPDIR/plua" && ORIGIN_PORT=$ORIGIN exec "$HULL" -p "$PROXY_LUA" app.lua) > "$TMPDIR/plua.log" 2>&1 &
PIDS="$PIDS $!"
(cd "$TMPDIR/pjs" && ORIGIN_PORT=$ORIGIN exec "$HULL" -p "$PROXY_JS" app.js) > "$TMPDIR/pjs.log" 2>&1 &
PIDS="$PIDS $!"
wait_port "$PROXY_LUA"
wait_port "$PROXY_JS"
for pair in "lua:$PROXY_LUA" "js:$PROXY_JS"; do
    rt=${pair%%:*}; p=${pair#*:}
    got=$(curl -s --max-time 10 "http://127.0.0.1:$p/proxy" || true)
    [ "$got" = "n=1,n=2,n=3" ] && pass "[$rt] /proxy relays the events" ||
        fail "[$rt] /proxy" "'$got' $(tail -3 "$TMPDIR/p$rt.log" | tr '\n' ' ')"
done

# ── audit 52, JS binding (stream_audit52.*) ──────────────────────────────
# A raw origin (python3) for the byte-level cases: headers that arrive late,
# a body sent one byte per chunk, a body that only starts later.
#   gchead     a handle whose header wait is parked survives a collection
#   tmpread    `await (await stream()).read()` (the handle a temporary) is
#              answered, from app.main and from a request handler
#   ended      65 streams read to their end and kept (never closed) do not
#              hold the VM's 64 in-flight slots
#   gcretry    at the 64-slot cap, dropped open streams are collected first
#   mix        read() after readText() kept part of a character returns
#              those bytes first
#   drip       readText() over a one-byte-at-a-time body never returns ""
#              before the end
#   frac       a fractional maxBytes is refused
#   parkexit   app.main returning with an un-awaited read parked exits
#              cleanly (the wait's pin is dropped at teardown)
#   hangup     a client hanging up on a handler whose read is parked, then
#              SIGTERM: the server exits cleanly
A52_ORIGIN=$((PORT + 3))
A52_PROXY=$((PORT + 4))
mkdir -p "$TMPDIR/a52" "$TMPDIR/a52p"
cat > "$TMPDIR/a52/origin.py" <<'EOF'
import socketserver, sys, time

class H(socketserver.StreamRequestHandler):
    def handle(self):
        line = self.rfile.readline().decode("latin-1")
        while self.rfile.readline() not in (b"\r\n", b"\n", b""):
            pass
        path = line.split(" ")[1] if " " in line else "/"
        w = self.wfile
        def head():
            w.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                    b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
            w.flush()
        def chunk(b):
            w.write(b"%x\r\n" % len(b) + b + b"\r\n")
            w.flush()
        def end():
            w.write(b"0\r\n\r\n")
            w.flush()
        if path == "/slowhead":
            time.sleep(1.5); head(); chunk(b"late"); end()
        elif path == "/later":
            head(); time.sleep(0.5); chunk(b"later"); end()
        elif path == "/small":
            head(); chunk(b"ok"); end()
        elif path == "/hold":
            head(); time.sleep(6); end()
        elif path == "/split":
            head(); chunk(b"a"); chunk(b"\xc3"); time.sleep(0.4); chunk(b"\xa9"); end()
        elif path == "/drip":
            head()
            for b in "hé€\U0001f600!".encode("utf-8"):
                chunk(bytes([b])); time.sleep(0.03)
            end()

class S(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True

S(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
EOF
cat > "$TMPDIR/a52/app.js" <<'EOF'
import { app } from "hull:app";
import { httpClient } from "hull:http-client";
app.manifest({ modules: ["hull/http-client@1"], hosts: ["127.0.0.1"] });
const stream = httpClient.async.stream;
async function bytes(res) {
    const out = [];
    let c;
    while ((c = await res.read()) !== null) out.push(...new Uint8Array(c));
    return out;
}
app.main(async (ctx) => {
    const [kase, port] = ctx.args;
    const base = `http://127.0.0.1:${port}`;
    let out;
    if (kase === "gchead") {
        const p = stream("GET", base + "/slowhead");
        let keep = 0;
        for (let i = 0; i < 400000; i++) { const x = { i }; x.self = x; keep ^= i; }
        const res = await p;
        out = `${res.status} ${String.fromCharCode(...(await bytes(res)))}`;
    } else if (kase === "tmpread") {
        const c = await (await stream("GET", base + "/later")).read();
        out = String.fromCharCode(...new Uint8Array(c));
    } else if (kase === "ended") {
        const kept = [];
        let ok = 0;
        for (let i = 0; i < 70; i++) {
            const res = await stream("GET", base + "/small");
            kept.push(res);
            if ((await bytes(res)).length === 2) ok++;
            if ((await res.read()) !== null) throw new Error("read after the end");
        }
        out = `${ok}`;
    } else if (kase === "gcretry") {
        for (let i = 0; i < 64; i++) {
            const o = { res: await stream("GET", base + "/hold") };
            o.self = o;
        }
        const res = await stream("GET", base + "/small");
        out = String.fromCharCode(...(await bytes(res)));
    } else if (kase === "mix") {
        const res = await stream("GET", base + "/split");
        await hull.sleep(200);
        const t = await res.readText();
        out = `${t} ${(await bytes(res)).map((b) => b.toString(16)).join(",")}`;
    } else if (kase === "drip") {
        const res = await stream("GET", base + "/drip");
        const parts = [];
        let t;
        while ((t = await res.readText()) !== null) parts.push(t);
        out = `${parts.includes("") ? "empty" : "noempty"} ${parts.join("") === "hé€\u{1f600}!"}`;
    } else if (kase === "parkexit") {
        const res = await stream("GET", base + "/hold");
        res.read();
        out = "returning";
    } else if (kase === "frac") {
        try { stream("GET", base + "/small", { maxBytes: 1.5 }); out = "accepted"; }
        catch (e) { out = `threw ${e.name}`; }
    }
    ctx.stdout.write(`${kase}:${out}\n`);
    return 0;
});
EOF
cat > "$TMPDIR/a52p/app.js" <<'EOF'
import { app } from "hull:app";
import { httpClient } from "hull:http-client";
import { env } from "hull:env";
app.manifest({ modules: ["hull/http-server@1", "hull/http-client@1", "hull/env@1"],
               hosts: ["127.0.0.1"], env: ["ORIGIN_PORT"] });
app.get("/tmpread", async (req, res) => {
    const c = await (await httpClient.async.stream("GET",
        `http://127.0.0.1:${env.get("ORIGIN_PORT")}/later`)).read();
    res.text(String.fromCharCode(...new Uint8Array(c)));
});
app.get("/hold", async (req, res) => {
    const s = await httpClient.async.stream("GET",
        `http://127.0.0.1:${env.get("ORIGIN_PORT")}/hold`);
    await s.read();
    res.text("held");
});
EOF

if command -v python3 >/dev/null 2>&1; then
    echo "== http stream, audit 52 (js)"
    python3 "$TMPDIR/a52/origin.py" "$A52_ORIGIN" > "$TMPDIR/a52origin.log" 2>&1 &
    PIDS="$PIDS $!"
    n=0
    until nc -z 127.0.0.1 "$A52_ORIGIN" 2>/dev/null || [ $n -ge 50 ]; do sleep 0.1; n=$((n + 1)); done
    a52() {   # case expected: runs app.main under a 15 s limit (a hang fails)
        kase=$1; want=$2
        (cd "$TMPDIR/a52" && exec "$HULL" app.js -- "$kase" "$A52_ORIGIN") \
            > "$TMPDIR/a52.out" 2> "$TMPDIR/a52.err" &
        pid=$!
        t=0
        while kill -0 "$pid" 2>/dev/null && [ $t -lt 150 ]; do sleep 0.1; t=$((t + 1)); done
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            fail "stream_audit52.$kase" "hung"
            return
        fi
        rc=0; wait "$pid" || rc=$?
        got=$(grep "^$kase:" "$TMPDIR/a52.out" | head -1)
        if [ "$rc" -eq 0 ] && [ "$got" = "$kase:$want" ]; then
            pass "stream_audit52.$kase"
        else
            fail "stream_audit52.$kase" "rc=$rc '$got' $(tail -3 "$TMPDIR/a52.err" | tr '\n' ' ')"
        fi
    }
    a52 gchead  "200 late"
    a52 tmpread "later"
    a52 ended   "70"
    a52 gcretry "ok"
    a52 mix     "a c3,a9"
    a52 drip    "noempty true"
    a52 frac    "threw TypeError"
    a52 parkexit "returning"
    (cd "$TMPDIR/a52p" && ORIGIN_PORT=$A52_ORIGIN exec "$HULL" -p "$A52_PROXY" app.js) \
        > "$TMPDIR/a52p.log" 2>&1 &
    PIDS="$PIDS $!"
    wait_port "$A52_PROXY"
    got=$(curl -s --max-time 5 "http://127.0.0.1:$A52_PROXY/tmpread" || true)
    [ "$got" = "later" ] && pass "stream_audit52.tmpread_handler" ||
        fail "stream_audit52.tmpread_handler" "'$got' $(tail -3 "$TMPDIR/a52p.log" | tr '\n' ' ')"
    curl -s --max-time 1 "http://127.0.0.1:$A52_PROXY/hold" > /dev/null 2>&1 || true
    sleep 0.3
    ppid=$(echo $PIDS | awk '{print $NF}')
    kill -TERM "$ppid" 2>/dev/null || true
    rc=0; wait "$ppid" 2>/dev/null || rc=$?
    if [ "$rc" -eq 0 ] && ! grep -q "Assertion failed" "$TMPDIR/a52p.log"; then
        pass "stream_audit52.hangup"
    else
        fail "stream_audit52.hangup" "rc=$rc $(tail -3 "$TMPDIR/a52p.log" | tr '\n' ' ')"
    fi
else
    echo "  SKIP: stream_audit52.* (no python3)"
fi

echo "e2e_http_stream: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
