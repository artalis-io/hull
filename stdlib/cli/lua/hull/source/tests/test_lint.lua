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

local pass, fail, failures = 0, 0, {}
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; failures[#failures + 1] = name; print("FAIL: " .. name) end
end
local function eq(a, b, name)
    ok(a == b, name .. " (expected " .. tostring(b) .. ", got " .. tostring(a) .. ")")
end

-- count of findings for a given rule id in `src` (all rules enabled unless given)
local function count(src, ruleid, enabled)
    local u = lua.parse(src, { path = "t.lua" })
    ok(u ~= nil and #u.diagnostics == 0, "lint fixture parses clean: " .. src:gsub("%s+", " "):sub(1, 40))
    local n = 0
    for _, f in ipairs(lint.run(u, enabled or lint.default_enabled())) do
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

print(string.format("test_lint: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail, failures = failures }
