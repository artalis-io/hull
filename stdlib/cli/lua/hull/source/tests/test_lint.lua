--
-- test_lint.lua — slice-1 tests for hull.source.lint (the structural rule engine).
--
-- Each rule: a positive case that MUST fire and a negative that must NOT. Plus the
-- engine (selection, registry). Pure Lua; run by tests/hull/source/test_lua_source.c.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local lua = require("hull.source.lua")
local lint = require("hull.source.lint")
local scope = require("hull.source.scope")

local pass, fail, failures = 0, 0, {}
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; failures[#failures + 1] = name; print("FAIL: " .. name) end
end
local function eq(a, b, name)
    ok(a == b, name .. " (expected " .. tostring(b) .. ", got " .. tostring(a) .. ")")
end

-- count of findings for a given rule id in `src` (all default rules unless given).
-- Computes the scope model when a needs_scope rule is active.
local function count(src, ruleid, enabled)
    local u = lua.parse(src, { path = "t.lua" })
    ok(u ~= nil and #u.diagnostics == 0, "lint fixture parses clean: " .. src:gsub("%s+", " "):sub(1, 40))
    local en = enabled or lint.default_enabled()
    local sc = nil
    if lint.needs_scope(en) then sc = (scope.resolve(u)) end
    local n = 0
    for _, f in ipairs(lint.run(u, en, sc)) do
        if f.rule == ruleid then
            n = n + 1
            -- every finding carries a namespaced code, severity, and a range
            ok(f.code == "lua.lint." .. ruleid, "finding code namespaced: " .. tostring(f.code))
            ok(f.range ~= nil and f.range.start ~= nil, "finding has a range: " .. ruleid)
        end
    end
    return n
end

-- ── empty-block ───────────────────────────────────────────────────────
do
    ok(count("if x then end", "empty-block") == 1, "empty if branch fires")
    ok(count("while c do end", "empty-block") == 1, "empty while fires")
    ok(count("for i = 1, 3 do end", "empty-block") == 1, "empty numeric for fires")
    ok(count("for k in pairs(t) do end", "empty-block") == 1, "empty generic for fires")
    ok(count("do end", "empty-block") == 1, "empty do fires")
    -- negatives: a non-empty block, and an empty FUNCTION body (intentional stub)
    ok(count("if x then y() end", "empty-block") == 0, "non-empty if does not fire")
    ok(count("local f = function() end", "empty-block") == 0, "empty function body does NOT fire")
    ok(count("while c do g() end", "empty-block") == 0, "non-empty while does not fire")
end

-- ── duplicate-table-key ───────────────────────────────────────────────
do
    ok(count("local t = { a = 1, a = 2 }", "duplicate-table-key") == 1, "dup name key fires")
    ok(count('local t = { a = 1, ["a"] = 2 }', "duplicate-table-key") == 1, "name vs [\"a\"] unify")
    ok(count("local t = { [1] = x, [1] = y }", "duplicate-table-key") == 1, "dup number key fires")
    -- negatives
    ok(count("local t = { a = 1, b = 2 }", "duplicate-table-key") == 0, "distinct keys clean")
    ok(count("local t = { 1, 2, 3 }", "duplicate-table-key") == 0, "positional items clean")
    ok(count("local t = { [x] = 1, [y] = 2 }", "duplicate-table-key") == 0, "non-literal keys not compared")
end

-- ── todo-comment ──────────────────────────────────────────────────────
do
    ok(count("-- TODO fix this\nreturn 1", "todo-comment") == 1, "TODO fires")
    ok(count("--[[ FIXME later ]]\nreturn 1", "todo-comment") == 1, "FIXME in long comment fires")
    ok(count("-- XXX\nreturn 1", "todo-comment") == 1, "XXX fires")
    ok(count("-- an ordinary note\nreturn 1", "todo-comment") == 0, "ordinary comment clean")
    -- severity is info
    local u = lua.parse("-- TODO\nreturn 1", { path = "t.lua" })
    local f = lint.run(u, lint.default_enabled())[1]
    eq(f.severity, "info", "todo-comment severity is info")
end

-- ── engine: selection + registry ──────────────────────────────────────
do
    ok(lint.exists("empty-block") and not lint.exists("nope"), "exists() checks the registry")
    local only_todo = { ["todo-comment"] = true }
    ok(count("-- TODO\nlocal t={a=1,a=2}\nif x then end", "duplicate-table-key", only_todo) == 0,
        "selection: dup-key disabled when only todo enabled")
    ok(count("-- TODO\nlocal t={a=1,a=2}\nif x then end", "todo-comment", only_todo) == 1,
        "selection: todo still fires when selected")
    local de = lint.default_enabled()
    ok(de["empty-block"] and de["duplicate-table-key"] and de["todo-comment"],
        "default_enabled has all slice-1 rules on")
end

-- ── unused-local (local + localfunc; loop vars / params excluded; _ exempt) ─
do
    ok(count("local x = 1\nreturn 0", "unused-local") == 1, "unused local fires")
    ok(count("local function f() end\nreturn 0", "unused-local") == 1, "unused local function fires")
    ok(count("local x = 1\nreturn x", "unused-local") == 0, "used local clean")
    ok(count("local _ = 1\nreturn 0", "unused-local") == 0, "_ exempt from unused-local")
    ok(count("for i = 1, 3 do print(0) end", "unused-local") == 0, "loop var not flagged by unused-local")
    ok(count("local function f(a) return 0 end\nreturn f", "unused-param") == 1, "unused-local does not double-flag a param")
end

-- ── unused-param (_ / implicit self exempt) ───────────────────────────
do
    ok(count("local function f(a, b) return a end\nreturn f", "unused-param") == 1, "unused param b fires")
    ok(count("local function f(a) return a end\nreturn f", "unused-param") == 0, "used param clean")
    ok(count("local function f(_a) return 1 end\nreturn f", "unused-param") == 0, "_-prefixed param exempt")
    ok(count("local o = {}\nfunction o:m() return 1 end\nreturn o", "unused-param") == 0, "implicit self exempt")
end

-- ── shadowed-local (_ / implicit exempt) ──────────────────────────────
do
    ok(count("local x = 1\ndo local x = 2\nprint(x) end\nreturn x", "shadowed-local") == 1, "shadowing fires")
    ok(count("local x = 1\nreturn x", "shadowed-local") == 0, "no shadow -> clean")
    ok(count("local _ = 1\nlocal _ = 2\nreturn 0", "shadowed-local") == 0, "_ shadowing exempt")
end

-- ── undefined-global: OFF by default; evidence-based allowlist; reads only ─
do
    local en = lint.default_enabled(); en["undefined-global"] = true
    for _, g in ipairs({ "pairs", "string", "app", "hull", "require", "print", "math",
                         "test", "coroutine", "_ENV", "_G" }) do
        ok(count("return " .. g, "undefined-global", en) == 0, "allowed global NOT flagged: " .. g)
    end
    for _, g in ipairs({ "db", "req", "res", "json", "os", "io", "load", "tool", "arg", "debug", "package" }) do
        ok(count("return " .. g, "undefined-global", en) == 1, "undefined global flagged: " .. g)
    end
    ok(count("return db", "undefined-global") == 0, "undefined-global OFF by default")
    ok(count("undefinedthing = 1", "undefined-global", en) == 0, "a global WRITE is not flagged (reads only)")
end

-- ── engine: a scope-backed rule is SKIPPED when scope is nil (resolver failed) ─
do
    local u = lua.parse("local x = 1\nreturn 0", { path = "t.lua" })
    eq(#lint.run(u, { ["unused-local"] = true }, nil), 0, "needs_scope rule skipped when scope is nil")
    ok(lint.needs_scope({ ["unused-local"] = true }), "needs_scope true for a scope rule")
    ok(not lint.needs_scope({ ["todo-comment"] = true }), "needs_scope false for structural-only")
    -- structural rules still run without scope
    ok(#lint.run(lua.parse("if x then end", { path = "t.lua" }), { ["empty-block"] = true }, nil) == 1,
        "structural rule still runs without scope")
end

print(string.format("test_lint: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail, failures = failures }
