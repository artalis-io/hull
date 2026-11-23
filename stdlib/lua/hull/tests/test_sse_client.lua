-- test_sse_client.lua - Tests for hull.sse-client's event-stream parser
--
-- Run via the C test harness (test_lua.c, lua_stdlib.sse_client_suite).

local sse = require('hull.sse-client')

local pass = 0
local fail = 0

local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a), 2)
    end
end

-- Feed `chunks` to a fresh parser, then finish; all events in order.
local function run(chunks)
    local p = sse.parser()
    local out = {}
    for _, c in ipairs(chunks) do
        for _, ev in ipairs(p:feed(c)) do out[#out + 1] = ev end
    end
    for _, ev in ipairs(p:finish()) do out[#out + 1] = ev end
    return out
end

test("one event with a name, an id and data", function()
    local evs = run({ "event: tick\nid: 7\ndata: hello\n\n" })
    eq(#evs, 1)
    eq(evs[1].event, "tick"); eq(evs[1].id, "7"); eq(evs[1].data, "hello")
end)

test("the default event name is message", function()
    local evs = run({ "data: x\n\n" })
    eq(evs[1].event, "message"); eq(evs[1].id, nil)
end)

test("data lines join with LF", function()
    eq(run({ "data: a\ndata: b\ndata:c\n\n" })[1].data, "a\nb\nc")
end)

test("CRLF and CR end lines too", function()
    local evs = run({ "data: a\r\n\r\ndata: b\r\rdata: c\n\n" })
    eq(#evs, 3); eq(evs[1].data, "a"); eq(evs[2].data, "b"); eq(evs[3].data, "c")
end)

test("a CR at the end of a chunk waits for the next", function()
    local evs = run({ "data: a\r", "\n\r", "\n" })
    eq(#evs, 1); eq(evs[1].data, "a")
end)

test("any split of the input gives the same events", function()
    local text = "event: e\r\nid: 1\r\ndata: x\r\ndata: y\r\n\r\n: note\ndata: z\n\n"
    local whole = run({ text })
    local bytes = {}
    for i = 1, #text do bytes[i] = text:sub(i, i) end
    local split = run(bytes)
    eq(#split, #whole)
    for i = 1, #whole do
        eq(split[i].event, whole[i].event); eq(split[i].data, whole[i].data)
        eq(split[i].id, whole[i].id)
    end
end)

test("a leading byte-order mark is skipped", function()
    eq(run({ "\239\187\191data: x\n\n" })[1].data, "x")
end)

test("comment lines are ignored", function()
    local evs = run({ ": keep-alive\n\n: x\ndata: y\n\n" })
    eq(#evs, 1); eq(evs[1].data, "y")
end)

test("a blank line with no data dispatches nothing", function()
    eq(#run({ "event: x\n\n\n" }), 0)
end)

test("an empty data field dispatches an empty event", function()
    local evs = run({ "data\n\n" })
    eq(#evs, 1); eq(evs[1].data, "")
end)

test("only one leading space is removed from a value", function()
    eq(run({ "data:  two\n\n" })[1].data, " two")
end)

test("the last id persists across events; an id with NUL is ignored", function()
    local evs = run({ "id: 1\ndata: a\n\ndata: b\n\nid: x\0y\ndata: c\n\n" })
    eq(evs[1].id, "1"); eq(evs[2].id, "1"); eq(evs[3].id, "1")
end)

test("retry takes digits only", function()
    local evs = run({ "retry: 1500\ndata: a\n\nretry: 1.5\ndata: b\n\n" })
    eq(evs[1].retry, 1500); eq(evs[2].retry, 1500)
end)

test("unknown fields are ignored", function()
    local evs = run({ "foo: bar\ndata: a\n\n" })
    eq(#evs, 1); eq(evs[1].data, "a")
end)

test("an event the input ended inside is dropped", function()
    eq(#run({ "data: a\n\ndata: b\n" }), 1)
    eq(#run({ "data: a" }), 0)
end)

test("a trailing CR at the end of the input completes its line", function()
    local evs = run({ "data: a\r\r" })
    eq(#evs, 1); eq(evs[1].data, "a")
end)

test("feed refuses a non-string", function()
    local ok = pcall(function() sse.parser():feed(42) end)
    eq(ok, false)
end)

test("events() refuses something that is not a stream response", function()
    eq(pcall(sse.events, {}), false)
end)

test("events() iterates a response's reads and ends at nil", function()
    local chunks = { "data: 1\n\nda", "ta: 2\n", "\n" }
    local i = 0
    local res = { read = function() i = i + 1; return chunks[i] end }
    local got = {}
    for ev in sse.events(res) do got[#got + 1] = ev.data end
    eq(#got, 2); eq(got[1], "1"); eq(got[2], "2")
end)

test("events() raises the stream's error with its code", function()
    local first = true
    local res = { read = function()
        if first then first = false; return "data: 1\n\n" end
        return nil, "nothing arrived within the idle timeout", "idle"
    end }
    local it = sse.events(res)
    eq(it().data, "1")
    local ok, err = pcall(it)
    eq(ok, false)
    eq(tostring(err):find("(idle)", 1, true) ~= nil, true, tostring(err))
end)

-- ── audit 52 ──────────────────────────────────────────────────────────

-- A fake stream: `chunks` read in order, then `final` (nil = the end, or
-- { err, code }); counts close() calls.
local function fake(chunks, final)
    local i, r = 0, { closes = 0 }
    r.read = function()
        i = i + 1
        if chunks[i] then return chunks[i] end
        if final then return nil, final[1], final[2] end
        return nil
    end
    r.close = function() r.closes = r.closes + 1 end
    return r
end

test("sse_client_audit52.long_line_linear", function()
    -- A 1 MB line in 64-byte chunks: each chunk is scanned once. The old
    -- parser recopied and rescanned the whole pending line per chunk.
    local p = sse.parser()
    local piece = string.rep("x", 64)
    local t0 = os.clock()
    p:feed("data: ")
    for i = 1, 16000 do
        p:feed(piece)
        if i % 256 == 0 and os.clock() - t0 > 3 then
            error("quadratic: " .. i .. " chunks took over 3 s")
        end
    end
    local evs = p:feed("\n\n")
    eq(#evs, 1); eq(#evs[1].data, 16000 * 64)
end)

test("sse_client_audit52.tiny_chunks", function()
    -- 200 KB of a line fed one byte at a time: the pending pieces stay a
    -- short tower, and the event arrives intact.
    local p = sse.parser()
    local t0 = os.clock()
    p:feed("data: ")
    for i = 1, 204800 do
        p:feed(string.char(97 + i % 26))
        if i % 4096 == 0 and os.clock() - t0 > 3 then
            error("too slow: " .. i .. " bytes took over 3 s")
        end
    end
    local evs = p:feed("\n\n")
    eq(#evs, 1); eq(#evs[1].data, 204800)
    eq(evs[1].data:sub(1, 3), "bcd"); eq(evs[1].data:sub(-1), string.char(97 + 204800 % 26))
end)

test("sse_client_audit52.max_event_bytes", function()
    -- A line that never ends.
    local p = sse.parser({ max_event_bytes = 100 })
    p:feed("data: " .. string.rep("x", 80))
    local ok, e = pcall(p.feed, p, string.rep("x", 40))
    eq(ok, false); eq(type(e), "table"); eq(e.code, "too_large")
    -- Spent: a later feed raises again.
    ok, e = pcall(p.feed, p, "\n\n")
    eq(ok, false); eq(e.code, "too_large")
    -- data lines with no blank line between them.
    p = sse.parser({ max_event_bytes = 100 })
    ok, e = pcall(function()
        for _ = 1, 50 do p:feed("data: xyz\n") end
    end)
    eq(ok, false); eq(e.code, "too_large")
    -- The default is 1 MiB.
    p = sse.parser()
    ok, e = pcall(p.feed, p, "data: " .. string.rep("x", 1048576))
    eq(ok, false); eq(e.code, "too_large")
    -- Within it nothing changes, and a dispatch starts the count again.
    p = sse.parser({ max_event_bytes = 100 })
    for _ = 1, 5 do eq(#p:feed("data: " .. string.rep("y", 90) .. "\n\n"), 1) end
    -- Bad options are refused.
    eq(pcall(sse.parser, { max_event_bytes = 0 }), false)
    eq(pcall(sse.parser, { max_event_bytes = 1.5 }), false)
end)

test("sse_client_audit52.events_too_large_closes", function()
    local res = fake({ "data: " .. string.rep("x", 64) })
    local it = sse.events(res, { max_event_bytes = 16 })
    local ok, e = pcall(it)
    eq(ok, false); eq(e.code, "too_large"); eq(res.closes, 1)
    eq(it(), nil)
end)

test("sse_client_audit52.events_close_on_end", function()
    local res = fake({ "data: 1\n\n" })
    local got = {}
    for ev in sse.events(res) do got[#got + 1] = ev.data end
    eq(#got, 1); eq(res.closes, 1)
end)

test("sse_client_audit52.events_close_on_error", function()
    local res = fake({ "data: 1\n\n" }, { "went quiet", "idle" })
    local it = sse.events(res)
    eq(it().data, "1")
    local ok, e = pcall(it)
    eq(ok, false); eq(e.code, "idle")
    eq(res.closes, 1)
    eq(it(), nil)
end)

test("sse_client_audit52.events_close_on_break", function()
    local res = fake({ "data: 1\n\ndata: 2\n\n" })
    local seen = 0
    for ev in sse.events(res) do
        seen = seen + 1
        if ev.data == "1" then break end
    end
    eq(seen, 1)
    eq(res.closes, 1)
end)

test("sse_client_audit52.error_code", function()
    local res = fake({}, { "nothing arrived within the idle timeout", "idle" })
    local ok, e = pcall(sse.events(res))
    eq(ok, false); eq(type(e), "table")
    eq(e.code, "idle"); eq(e.message, "nothing arrived within the idle timeout")
    eq(tostring(e), "sse-client: nothing arrived within the idle timeout (idle)")
end)

test("sse_client_audit52.retry_persists", function()
    local evs = run({ "retry: 5000\n\ndata: a\n\nretry: 99999999999999999999\ndata: b\n\n" })
    eq(#evs, 2); eq(evs[1].retry, 5000); eq(evs[2].retry, 5000)
    eq(run({ "data: a\n\n" })[1].retry, nil)
end)

test("sse_client_audit52.cr_at_chunk_end", function()
    local p = sse.parser()
    eq(#p:feed("data: a\r"), 0)
    local evs = p:feed("\r")      -- the blank line ends at once
    eq(#evs, 1); eq(evs[1].data, "a")
    -- An LF right after a chunk-ending CR is skipped, once.
    p = sse.parser()
    p:feed("data: b\r")
    evs = p:feed("\n\n")
    eq(#evs, 1); eq(evs[1].data, "b")
    p = sse.parser()
    p:feed("data: c\r")
    eq(#p:feed("\n"), 0)
    eq(#p:feed("\n"), 1)
end)

test("sse_client_audit52.empty_event_type", function()
    local evs = run({ "event:\ndata: x\n\n" })
    eq(#evs, 1); eq(evs[1].event, "message")
end)

test("sse_client_audit52.bom_split", function()
    eq(run({ "\239", "\187\191data: x\n\n" })[1].data, "x")
    eq(run({ "\239\187", "\191data: y\n\n" })[1].data, "y")
    -- Not a BOM after all: the bytes are kept.
    local evs = run({ "\239", "\187data: z\n\ndata: w\n\n" })
    eq(#evs, 1); eq(evs[1].data, "w")
end)

return { pass = pass, fail = fail }
