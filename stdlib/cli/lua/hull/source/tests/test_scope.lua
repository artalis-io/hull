--
-- test_scope.lua — slice-2 tests for hull.source.scope (the lexical binding pass).
--
-- Asserts ref_of resolutions (local / upvalue / global) + per-decl reads/writes/shadows
-- over the Lua-5.4 scoping edge cases. Pure Lua; run by tests/hull/source/test_lua_source.c.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local lua = require("hull.source.lua")
local scope = require("hull.source.scope")

local pass, fail, failures = 0, 0, {}
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; failures[#failures + 1] = name; print("FAIL: " .. name) end
end
local function eq(a, b, name)
    ok(a == b, name .. " (expected " .. tostring(b) .. ", got " .. tostring(a) .. ")")
end

local function analyze(src)
    local u = lua.parse(src, { path = "t.lua" })
    ok(u ~= nil and #u.diagnostics == 0, "scope fixture parses clean: " .. src:gsub("%s+", " "):sub(1, 40))
    local sc, err = scope.resolve(u)
    ok(err == nil and sc ~= nil, "scope.resolve ok: " .. src:gsub("%s+", " "):sub(1, 40))
    return u, sc
end

-- resolution of the first RESOLVED name node with this text (in source order)
local function first_ref(u, sc, name)
    local found
    lua.walk(u.ast, function(n)
        if not found and n.kind == "name" and n.name == name and sc.ref_of[n] then
            found = sc.ref_of[n]
        end
    end)
    return found
end
-- all bindings with a given name, in source order
local function binds(sc, name)
    local out = {}
    for _, d in ipairs(sc.bindings) do if d.name == name then out[#out + 1] = d end end
    return out
end

-- ── declaration-after-use (local x = x) + recursion vs closure ────────
do
    local u, sc = analyze("local x = x")
    eq(first_ref(u, sc, "x").kind, "global", "local x = x: RHS binds to global, not the new local")
    eq(binds(sc, "x")[1].reads, 0, "new local x is unread")

    -- local function f captures itself: the body reference is an UPVALUE (a chunk-scope
    -- local captured by the closure), and it resolves to the f declaration (recursion).
    local u2, sc2 = analyze("local function f() return f() end")
    local fref = first_ref(u2, sc2, "f")
    ok(fref.decl == binds(sc2, "f")[1], "local function f: body f resolves to the f decl (recursion)")
    eq(fref.kind, "upvalue", "recursive self-reference is an upvalue capture")
    eq(binds(sc2, "f")[1].reads, 1, "the recursive call counts as a read of f")

    local u3, sc3 = analyze("local f = function() return f end")
    eq(first_ref(u3, sc3, "f").kind, "global", "local f = function() return f end: inner f is global")
end

-- ── shadowing: nested block, same-block, same-declaration ─────────────
do
    local _, sc = analyze("local x = 1\ndo local x = 2\nreturn x end")
    local xs = binds(sc, "x")
    eq(#xs, 2, "two x bindings")
    ok(xs[2].shadows == xs[1], "nested-block x shadows the outer x")

    local _, sc2 = analyze("local x = 1\nlocal x = 2\nreturn x")
    local xs2 = binds(sc2, "x")
    ok(xs2[2].shadows == xs2[1], "same-block redeclaration shadows")

    local _, sc3 = analyze("local x, x = 1, 2\nreturn x")
    local xs3 = binds(sc3, "x")
    eq(#xs3, 2, "two x from one declaration")
    ok(xs3[2].shadows == xs3[1], "repeated name within one declaration shadows")

    -- a callback param shadowing an UPVALUE from an outer function is NOT flagged
    local _, sc4 = analyze("local err = 1\nlocal g = function(err) return err end\nreturn err, g")
    local errs = binds(sc4, "err")
    ok(errs[2].shadows == nil, "param shadowing an outer-function upvalue is NOT flagged")
end

-- ── function declarations: write to local / global / dotted base read ─
do
    local _, sc = analyze("local f\nfunction f() end\nreturn f")
    local fb = binds(sc, "f")[1]
    ok(fb.writes >= 1, "function f() writes to the resolved LOCAL f")
    eq(fb.reads, 1, "f read once (the return)")

    local u2, sc2 = analyze("function g() end")
    eq(first_ref(u2, sc2, "g").kind, "global", "function g() (no local g) is a global write")

    local u3, sc3 = analyze("local a = {}\nfunction a.b.c() end\nreturn a")
    eq(first_ref(u3, sc3, "a").kind, "local", "dotted function decl reads the base name a (local)")
end

-- ── loop-variable scope + control-expr scope ──────────────────────────
do
    local u, sc = analyze("for i = 1, 3 do return i end")
    eq(first_ref(u, sc, "i").kind, "local", "numeric-for var i is local in the body")
    eq(binds(sc, "i")[1].kind, "loopvar", "i is a loopvar")

    local u2, sc2 = analyze("for k, v in pairs(t) do return k end")
    eq(first_ref(u2, sc2, "k").kind, "local", "generic-for var k local in body")
    eq(first_ref(u2, sc2, "pairs").kind, "global", "for control expr resolved in enclosing scope (pairs global)")
end

-- ── method self (implicit, anchored) + upvalue + repeat-until ─────────
do
    local u, sc = analyze("local obj = {}\nfunction obj:m() return self end\nreturn obj")
    eq(first_ref(u, sc, "self").kind, "local", "method self is resolvable in the body")
    local selfd = binds(sc, "self")[1]
    ok(selfd and selfd.implicit == true, "self is an implicit binding")
    ok(selfd and selfd.range ~= nil and selfd.range.start ~= nil, "implicit self has a real anchor range")

    local u2, sc2 = analyze("local x = 1\nlocal function f() return x end\nreturn f")
    eq(first_ref(u2, sc2, "x").kind, "upvalue", "inner function capturing a chunk local is an upvalue")

    local u3, sc3 = analyze("repeat local y = 1 until y")
    eq(first_ref(u3, sc3, "y").kind, "local", "repeat: until condition sees the body's local y")
    eq(binds(sc3, "y")[1].reads, 1, "y read by the until condition")
end

-- ── usage: reads vs writes (unused = zero reads) ──────────────────────
do
    local _, sc = analyze("local a = 1\nlocal b = 1\nreturn b")
    eq(binds(sc, "a")[1].reads, 0, "a unused (0 reads)")
    eq(binds(sc, "b")[1].reads, 1, "b read once")

    local _, sc2 = analyze("local x = 1\nx = 2")
    local xb = binds(sc2, "x")[1]
    eq(xb.reads, 0, "written-but-never-read x has 0 reads (still unused)")
    ok(xb.writes >= 1, "x has a write (the assignment)")
end

-- ── robustness: never-raises on a recovered AST; internal failure -> err ─
do
    local u = lua.parse("local x = (", { path = "t.lua" })     -- broken: recovered AST
    local sc, err = scope.resolve(u)
    ok(sc ~= nil and err == nil, "resolve degrades locally on a recovered AST (no raise)")

    local bad, berr = scope.resolve({ ast = { body = 42 } })   -- forces ipairs(42) to raise
    ok(bad == nil and type(berr) == "table" and berr.code == "lua.internal",
        "internal resolver failure -> (nil, lua.internal), never a silent partial")
end

print(string.format("test_scope: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail, failures = failures }
