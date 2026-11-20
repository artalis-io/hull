-- luacheck: globals T
-- Audit 49: the edges of a build plugin's channel, through real plugin
-- processes (tests/fixtures/build_host_{lua,js}) and a scripted one. Run by
-- tests/hull/buildctx/test_buildctx.c, one case per UTEST (T.case).

local action = require("hull.buildctx.action")
local plugin = require("hull.buildctx.plugin")
local native = require("hull.buildctx._plugin")

local pass, fail = 0, 0
local function ok(cond, name)
    if cond then pass = pass + 1 else fail = fail + 1; io.stderr:write("  FAIL: " .. name .. "\n") end
end

local roots = {}
local function tmp(name) local d = T.tmpdir(name); roots[#roots + 1] = d; return d end
local root = tmp("a49_ws")
T.mkdir(root .. "/src")
local f = assert(io.open(root .. "/src/a.txt", "wb")); f:write("hello"); f:close()

local nkey = 0
local function new_action(store)
    nkey = nkey + 1
    return assert(action.new({ root = root, grants = { "src/" }, key = ("%064x"):format(nkey + 9000),
                               store = store }))
end
local function req(mode)
    return { id = 1, config = { greeting = "hi" }, meta = { mode = mode }, source = { units = { "a.lua" } } }
end

local fixtures = {
    lua = T.cwd .. "/tests/fixtures/build_host_lua/app.lua",
    js  = T.cwd .. "/tests/fixtures/build_host_js/app.js",
}

-- Run MODE in both runtimes: check(lang, done, tok, detail, ms).
local function each_runtime(mode, check)
    local probe = native.spawn(T.hull, fixtures.lua, plugin.PLUGIN_ARGS)
    if probe == nil then
        io.stderr:write("  (no plugin channel on this host: skipped)\n")
        pass = pass + 1
        return
    end
    probe:kill()
    for _, lang in ipairs({ "lua", "js" }) do
        local store = assert(require("hull.buildctx._store").open(tmp("a49_store_" .. lang)))
        -- A short deadline: a plugin that never sends DONE costs 10 s, not 120.
        local r = assert(plugin.new({ exe = T.hull, entry = fixtures[lang],
                                      limits = { action_ms = 10000 } }))
        local t0 = native.now()
        local done, tok, detail = r:run(new_action(store), req(mode))
        check(lang, done, tok, detail, native.now() - t0)
        r:close()
        store:close()
    end
end

local case = T.case

if case == "big_error_reported" then
    -- A transform error past 64 KiB: the DONE went unsent and the host timed
    -- out with the error lost. It is cut to 4 KiB, on a UTF-8 boundary.
    each_runtime("bigerr", function(lang, done, tok, detail, ms)
        ok(not done and tok == "plugin_failed", lang .. ": plugin_failed (got " .. tostring(tok) .. ")")
        -- The cut falls inside the two-byte character at 4095: it goes whole.
        ok(detail == string.rep("A", 4095) .. "...", lang .. ": the error, cut before the "
           .. "character it would split (" .. #tostring(detail) .. " bytes)")
        ok(ms < 5000, lang .. ": reported at once (" .. ms .. " ms)")
    end)
elseif case == "nil_error_fails" then
    -- Lua error(nil) committed the action as a success; a JS value whose
    -- toString throws left the action loop without a DONE.
    each_runtime("nilerr", function(lang, done, tok, detail)
        ok(not done and tok == "plugin_failed",
           lang .. ": a raised nil / unprintable value fails the action (got " .. tostring(tok) .. ")")
        ok(type(detail) == "string" and detail ~= "", lang .. ": with a text (" .. tostring(detail) .. ")")
    end)
elseif case == "deep_args_refused_cleanly" then
    -- One level past the host's depth check: the plugin's encoder took it
    -- (it counted args from 0) and the host killed the plugin as malformed.
    each_runtime("deep", function(lang, done, tok, detail)
        ok(not done and tok == "plugin_failed" and tostring(detail):find("too_deep", 1, true),
           lang .. ": too_deep in the plugin (got " .. tostring(tok) .. " " .. tostring(detail) .. ")")
    end)
elseif case == "shared_value_bounded" then
    -- A DAG of a million shared empty tables: small in the plugin, millions
    -- of tables decoded on the host. The item count bounds it.
    each_runtime("wide", function(lang, done, tok, detail)
        ok(not done and tok == "plugin_failed" and tostring(detail):find("too_big", 1, true),
           lang .. ": refused as too_big (got " .. tostring(tok) .. " " .. tostring(detail) .. ")")
    end)
elseif case == "recv_raise_contained" then
    -- A frame whose decode raises (the tool VM out of memory) fails that
    -- plugin; it used to raise out of the whole `hull plugins run`.
    local killed = 0
    local fake = setmetatable({
        spawn = function()
            return {
                send = function() return true end,
                recv = function() error("not enough memory") end,
                kill = function() killed = killed + 1 end,
                close = function() return 0 end,
            }
        end,
    }, { __index = native })
    local store = assert(require("hull.buildctx._store").open(tmp("a49_store_fake")))
    local r = assert(plugin.new({ exe = "/x", entry = "/x/app.lua" }, { native = fake }))
    local fine, done, tok, detail = pcall(r.run, r, new_action(store), req("x"))
    ok(fine, "run does not raise (" .. tostring(done) .. ")")
    ok(fine and not done and tok == "plugin_protocol" and tostring(detail):find("not enough memory", 1, true),
       "the plugin fails as plugin_protocol (" .. tostring(tok) .. ")")
    ok(killed == 1, "and its process is killed")
    store:close()
else
    ok(false, "unknown case " .. tostring(case))
end

for _, d in ipairs(roots) do T.rmtree(d) end
return { pass = pass, fail = fail }
