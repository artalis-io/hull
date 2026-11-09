-- test_stdlib_data_audit44.lua - stdlib data-layer regressions from audit 44
-- (Lua; the JS twin is test_stdlib_data_audit44.js).
--
-- jobs: a typed subscription moves its cursor past events it does not take
-- (M10), cleanup keeps the newest job / event row as a high-water mark,
-- timestamp columns are BIGINT, and the type lists / queue scopes are
-- checked; idempotency: the in-flight lease outlives a parked handler (M11)
-- and respond encodes once inside the store's guard; attachment.read_to_file
-- checks the blob's hash; kv.open at its cap is a "busy" refusal; retry
-- checks factor; search snippet / highlight / index parity; inbox / outbox
-- parity.
--
-- Each case runs in a fresh caps-bearing state and a fresh :memory: database
-- (run_data44_case in tests/hull/runtime/lua/test_lua.c), named in the
-- global __data44_case.

local json = require("hull.json")

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

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a), 2)
    end
end

local function raises(fn, needle, msg)
    local ok, err = pcall(fn)
    if ok then error((msg or "") .. ": expected an error", 2) end
    if needle and not tostring(err):find(needle, 1, true) then
        error((msg or "") .. ": expected an error naming '" .. needle .. "', got "
              .. tostring(err), 2)
    end
end

local cases = {}

-- M10: a typed subscription's cursor moves past the events it does not take.
function cases.jobs_typed_cursor()
    local jobs = require("hull.jobs")
    local hooks = __test_hooks("hull.jobs")
    jobs.init({ events = true })
    test("a typed subscription moves past unmatched events", function()
        local seen = {}
        jobs.subscribe("dead44", function(ev) seen[#seen + 1] = ev.type end,
                       { types = { "cancelled" }, from = "beginning" })
        for i = 1, 5 do jobs.enqueue("t44", { i = i }, { queue = "q44" }) end
        local ids = hooks.event_ids()
        assert_eq(#ids, 5, "five enqueued events")
        local r = hooks.events_drain("dead44", { grace = 0 })
        assert_eq(r.delivered, 0, "none matched")
        assert_eq(hooks.cursor("dead44"), ids[5], "cursor at the scanned horizon")
    end)
    test("matching events are still delivered, in order", function()
        local seen = {}
        jobs.subscribe("canc44", function(ev) seen[#seen + 1] = ev.type end,
                       { types = { "cancelled" }, from = "beginning" })
        local a = jobs.enqueue("t44", {}, { queue = "q44" })
        jobs.cancel(a)
        jobs.enqueue("t44", {}, { queue = "q44" })
        local ids = hooks.event_ids()
        local r = hooks.events_drain("canc44", { grace = 0 })
        assert_eq(r.delivered, 1, "the cancelled event")
        assert_eq(seen[1], "cancelled", "type")
        assert_eq(hooks.cursor("canc44"), ids[#ids], "past the later unmatched one")
    end)
    test("a full batch does not jump past undelivered matches", function()
        local n = 0
        jobs.subscribe("full44", function() n = n + 1 end,
                       { types = { "enqueued" }, from = "beginning" })
        local ids = hooks.event_ids()
        local r = hooks.events_drain("full44", { grace = 0, batch = 1 })
        assert_eq(r.delivered, 1, "one")
        assert_eq(hooks.cursor("full44"), ids[1], "only the delivered one")
    end)
    test("the commit-order horizon bounds the move", function()
        jobs.subscribe("young44", function() end,
                       { types = { "cancelled" }, from = "now" })
        local before = hooks.cursor("young44")
        jobs.enqueue("t44", {}, { queue = "q44" })
        -- Every event is younger than an hour: nothing is past the horizon.
        hooks.events_drain("young44", { grace = 3600 })
        assert_eq(hooks.cursor("young44"), before, "held back")
    end)
    test("cleanup then reclaims the scanned events", function()
        local now = require("hull.time").now()
        hooks.events_drain("dead44", { grace = 0 })
        hooks.events_drain("canc44", { grace = 0 })
        jobs.unsubscribe("young44")
        jobs.unsubscribe("full44")
        jobs.cleanup({ before = now + 10 })
        local left = hooks.event_ids()
        assert_eq(#left, 1, "only the high-water event is left")
    end)
end

-- Plausible Medium: cleanup never deletes the newest job / event row.
function cases.jobs_high_water()
    local jobs = require("hull.jobs")
    local hooks = __test_hooks("hull.jobs")
    local now = require("hull.time").now()
    jobs.init({ events = true })
    test("the newest event and job rows survive a cleanup that covers all", function()
        for i = 1, 3 do jobs.enqueue("hw44", { i = i }) end
        local ev = hooks.event_ids()
        local jb = hooks.job_ids()
        jobs.cleanup({ before = now + 10, statuses = { "pending" } })
        local ev2, jb2 = hooks.event_ids(), hooks.job_ids()
        assert_eq(#ev2, 1, "one event left")
        assert_eq(ev2[1], ev[#ev], "the newest event")
        assert_eq(#jb2, 1, "one job left")
        assert_eq(jb2[1], jb[#jb], "the newest job")
    end)
    test("a subscription cursor at the head keeps the head too", function()
        jobs.subscribe("hw44", function() end, { from = "now" })
        jobs.enqueue("hw44", {})
        local ev = hooks.event_ids()
        hooks.events_drain("hw44", { grace = 0 })
        jobs.cleanup({ before = now + 10, statuses = { "pending" } })
        local ev2 = hooks.event_ids()
        assert_eq(ev2[#ev2], ev[#ev], "head kept")
    end)
end

-- Low: timestamp columns are BIGINT; a schedule past 2^31 is stored.
function cases.jobs_schedule_bigint()
    local jobs = require("hull.jobs")
    local hooks = __test_hooks("hull.jobs")
    jobs.init({ events = true })
    test("the timestamp columns are declared BIGINT", function()
        local cols = {
            { "_hull_jobs", "run_at" }, { "_hull_jobs", "claimed_at" },
            { "_hull_jobs", "created_at" }, { "_hull_jobs", "updated_at" },
            { "_hull_jobs", "claim_started_at" },
            { "_hull_cron", "next_run_at" }, { "_hull_cron", "last_run_at" },
            { "_hull_cron", "updated_at" }, { "_hull_job_events", "ts" },
            { "_hull_job_subscriptions", "lease_until" },
            { "_hull_job_subscriptions", "updated_at" },
            { "_hull_workflow_steps", "created_at" },
            { "_hull_workflow_signals", "created_at" },
            { "_hull_workflow_signals", "consumed_at" },
            { "_hull_ratelimit", "window_start" },
        }
        for _, c in ipairs(cols) do
            assert_eq(hooks.column_type(c[1], c[2]), "BIGINT", c[1] .. "." .. c[2])
        end
    end)
    test("a schedule past 2^31 is stored as given", function()
        local at = 2 ^ 40
        local id = jobs.enqueue("far44", {}, { at = at })
        assert_eq(jobs.get(id).run_at, math.tointeger(at), "run_at")
    end)
end

-- Low: event-type lists and queue scopes are checked.
function cases.jobs_types_queue()
    local jobs = require("hull.jobs")
    jobs.init({ events = true })
    test("jobs.events takes checked type lists", function()
        raises(function() jobs.events({ types = { 1 } }) end, "types", "number")
        raises(function() jobs.events({ types = { "a", [3] = "b" } }) end, "types", "hole")
        raises(function() jobs.events({ types = "dead" }) end, "types", "string")
        raises(function() jobs.events({ types = { "a,b" } }) end, "types", "comma")
        assert_eq(#jobs.events({ types = { "dead" } }), 0, "a good list")
        assert_eq(#jobs.events({ types = {} }), 0, "empty is no filter")
    end)
    test("jobs.subscribe takes checked type lists of at most 255 bytes", function()
        raises(function() jobs.subscribe("s", function() end, { types = { 1 } }) end, "types")
        raises(function() jobs.subscribe("s", function() end, { types = { "x,y" } }) end, "types")
        raises(function() jobs.subscribe("s", function() end, { types = { "" } }) end, "types")
        local long = {}
        for i = 1, 10 do long[i] = ("t"):rep(30) .. i end
        raises(function() jobs.subscribe("s", function() end, { types = long }) end, "255")
        jobs.subscribe("s", function() end, { types = { "dead", "cancelled" } })
    end)
    test("dead / stats / metrics check the queue scope", function()
        for _, f in ipairs({ jobs.dead, jobs.stats, jobs.metrics }) do
            raises(function() f({ queue = 5 }) end, "queue", "number")
            raises(function() f({ queue = {} }) end, "queue", "table")
            raises(function() f({ queue = ("q"):rep(256) }) end, "queue", "long")
        end
        jobs.enqueue("dq44", {}, { queue = "dq" })
        assert_eq(jobs.stats({ queue = "" }).pending, 1, "\"\" is every queue")
        assert_eq(jobs.stats({ queue = "dq" }).pending, 1, "a named queue")
        assert_eq(jobs.stats({ queue = "other" }).pending, 0, "another queue")
    end)
end

-- M11: the in-flight lease outlives a parked handler.
function cases.idem_inflight_lease()
    local idempotency = require("hull.web.middleware.idempotency")
    local H = __test_hooks("hull.web.middleware.idempotency")
    idempotency.init({ ttl = 60 })
    local mw = idempotency.middleware()
    test("an in-flight claim lasts the fetch ceiling plus a minute", function()
        local req = { method = "POST", path = "/a44", body = "{}",
                      headers = { ["idempotency-key"] = "k44" }, ctx = {} }
        local res = { status = function(s) return s end, json = function(s) return s end,
                      header = function(s) return s end, text = function(s) return s end }
        assert_eq(mw(req, res), 0, "claimed")
        local row = H.rows("k44")[1]
        assert_eq(row.state, "inflight", "state")
        assert_eq(row.expires_at - row.created_at, 660, "lease")
        assert_eq(H.inflight_lease, 660, "constant")
    end)
end

-- Low: respond encodes once, inside the store's guard.
function cases.idem_respond_encode()
    local idempotency = require("hull.web.middleware.idempotency")
    local H = __test_hooks("hull.web.middleware.idempotency")
    idempotency.init({ ttl = 600 })
    local mw = idempotency.middleware()
    local function mkres()
        local r = { headers = {} }
        function r.status(self, c) self.code = c; return self end
        function r.json(self, v) self.body = json.encode(v); self.via = "json"; return self end
        function r.text(self, v) self.body = v; self.via = "text"; return self end
        function r.html(self, v) self.body = v; return self end
        function r.header(self, k, v) self.headers[k] = v; return self end
        return r
    end
    local function claim(key)
        local req = { method = "POST", path = "/a44", body = "{}",
                      headers = { ["idempotency-key"] = key }, ctx = {} }
        assert_eq(mw(req, mkres()), 0, "claimed " .. key)
        return req
    end
    test("an encode error marks the key unstored and is raised", function()
        local cyc = {}
        cyc.self = cyc
        assert_eq(pcall(json.encode, cyc), false, "the value cannot be encoded")
        local req = claim("enc44")
        local ok = pcall(idempotency.respond, req, mkres(), 201, cyc)
        assert_eq(ok, false, "raised")
        assert_eq(H.rows("enc44")[1].state, "unstored", "not left in flight")
    end)
    test("the stored string is the one sent", function()
        local req = claim("ok44")
        local res = mkres()
        idempotency.respond(req, res, 201, { a = 1, b = "x" })
        assert_eq(res.code, 201, "status")
        assert_eq(res.via, "text", "the encoded string, not a second encode")
        assert_eq(res.headers["Content-Type"], "application/json", "content type")
        local row = H.rows("ok44")[1]
        assert_eq(row.state, "complete", "stored")
        local stored = require("hull.encoding").base64.decode(row.response_body)
        assert_eq(stored, res.body, "same bytes")
    end)
end

-- Low: read_to_file checks the blob's hash.
function cases.attachment_tamper()
    local blob = require("hull.blob")
    local fs = require("hull.fs")
    local attachment = require("hull.attachment")
    blob.init({ dir = "blobs" })
    attachment.init()
    local function part(bytes)
        return { filename = "a.txt", content_type = "text/plain",
                 chunks = function()
                     local done = false
                     return function()
                         if done then return nil end
                         done = true
                         return bytes
                     end
                 end }
    end
    test("a blob changed on disk is not written out", function()
        local id = attachment.store(part("hello attachment 44"))
        local meta = attachment.metadata(id)
        assert_eq(attachment.read_to_file(id, "out_ok.bin"), 19, "intact")
        assert_eq(fs.read("out_ok.bin"), "hello attachment 44", "bytes")
        local path
        for _, p in ipairs({ "blobs/blobs/" .. meta.blob_id:sub(1, 2) .. "/" .. meta.blob_id,
                             "blobs/" .. meta.blob_id:sub(1, 2) .. "/" .. meta.blob_id }) do
            if fs.stat(p) then path = p end
        end
        assert_eq(path ~= nil, true, "found the blob file")
        assert(fs.write(path, "HELLO attachment 44"))
        assert_eq(attachment.read(id), nil, "read refuses it")
        assert_eq(attachment.read_to_file(id, "out_bad.bin"), nil, "read_to_file refuses it")
        assert_eq(fs.stat("out_bad.bin"), nil, "nothing written")
    end)
end

-- Low: kv.open at its cap is a coded busy refusal (__kvn is the native
-- open, a fake backend behind it; see the C runner).
function cases.kv_open_busy()
    test("the cap refusal starts [hull:busy]", function()
        local open = {}
        for i = 1, 16 do open[i] = __kvn("redis://127.0.0.1:1") end
        local ok, err = pcall(__kvn, "redis://127.0.0.1:1")
        assert_eq(ok, false, "refused")
        assert_eq(tostring(err):sub(1, 12), "[hull:busy] ", tostring(err))
        for i = 1, 16 do open[i]:close() end
        local c = __kvn("redis://127.0.0.1:1")
        c:close()
    end)
end

-- Low: retry checks factor.
function cases.retry_factor()
    local retry = require("hull.retry")
    test("backoff and run take a finite factor >= 1", function()
        for _, bad in ipairs({ -2, 0, 0.5, 0 / 0, math.huge, "2", {} }) do
            raises(function() retry.backoff(2, { factor = bad }) end, "factor", tostring(bad))
            raises(function() retry.run(function() return 1 end, { factor = bad }) end,
                   "factor", "run " .. tostring(bad))
        end
        assert_eq(retry.backoff(3, { base_ms = 10, factor = 3 }), 90, "3^2 * 10")
        assert_eq(retry.backoff(3, { base_ms = 10, factor = 1 }), 10, "1")
        assert_eq(retry.backoff(2, { base_ms = 10 }), 20, "default 2")
    end)
end

-- Low: search parity with the JS twin.
function cases.search_parity()
    local search = require("hull.search")
    search.create_index("s44", { "title", "body" })
    search.index("s44", "doc1", { title = "hello world", body = "other words" })
    test("snippet and highlight default to column 1, and both are honoured", function()
        local rows = search.query("s44", "hello", { snippet = {}, highlight = {} })
        assert_eq(#rows, 1, "a hit")
        assert_eq(rows[1].snippet ~= nil and rows[1].snippet:find("<b>hello</b>", 1, true) ~= nil,
                  true, "snippet of the title: " .. tostring(rows[1].snippet))
        assert_eq(rows[1].highlight, "<b>hello</b> world", "highlight of the title")
    end)
    test("index refuses no column and non-string values", function()
        raises(function() search.index("s44", "d2", {}) end, "at least one column")
        raises(function() search.index("s44", "d2", { title = true }) end, "strings or numbers")
        raises(function() search.index("s44", "d2", { title = {} }) end, "strings or numbers")
        search.index("s44", "d3", { title = 44 })
        assert_eq(#search.query("s44", "44"), 1, "a number is indexed")
    end)
    test("max_query_len counts bytes", function()
        raises(function() search.query("s44", ("\u{e9}"):rep(3), { max_query_len = 5 }) end,
               "too long")
    end)
end

-- Low: inbox source parity.
function cases.inbox_source()
    local inbox = require("hull.web.middleware.inbox")
    inbox.init()
    test("an empty source is the default source", function()
        inbox.mark_processed("m44", "")
        assert_eq(inbox.is_duplicate("m44"), true, "seen under the default source")
        assert_eq(inbox.is_duplicate("m44", "default"), true, "named default")
        assert_eq(inbox.check_and_mark("m45", ""), false, "first")
        assert_eq(inbox.check_and_mark("m45"), true, "the same source")
    end)
end

-- Low: outbox cleanup / empty key parity.
function cases.outbox_cleanup()
    local outbox = require("hull.web.middleware.outbox")
    local H = __test_hooks("hull.web.middleware.outbox")
    outbox.init()
    test("cleanup checks max_age and takes also_failed", function()
        for _, bad in ipairs({ -1, 0 / 0, math.huge, "60", {} }) do
            raises(function() outbox.cleanup(bad) end, "max_age", tostring(bad))
        end
        assert_eq(outbox.cleanup(0, { also_failed = true }), 0, "also_failed accepted")
    end)
    test("an empty idempotency key is stored as NULL", function()
        local id = outbox.enqueue({ kind = "webhook", destination = "https://a44.test/h",
                                    payload = "{}", idempotency_key = "" })
        assert_eq(H.rows(id)[1].idempotency_key, nil, "NULL")
    end)
end

test("the case is known", function()
    assert_eq(type(cases[__data44_case]), "function", tostring(__data44_case))
end)
if cases[__data44_case] then cases[__data44_case]() end

return { pass = pass, fail = fail }
