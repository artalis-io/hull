-- BuildContext checkpoint 1: the action core and ctx.inputs over the real
-- hull.buildctx._fs binding (docs/buildcontext_design.md §12, "Action
-- transaction" and "Workspace inputs"). Run by tests/hull/buildctx/test_buildctx.c,
-- which provides T (tmpdir, mkdir, symlink, set_mtime, chmod, open_fds).

-- luacheck: globals T
local action = require("hull.buildctx.action")

local pass, fail = 0, 0
local function ok(cond, name)
    if cond then pass = pass + 1 else fail = fail + 1; io.stderr:write("  FAIL: " .. name .. "\n") end
end
local function eq(a, b, name)
    ok(a == b, name .. " (got " .. tostring(a) .. ", want " .. tostring(b) .. ")")
end

local root
local roots = {}
local function fresh(name) root = T.tmpdir(name); roots[#roots + 1] = root end
local function put(rel, data) local f = assert(io.open(root .. "/" .. rel, "wb")); f:write(data); f:close() end
local function dir(rel) T.mkdir(root .. "/" .. rel) end
local function rm(rel) os.remove(root .. "/" .. rel) end

local function started(grants, limits)
    local a, err = action.new({ root = root, grants = grants, limits = limits })
    assert(a, err)
    assert(a:start())
    return a
end

-- ── the transaction ──────────────────────────────────────────────────

do
    fresh("lifecycle")
    put("a.txt", "hello")
    local a = assert(action.new({ root = root, grants = { "a.txt" } }))
    eq(a:get_state(), "created", "created")
    local _, e = a:read("a.txt"); eq(e, "bad_state", "no input before start")
    _, e = a:prepare(); eq(e, "bad_state", "no prepare before start")
    ok(a:start(), "start")
    _, e = a:start(); eq(e, "bad_state", "start is once")
    _, e = a:commit(); eq(e, "bad_state", "no commit before prepare")
    eq(a:read("a.txt"), "hello", "read")
    ok(a:prepare(), "prepare")
    _, e = a:read("a.txt"); eq(e, "bad_state", "inputs frozen at prepare")
    ok(a:commit(), "commit")
    eq(a:get_state(), "committed", "committed")
    _, e = a:read("a.txt"); eq(e, "stale_handle", "input after commit is stale")
    ok(a:abort(), "abort once terminal is a no-op")
    eq(a:get_state(), "committed", "still committed")
    ok(a:deps() ~= nil, "manifest after commit")
end

do
    fresh("abort")
    put("a.txt", "x")
    for stop = 0, 2 do
        local a = assert(action.new({ root = root, grants = { "a.txt" } }))
        if stop >= 1 then a:start() end
        if stop >= 2 then a:prepare() end
        a:abort()
        eq(a:get_state(), "aborted", "abort from step " .. stop)
        local _, e = a:commit(); eq(e, "bad_state", "no commit after abort " .. stop)
        _, e = a:deps(); eq(e, "bad_state", "no result after abort " .. stop)
    end
end

do
    fresh("cancel")
    put("a.txt", "x")
    for at = 1, 3 do
        local a = started({ "a.txt" })
        if at == 3 then a:prepare() end
        a:cancel()
        local _, e
        if at == 1 then _, e = a:read("a.txt") elseif at == 2 then _, e = a:prepare() else _, e = a:commit() end
        eq(e, "cancelled", "cancelled at host call " .. at)
        eq(a:get_state(), "aborted", "cancel aborts " .. at)
    end
end

-- ── inputs: authority ────────────────────────────────────────────────

do
    fresh("deny")
    dir("src"); put("src/a.lua", "a"); put("secret.txt", "s")
    dir(".hull"); put(".hull/x", "x")
    local a = started({ "src/" })
    local _, e = a:read("secret.txt"); eq(e, "permission", "undeclared path denied")
    for _, p in ipairs({ "/etc/passwd", "../x", "src/../secret.txt", "./src/a.lua",
                         "src//a.lua", "src/a.lua/", "src/./a.lua", "src/\1x", "src/\255", "" }) do
        _, e = a:read(p); eq(e, "invalid_path", "invalid path " .. ("%q"):format(p))
    end
    ok(a:prepare(), "denials do not fail the action")
    ok(a:deps():find('"observations":[]', 1, true) ~= nil, "denials are not recorded")

    local b = started({ "." })
    _, e = b:read(".hull/x"); eq(e, "reserved_path", "a root grant keeps .hull out")
end

do
    fresh("links")
    dir("src"); dir("other")
    put("other/t.txt", "target"); put("src/real.txt", "inside"); put("exact.txt", "e")
    local made = T.symlink("real.txt", root .. "/src/in_root") and
                 T.symlink("../other/t.txt", root .. "/src/up") and
                 T.symlink("/etc/hosts", root .. "/src/abs") and
                 T.symlink("other/t.txt", root .. "/lnk.txt")
    if made then
        local _, e = action.new({ root = root, grants = { "lnk.txt" } })
        eq(e, "symlink_denied", "an EXACT grant on a link is refused at compile")
        local a = started({ "src/", "exact.txt" })
        eq(a:read("src/in_root"), "inside", "SUBTREE follows a link inside its anchor")
        _, e = a:read("src/up"); eq(e, "not_found", "'..' clamps at the anchor")
        _, e = a:read("src/abs"); eq(e, "not_found", "an absolute target re-roots")
        rm("exact.txt")
        T.symlink("other/t.txt", root .. "/exact.txt")
        _, e = a:read("exact.txt"); eq(e, "symlink_denied", "EXACT never widens through a link")
        ok(a:prepare(), "link answers are observations, not failures")
    else
        io.stderr:write("  SKIP: links (this host cannot make them)\n")
    end
end

-- ── inputs: observations ─────────────────────────────────────────────

do
    fresh("repeat")
    dir("src"); put("src/a.lua", "one")
    local a = started({ "src/" })
    eq(a:read("src/a.lua"), "one", "first read")
    local l1 = a:list("src")
    local s1 = a:stat("src/a.lua", { size = true })
    local _, e = a:read("src/gone.lua"); eq(e, "not_found", "absence observed")

    put("src/a.lua", "changed!"); put("src/gone.lua", "here"); put("src/new.lua", "n")
    eq(a:read("src/a.lua"), "one", "a repeat read returns the first capture")
    local l2 = a:list("src")
    eq(#l2, 1, "a repeat list returns the first listing")
    eq(l2[1].name, "a.lua", "listing name"); eq(l2[1].type, "file", "listing type")
    ok(l1 == l2, "the same listing object")
    eq(a:stat("src/a.lua", { size = true }).size, 3, "a repeat stat returns the first")
    _, e = a:read("src/gone.lua"); eq(e, "not_found", "a repeat absence stays absent")
    eq(a:stat("src/new.lua").type, "file", "other fields: a new observation")
    eq(a:stat("src/nope").type, "absent", "an absent stat is an observation")
    eq(s1.exec, nil, "only asked fields are reported")
end

do
    fresh("canon")
    dir("src"); put("src/a.lua", "a"); put("src/b.lua", "b")
    local m = {}
    for k = 1, 2 do
        local a = started({ "src/" })
        if k == 1 then
            a:read("src/a.lua"); a:read("src/b.lua"); a:stat("src/a.lua", { size = true, exec = true })
            a:list("src"); a:read("src/zz")
        else
            a:read("src/zz"); a:list("src"); a:stat("src/a.lua", { exec = true, size = true })
            a:read("src/b.lua"); a:read("src/a.lua"); a:read("src/b.lua")
        end
        assert(a:prepare())
        m[k] = a:deps()
    end
    eq(m[1], m[2], "the same observations in any order give the same manifest")
    ok(m[1]:find('"op":"list","path":"src"', 1, true) < m[1]:find('"path":"src/a.lua"', 1, true),
       "sorted by path")
    ok(m[1]:find('"result":"not_found"', 1, true) ~= nil, "absence recorded")
    ok(not m[1]:find("mtime", 1, true), "no mtime")
end

-- ── cache validation ─────────────────────────────────────────────────

do
    fresh("check")
    dir("src"); put("src/a.lua", "abc")
    T.set_mtime(root .. "/src/a.lua", 1000000000)
    local a = started({ "src/" })
    a:list("src"); a:read("src/a.lua"); a:read("src/missing.lua"); a:stat("src/a.lua", { exec = true })
    assert(a:prepare()); assert(a:commit())
    local m = a:deps()
    local function fresh_now() return (action.check(root, m)) end

    eq(fresh_now(), true, "unchanged -> fresh")
    T.set_mtime(root .. "/src/a.lua", 1500000000)
    eq(fresh_now(), true, "mtime-only change -> fresh")
    put("src/a.lua", "abd"); T.set_mtime(root .. "/src/a.lua", 1500000000)
    eq(fresh_now(), false, "content change, same mtime -> stale")
    put("src/a.lua", "abc")
    eq(fresh_now(), true, "content back -> fresh")
    put("src/b.lua", "b")
    eq(fresh_now(), false, "directory addition -> stale")
    rm("src/b.lua")
    eq(fresh_now(), true, "removed again -> fresh")
    put("src/missing.lua", "m")
    eq(fresh_now(), false, "a recorded absence that now exists -> stale")
    rm("src/missing.lua")
    if T.chmod(root .. "/src/a.lua", 493) then        -- 0755
        eq(fresh_now(), false, "a recorded exec bit changed -> stale")
        T.chmod(root .. "/src/a.lua", 420)            -- 0644
        eq(fresh_now(), true, "exec bit back -> fresh")
    end

    local _, e = action.check(root, "{}"); eq(e, "bad_manifest", "malformed manifest")
    _, e = action.check(root, '{"format":"hull-build-deps","version":2,"grants":[],"observations":[]}')
    eq(e, "bad_manifest", "another version")
    _, e = action.check(root, '{"format":"hull-build-deps","version":1,"grants":["src/"],' ..
        '"observations":[{"op":"read","path":"../x","result":"not_found"}]}')
    eq(e, "bad_manifest", "an escaping path in a manifest")
end

-- ── limits and resources ─────────────────────────────────────────────

do
    fresh("limits")
    put("big.txt", "0123456789"); put("a", "a"); put("b", "b")
    local g = { "big.txt", "a", "b" }
    local a = started(g, { max_read_bytes = 4 })
    local _, e = a:read("big.txt"); eq(e, "limit", "read over max_read_bytes")
    eq(a:read("a"), "a", "other ops go on")
    _, e = a:prepare(); eq(e, "input_failed", "a limit fails the action")
    eq(a:get_state(), "aborted", "and aborts it")

    a = started(g, { max_observations = 1 })
    a:read("a"); a:read("a")
    _, e = a:read("b"); eq(e, "limit", "past max_observations (a repeat is free)")

    a = started(g, { max_total_bytes = 11 })
    a:read("big.txt"); a:read("a")
    _, e = a:read("b"); eq(e, "limit", "past max_total_bytes")
end

do
    fresh("fds")
    dir("src"); put("src/a.lua", "a")
    local function round()
        local a = started({ "src/" })
        a:read("src/a.lua"); a:read("src/none"); a:list("src"); a:stat("src", { size = true })
        a:read("../escape")
        a:prepare(); a:commit()
        action.check(root, a:deps())
        local b = started({ "src/" })      -- dropped without prepare
        b:read("src/a.lua")
        b:abort()
    end
    round()                                 -- the first compile reserves the root's .hull (one fd)
    collectgarbage()
    local before = T.open_fds()
    for _ = 1, 20 do round() end
    collectgarbage()
    eq(T.open_fds(), before, "no descriptor outlives its action")
end

for _, r in ipairs(roots) do T.rmtree(r) end
return { pass = pass, fail = fail }
