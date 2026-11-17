-- luacheck: globals T
-- BuildContext ctx.outputs, diagnostics, the committed result and
-- its lookup, over the real hull.buildctx._store (docs/buildcontext_design.md
-- §12, "Action transaction" and "Outputs"). Run by tests/hull/buildctx/
-- test_buildctx.c, which provides T and a temporary HOME (the seal key).

local action = require("hull.buildctx.action")

local pass, fail = 0, 0
local function ok(cond, name)
    if cond then pass = pass + 1 else fail = fail + 1; io.stderr:write("  FAIL: " .. name .. "\n") end
end
local function eq(a, b, name)
    ok(a == b, name .. " (got " .. tostring(a) .. ", want " .. tostring(b) .. ")")
end

local roots = {}
local function tmp(name) local d = T.tmpdir(name); roots[#roots + 1] = d; return d end
local root = tmp("ws")
T.mkdir(root .. "/src")
local function put(rel, data) local f = assert(io.open(root .. "/" .. rel, "wb")); f:write(data); f:close() end
put("src/a.ts", "let x: number = 1;\n")

local store = assert(require("hull.buildctx._store").open(tmp("store")))
local nkey = 0
local function key() nkey = nkey + 1; return ("%064x"):format(nkey + 1000) end

local function started(st, k)
    local a = assert(action.new({ root = root, grants = { "src/" }, key = k or key(), store = st or store }))
    assert(a:start())
    return a
end

-- One blob output and one tree artifact, from a read input.
local function build(a)
    local src = assert(a:read("src/a.ts"))
    local w = assert(a:create_blob("app"))
    assert(w:write((src:gsub(": number", "        "))))
    assert(w:close())
    assert(a:declare_generated_output({ name = "./src/a.ts" }, w))
    local t = assert(a:create_tree("assets"))
    assert(t:write("css/site.css", "body{}", "regular"))
    assert(t:write("bin/run", "#!/bin/sh\n", "executable"))
    assert(t:close())
    assert(a:declare_artifact({ name = "assets", kind = "static-tree" }, t))
    assert(a:emit({ severity = "warning", code = "demo.note", message = "hello", path = "src/a.ts",
                    range = { start = 4, stop = 5 } }))
end

-- ── the transaction ──────────────────────────────────────────────────

do
    local k = key()
    local a = started(store, k)
    build(a)
    local _, why = action.lookup(store, k, root); eq(why, "miss", "nothing visible while running")
    ok(a:prepare(), "prepare")
    _, why = action.lookup(store, k, root); eq(why, "miss", "nothing visible after prepare")
    ok(a:commit(), "commit")
    local r = action.lookup(store, k, root)
    ok(r ~= nil, "the committed result is found")
    eq(#r.outputs, 2, "both outputs, as one result")
    local gen, art
    for _, o in ipairs(r.outputs) do
        if o.declared == "generated" then gen = o else art = o end
    end
    eq(gen.descriptor.name, "./src/a.ts", "generated output named for its source")
    eq(gen.bytes, (("let x: number = 1;\n"):gsub(": number", "        ")), "committed bytes are the staged bytes")
    eq(#art.entries, 2, "the tree's two files")
    eq(art.entries[1].path, "bin/run", "tree entries sorted by path")
    eq(art.entries[1].mode, "executable", "mode kept")
    eq(art.entries[2].bytes, "body{}", "tree bytes")
    eq(#r.diagnostics, 1, "the warning is recorded")
    eq(r.diagnostics[1].code, "demo.note", "warning code")

    -- the hashes are Hull's, from the staged bytes
    local res = a:get_result()
    ok(res:find(store:put_payload(gen.bytes), 1, true) ~= nil, "payload hash computed from the bytes")
    ok(a.publish == nil and a.install == nil and a.rename_to_workspace == nil, "no publication primitive")

    -- stale handles
    local _, e = a:create_blob("late"); eq(e, "stale_handle", "no output after commit")

    -- an input change makes the result stale; changing it back makes it fresh
    put("src/a.ts", "let x: number = 2;\n")
    _, why = action.lookup(store, k, root); eq(why, "stale", "input changed -> stale")
    put("src/a.ts", "let x: number = 1;\n")
    ok(action.lookup(store, k, root) ~= nil, "input back -> fresh")
end

do  -- every refusal leaves no visible result
    local cases = {
        { "open writer", function(a) a:create_blob("w") end, "open_writer" },
        { "undeclared payload", function(a) local w = a:create_blob("w"); w:close() end, "undeclared_payload" },
        { "error diagnostic", function(a) a:emit({ severity = "error", code = "x", message = "bad" }) end,
          "diagnostic_error" },
        { "failed input", function(a) a:read("../x"); a.failed = true end, "input_failed" },
        { "cancelled", function(a) a:cancel() end, "cancelled" },
    }
    for _, c in ipairs(cases) do
        local k = key()
        local a = started(store, k)
        c[2](a)
        local _, e = a:prepare()
        eq(e, c[3], c[1] .. " refuses prepare")
        eq(a:get_state(), "aborted", c[1] .. " aborts")
        local _, why = action.lookup(store, k, root)
        eq(why, "miss", c[1] .. " leaves no result")
    end
    local k = key()
    local a = started(store, k)
    build(a)
    assert(a:prepare())
    a:abort()
    local _, why = action.lookup(store, k, root)
    eq(why, "miss", "abort after prepare leaves no result")
end

do  -- a store that fails part-way rolls the whole result back
    local function flaky(fail_payload_at, fail_result)
        local n = 0
        return setmetatable({
            put_payload = function(_, b)
                n = n + 1
                if n == fail_payload_at then return nil, "io_error" end
                return store:put_payload(b)
            end,
            put_result = function(_, k, b)
                if fail_result then return nil, "io_error" end
                return store:put_result(k, b)
            end,
        }, { __index = function(_, m) return function(_, ...) return store[m](store, ...) end end })
    end
    for at = 1, 4 do
        local k = key()
        local a = started(flaky(at, false), k)
        build(a)
        local _, e = a:prepare()
        eq(e, "store_failed", "payload " .. at .. " failing fails prepare")
        local _, why = action.lookup(store, k, root)
        eq(why, "miss", "payload " .. at .. " failing: no result")
    end
    local k = key()
    local a = started(flaky(0, true), k)
    build(a)
    assert(a:prepare())
    local _, e = a:commit()
    eq(e, "store_failed", "a failed install fails commit")
    eq(a:get_state(), "aborted", "and aborts")
    local _, why = action.lookup(store, k, root)
    eq(why, "miss", "a failed install: no result")
end

-- ── outputs: generic constraints (§6) ────────────────────────────────

do
    local a = started()
    local w = assert(a:create_blob("one"))
    local _, e = a:create_blob("one"); eq(e, "duplicate_payload", "payload names are unique")
    for _, bad in ipairs({ "../x", "/abs", "a//b", "a/", "./a", "", "a/\1" }) do
        _, e = a:create_blob(bad); eq(e, "invalid_name", "bad payload name " .. ("%q"):format(bad))
    end
    w:close()
    _, e = a:declare_generated_output({ name = "../escape.ts" }, w); eq(e, "invalid_descriptor", "escaping output name")
    _, e = a:declare_generated_output({ name = "a.ts", n = 1.5 }, w); eq(e, "invalid_descriptor", "a float in a descriptor")
    ok(a:declare_generated_output({ name = "a.ts" }, w), "declare")
    _, e = a:declare_generated_output({ name = "a.ts" }, w); eq(e, "already_declared", "a payload declared once")
    local w2 = assert(a:create_blob("two")); w2:close()
    _, e = a:declare_generated_output({ name = "./a.ts" }, w2); ok(e == nil, "./a.ts and a.ts are distinct names")

    local t = assert(a:create_tree("t"))
    for _, bad in ipairs({ "../x", "/abs", "a//b", "x/" }) do
        _, e = t:write(bad, "x"); eq(e, "invalid_path", "bad tree path " .. ("%q"):format(bad))
    end
    assert(t:write("d/f", "1"))
    _, e = t:write("d/f", "2"); eq(e, "duplicate_path", "duplicate tree path")
    _, e = t:write("d", "3"); eq(e, "path_conflict", "a file where a directory is")
    _, e = t:write("d/f/g", "4"); eq(e, "path_conflict", "a directory where a file is")
    _, e = t:write("m", "x", "setuid"); eq(e, "invalid_mode", "only regular and executable")
    t:close()
    _, e = t:write("late", "x"); eq(e, "closed", "no write after close")
    local o = {}
    _, e = a:declare_artifact({ name = "x" }, o); eq(e, "invalid_handle", "a handle from elsewhere")
    _, e = a:emit({ severity = "fatal", code = "x", message = "m" }); eq(e, "invalid_diagnostic", "unknown severity")
    _, e = a:emit({ severity = "info", code = "x", message = "m", range = { start = 5, stop = 2 } })
    eq(e, "invalid_diagnostic", "inverted range")
end

do  -- limits
    local a = assert(action.new({ root = root, grants = { "src/" }, key = key(), store = store,
                                  limits = { max_output_bytes = 10, max_output_files = 2, max_tree_depth = 2 } }))
    a:start()
    local w = assert(a:create_blob("b"))
    ok(w:write("12345"), "under the byte limit")
    local _, e = w:write("123456"); eq(e, "limit", "past max_output_bytes")
    local t = assert(a:create_tree("t"))
    _, e = t:write("a/b/c", "x"); eq(e, "limit", "past max_tree_depth")
    ok(t:write("a/b", "x"), "one file")
    _, e = t:write("a/c", "x"); eq(e, "limit", "past max_output_files (the blob counts)")
end

-- ── determinism, races, the store ────────────────────────────────────

do
    local function result_of(order)
        local a = started()
        local w1 = a:create_blob("x"); w1:write("X"); w1:close()
        local w2 = a:create_blob("y"); w2:write("Y"); w2:close()
        if order == 1 then
            a:declare_generated_output({ name = "x.ts" }, w1)
            a:declare_generated_output({ name = "y.ts" }, w2)
        else
            a:declare_generated_output({ name = "y.ts" }, w2)
            a:declare_generated_output({ name = "x.ts" }, w1)
        end
        assert(a:prepare())
        local r = a:get_result():gsub('"key":"%x+"', "")
        return r
    end
    eq(result_of(1), result_of(2), "declaration order does not change the result")

    local k = key()
    for i = 1, 2 do   -- two actions, one key, the same content: both commit, one result
        local a = started(store, k)
        build(a)
        assert(a:prepare())
        ok(a:commit(), "concurrent commit " .. i)
    end
    ok(action.lookup(store, k, root) ~= nil, "the shared key resolves")

    local a = started()                   -- an empty payload is a payload
    local w = a:create_blob("e"); w:close()
    a:declare_generated_output({ name = "empty.ts" }, w)
    assert(a:prepare()); assert(a:commit())
    eq(action.lookup(store, a.key, root).outputs[1].bytes, "", "an empty output round-trips")

    local sha = store:put_payload("payload bytes")
    eq(select(2, store:get_result(sha)), "miss", "a payload never reads as a result")
    eq(select(2, store:get_payload(("0"):rep(64))), "miss", "absent payload")
    eq(select(2, store:get_payload("zz")), "invalid_args", "bad payload key")
end

do  -- a result whose payload went missing is corrupt, not a hit
    local k = key()
    local a = started(store, k)
    build(a)
    assert(a:prepare()); assert(a:commit())
    local holes = setmetatable({
        get_payload = function(_, s)
            if s == a.result:match('"declared":"artifact".-"sha256":"(%x+)"') then return nil, "miss" end
            return store:get_payload(s)
        end,
    }, { __index = function(_, m) return function(_, ...) return store[m](store, ...) end end })
    local _, why = action.lookup(holes, k, root)
    eq(why, "corrupt", "a missing payload")
end

for _, r in ipairs(roots) do T.rmtree(r) end
return { pass = pass, fail = fail }
