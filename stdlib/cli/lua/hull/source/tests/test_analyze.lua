--
-- test_analyze.lua — the analyze_source three-state contract (state x diagnostics).
--
-- Focus: the resolver-failure path. When hull.source.scope.resolve returns (nil, err),
-- analyze_source must (1) DOWNGRADE the file to state "internal" (so JSON files[].state
-- and summary.internal stay consistent with the exit code), (2) surface the internal
-- diagnostic, (3) SKIP scope-backed rules, yet (4) still run structural rules. We inject
-- the failure by monkeypatching scope.resolve (the same cached module analyze uses).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local analyze = require("hull.source.analyze")   -- no `tool` global here -> module, not CLI
local scope = require("hull.source.scope")
local lint = require("hull.source.lint")
local diag = require("hull.source.diagnostic")

local pass, fail, failures = 0, 0, {}
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; failures[#failures + 1] = name; print("FAIL: " .. name) end
end
local function eq(a, b, name)
    ok(a == b, name .. " (expected " .. tostring(b) .. ", got " .. tostring(a) .. ")")
end

local function has(diags, code)
    for _, d in ipairs(diags) do if d.code == code then return true end end
    return false
end

-- analyze_source is the pure core, exported for exactly this test.
ok(type(analyze.analyze_source) == "function", "analyze_source is exported for testing")

-- ── success path: clean file, scope rule runs, state complete ─────────
do
    local state, diags = analyze.analyze_source("t.lua", "local x = 1\nreturn 0", lint.default_enabled())
    eq(state, "complete", "clean file -> state complete")
    ok(has(diags, "lua.lint.unused-local"), "scope rule runs on a clean parse (unused x)")
end

-- ── injected resolver failure: state internal, diag surfaced, structural still runs ──
do
    local orig = scope.resolve
    scope.resolve = function(u)
        return nil, diag.error("lua.internal", "injected resolver failure",
            u and u.path or "t.lua", { start = 1, stop = 2 })
    end
    -- unused-local (scope) is enabled AND `if y then end` trips empty-block (structural).
    local state, diags = analyze.analyze_source("t.lua",
        "local x = 1\nif y then end\nreturn 0", lint.default_enabled())
    scope.resolve = orig                                       -- restore before asserting

    eq(state, "internal", "resolver failure -> file state internal (not complete)")
    ok(has(diags, "lua.internal"), "resolver failure surfaces a lua.internal diagnostic")
    ok(has(diags, "lua.lint.empty-block"), "structural rules STILL run after a resolver failure")
    ok(not has(diags, "lua.lint.unused-local"), "scope-backed rules SKIPPED after a resolver failure")
end

-- ── the internal diagnostic carries the resolver's own code/message ───
do
    local orig = scope.resolve
    scope.resolve = function(u)
        return nil, diag.error("lua.internal", "boom", u and u.path or "t.lua", { start = 3, stop = 4 })
    end
    local _, diags = analyze.analyze_source("t.lua", "local x = 1\nreturn 0", { ["unused-local"] = true })
    scope.resolve = orig
    local found
    for _, d in ipairs(diags) do if d.code == "lua.internal" then found = d end end
    ok(found ~= nil and found.message == "boom", "internal diagnostic keeps the resolver message")
    ok(found ~= nil and found.line ~= nil and found.col ~= nil, "internal diagnostic is positioned (line/col)")
end

print(string.format("test_analyze: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail, failures = failures }
