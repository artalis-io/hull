-- luacheck: globals T
-- The build host's side of a build plugin (docs/buildcontext_design.md
-- §9.1, §12 "Single pipeline and plugin parity"): real plugin processes -
-- the hull binary T.hull running tests/fixtures/build_host_{lua,js} - driven
-- through hull.buildctx.plugin into real actions; then the protocol paths a
-- real plugin cannot produce, over a scripted process. Run by
-- tests/hull/buildctx/test_buildctx.c, which provides T and a temporary HOME.

local action = require("hull.buildctx.action")
local plugin = require("hull.buildctx.plugin")
local native = require("hull.buildctx._plugin")

local pass, fail = 0, 0
local function ok(cond, name)
    if cond then pass = pass + 1 else fail = fail + 1; io.stderr:write("  FAIL: " .. name .. "\n") end
end
local function eq(a, b, name)
    ok(a == b, name .. " (got " .. tostring(a) .. ", want " .. tostring(b) .. ")")
end

local roots = {}
local function tmp(name) local d = T.tmpdir(name); roots[#roots + 1] = d; return d end
local root = tmp("plugin_ws")
T.mkdir(root .. "/src")
T.mkdir(root .. "/empty")
local f = assert(io.open(root .. "/src/a.txt", "wb")); f:write("hello"); f:close()

local nkey = 0
local function key() nkey = nkey + 1; return ("%064x"):format(nkey + 5000) end
local function new_action(store, k)
    return assert(action.new({ root = root, grants = { "src/", "empty/" }, key = k or key(),
                               store = store }))
end

local function req(id, mode)
    return { id = id, config = { greeting = "hi" }, meta = { mode = mode or "full" },
             source = { units = { "a.lua" } } }
end

local fixtures = {
    lua = T.cwd .. "/tests/fixtures/build_host_lua/app.lua",
    js  = T.cwd .. "/tests/fixtures/build_host_js/app.js",
}

-- ── real processes ───────────────────────────────────────────────────

local probe = native.spawn(T.hull, fixtures.lua, plugin.PLUGIN_ARGS)
if probe == nil then
    io.stderr:write("  (no plugin channel on this host: real-process legs skipped)\n")
else
    probe:kill()
    local results, k_shared, fds = {}, key(), {}
    for _, lang in ipairs({ "lua", "js" }) do
        local store = assert(require("hull.buildctx._store").open(tmp("plugin_store_" .. lang)))
        local r = assert(plugin.new({ exe = T.hull, entry = fixtures[lang] }))

        local a = new_action(store, k_shared)
        local done, tok, detail = r:run(a, req(1))
        ok(done, lang .. ": a transform commits (" .. tostring(tok) .. " " .. tostring(detail) .. ")")
        results[lang] = a:get_result()
        local res = action.lookup(store, k_shared, root)
        ok(res ~= nil, lang .. ": the committed result is visible")
        if res then
            eq(#res.outputs, 2, lang .. ": two outputs")
            local blob, tree
            for _, o in ipairs(res.outputs) do
                if o.declared == "generated" then blob = o else tree = o end
            end
            eq(blob and blob.bytes, "hi:hello:5:1:0:1", lang .. ": the blob, from every input op")
            eq(tree and tree.entries[1].path, "x/y.txt", lang .. ": the tree")
            eq(#res.diagnostics, 1, lang .. ": one diagnostic")
        end

        a = new_action(store)
        done, tok, detail = r:run(a, req(2, "fail"))
        ok(not done and tok == "plugin_failed" and tostring(detail):find("boom", 1, true),
           lang .. ": a raising transform aborts its action (" .. tostring(tok) .. ")")
        eq(a:get_state(), "aborted", lang .. ": the failed action is aborted")

        a = new_action(store)
        ok(r:run(a, req(3)), lang .. ": the next action runs on the same process")
        eq(r:spawn_count(), 1, lang .. ": one process served three actions")

        a = new_action(store)
        tok = select(2, r:run(a, req(4, "undeclared")))
        eq(tok, "undeclared_payload", lang .. ": prepare refuses an undeclared payload")

        a = new_action(store)
        a:cancel()
        tok = select(2, r:run(a, req(5)))
        eq(tok, "cancelled", lang .. ": a cancelled action does not run")

        -- The plugin's own budget stops a spinning transform: the process
        -- exits, the action aborts, and the next action gets a new process.
        a = new_action(store)
        tok = select(2, r:run(a, req(6, "spin")))
        eq(tok, "plugin_crashed", lang .. ": a process that dies aborts its action")
        a = new_action(store)
        ok(r:run(a, req(7)), lang .. ": a fresh process serves the next action")
        eq(r:spawn_count(), 2, lang .. ": the dead process was replaced")
        eq(r:close(), 0, lang .. ": STOP ends the process cleanly")
        eq(r:close(), nil, lang .. ": closing twice is a no-op")

        -- The host's deadline: one millisecond, gone before the process has
        -- even loaded.
        local quick = assert(plugin.new({ exe = T.hull, entry = fixtures[lang],
                                          limits = { action_ms = 1 } }))
        a = new_action(store)
        tok = select(2, quick:run(a, req(8)))
        eq(tok, "plugin_timeout", lang .. ": the host's deadline aborts a slow action")
        eq(quick:close(), nil, lang .. ": the timed-out process is already gone")
        store:close()
        collectgarbage(); collectgarbage()
        fds[lang] = T.open_fds()
    end
    eq(results.lua, results.js, "Lua and JavaScript plugins commit byte-identical results")
    -- The first pass may open process-wide descriptors once (a key, a random
    -- source); the second, with as many processes, must leave none.
    eq(fds.js, fds.lua, "no descriptor is left behind by a plugin process")
end

-- ── a scripted process: protocol paths ───────────────────────────────

-- A process whose frames are scripted: each recv answers the next entry
-- ({ type, id, value } or { nil, token }); sends are recorded.
local function scripted(script)
    local sent, killed = {}, { n = 0 }
    local fake = setmetatable({
        spawn = function()
            local i = 0
            return {
                send = function(_, t, id, v) sent[#sent + 1] = { t, id, v }; return true end,
                recv = function()
                    i = i + 1
                    local e = script[i] or { nil, "eof" }
                    return e[1], e[2], e[3]
                end,
                kill = function() killed.n = killed.n + 1 end,
                close = function() return 0 end,
            }
        end,
    }, { __index = native })
    return fake, sent, killed
end

local store = assert(require("hull.buildctx._store").open(tmp("plugin_store_fake")))
local function run_script(script)
    local fake, sent, killed = scripted(script)
    local r = assert(plugin.new({ exe = "/x", entry = "/x/app.lua" }, { native = fake }))
    local a = new_action(store)
    local done, tok, detail = r:run(a, req(1))
    return done, tok, detail, sent, killed, a
end
local CALL, DONE, REPLY = native.CALL, native.DONE, native.REPLY

do
    local done, tok, _, sent, killed = run_script({ { REPLY, 1, {} } })
    ok(not done and tok == "plugin_protocol", "a frame out of turn is a protocol error")
    eq(killed.n, 1, "and the process is killed")
    eq(sent[1][1], native.ACTION, "the action was sent first")
end

do
    local done, tok, _, _, killed = run_script({ { nil, "malformed" } })
    ok(not done and tok == "plugin_protocol", "a malformed value is a protocol error")
    eq(killed.n, 1, "and the process is killed")
end

do
    local done, tok = run_script({ { DONE, 0, { error = 7 } } })
    ok(not done and tok == "plugin_protocol", "a DONE whose error is not a string")
end

do
    local _, tok, _, _, killed = run_script({})
    eq(tok, "plugin_crashed", "a process that closes the channel crashed")
    eq(killed.n, 1, "and is killed")
end

do
    local _, tok, _, _, killed = run_script({ { nil, "timeout" } })
    eq(tok, "plugin_timeout", "no DONE before the deadline")
    eq(killed.n, 1, "and the slow process is killed")
end

do
    local done, _, _, sent = run_script({
        { CALL, 11, { op = "no.such" } },
        { CALL, 12, { op = "blob.write", args = { handle = 1, bytes = "x" } } },
        { CALL, 13, { op = "inputs.read", args = { path = 7 } } },
        { CALL, 14, { op = "inputs.read", args = "src/a.txt" } },
        { CALL, 15, { op = "outputs.create_blob", args = { name = "b" } } },
        { CALL, 16, { op = "tree.write", args = { handle = 1, path = "a", bytes = "x" } } },
        { CALL, 17, { op = "blob.write", args = { handle = 1, bytes = "x" } } },
        { CALL, 18, { op = "source.erased_source", args = { unit = "a.ts" } } },
        { CALL, 19, { op = "inputs.list", args = { path = "empty" } } },
        { CALL, 20, { op = "blob.close", args = { handle = 1 } } },
        { CALL, 21, { op = "outputs.declare_generated_output", args = { descriptor = { name = "./b" },
                                                                         handle = 1 } } },
        { DONE, 0, {} },
    })
    ok(done, "the scripted action commits")
    local function reply(id)
        for _, s in ipairs(sent) do if s[1] == REPLY and s[2] == id then return s[3] end end
    end
    eq(reply(11).error, "unknown_op", "an unknown op")
    eq(reply(12).error, "invalid_handle", "a handle the host never gave")
    eq(reply(13).error, "invalid_args", "a path that is not a string")
    eq(reply(14).error, "invalid_args", "args that are not a map")
    eq(reply(15).ok, 1, "create_blob answers the handle's id")
    eq(reply(16).error, "invalid_handle", "a blob's id used as a tree")
    ok(reply(17).ok == nil and reply(17).error == nil, "a write answers nothing")
    eq(reply(18).error, "unsupported_op", "no frontend session: erased_source is unsupported")
    eq(reply(19).ok, native.empty_array, "an empty listing is an empty array")
end

do
    local fake = scripted({ { CALL, 1, { op = "inputs.read", args = { path = "src/a.txt" } } } })
    local r = assert(plugin.new({ exe = "/x", entry = "/x/app.lua" }, { native = fake }))
    local a = new_action(store)
    a.read = function() error("host bug") end
    local done, tok, detail = r:run(a, req(1))
    ok(not done and tok == "host_error" and tostring(detail):find("host bug", 1, true),
       "an operation that raises is a host error")
    eq(a:get_state(), "aborted", "and aborts the action")
end

-- A handle id names its action: the next action cannot use it.
do
    local fake, sent = scripted({
        { CALL, 1, { op = "outputs.create_blob", args = { name = "b" } } },
        { CALL, 2, { op = "blob.close", args = { handle = 1 } } },
        { CALL, 3, { op = "outputs.declare_generated_output", args = { descriptor = { name = "./b" },
                                                                        handle = 1 } } },
        { DONE, 0, {} },
        { CALL, 4, { op = "blob.write", args = { handle = 1, bytes = "x" } } },
        { DONE, 0, {} },
    })
    local r = assert(plugin.new({ exe = "/x", entry = "/x/app.lua" }, { native = fake }))
    ok(r:run(new_action(store), req(1)), "first action")
    ok(r:run(new_action(store), req(2)), "second action")
    local last
    for _, s in ipairs(sent) do if s[1] == REPLY and s[2] == 4 then last = s[3] end end
    eq(last and last.error, "invalid_handle", "an earlier action's handle is refused")
end

-- erased_source answered by the caller's frontend session.
do
    local fake, sent = scripted({
        { CALL, 1, { op = "source.erased_source", args = { unit = "a.ts" } } },
        { CALL, 2, { op = "source.erased_source", args = { unit = "b.ts" } } },
        { CALL, 3, { op = "source.erased_source", args = { unit = "c.ts" } } },
        { CALL, 4, { op = "source.erased_source", args = { unit = "d.ts" } } },
        { CALL, 5, { op = "source.erased_source", args = { unit = 7 } } },
        { DONE, 0, {} },
    })
    local r = assert(plugin.new({ exe = "/x", entry = "/x/app.lua" }, { native = fake }))
    local rq = req(1)
    local refusal = { severity = "error", code = "ts.unsupported", message = "no", path = "c.ts" }
    rq.source.erased_source = function(u)
        if u == "a.ts" then return { bytes = "let x = 1;", refusals = {} } end
        if u == "c.ts" then return { refusals = { refusal } } end
        if u == "d.ts" then return { refusals = {} } end
        return nil, "not_a_unit"
    end
    ok(r:run(new_action(store), rq), "erased_source action")
    local got = {}
    for _, s in ipairs(sent) do if s[1] == REPLY then got[s[2]] = s[3] end end
    ok(type(got[1].ok) == "table" and type(got[1].ok.bytes) == "userdata", "the erased source is bytes")
    ok(got[1].ok and got[1].ok.refusals == fake.empty_array, "with no refusals, as an empty array")
    eq(got[2].error, "not_a_unit", "the session's token")
    ok(got[3].ok and got[3].ok.bytes == nil, "a refused unit has no bytes")
    eq(got[3].ok and got[3].ok.refusals[1].code, "ts.unsupported", "and its refusals")
    eq(got[4].error, "io_error", "neither bytes nor refusals is a host fault")
    eq(got[5].error, "invalid_args", "a unit id is a string")
end

store:close()
for _, r in ipairs(roots) do T.rmtree(r) end
return { pass = pass, fail = fail }
