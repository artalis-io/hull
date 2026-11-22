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
    eq(evs[1].retry, 1500); eq(evs[2].retry, nil)
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

return { pass = pass, fail = fail }
