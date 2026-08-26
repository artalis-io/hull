--
-- test_statements.lua - slice-3 tests for hull.source.lua (statements + full AST).
--
-- Drives the PUBLIC lua.parse() (which now produces unit.ast = a chunk) plus
-- lua.walk / lua.is. Pure Lua; run by tests/hull/source/test_lua_source.c.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local lua = require("hull.source.lua")

local pass, fail, failures = 0, 0, {}
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; failures[#failures + 1] = name; print("FAIL: " .. name) end
end
local function eq(a, b, name)
    ok(a == b, name .. " (expected " .. tostring(b) .. ", got " .. tostring(a) .. ")")
end

local function parse(src)
    local u, e = lua.parse(src, { path = "t.lua" })
    ok(u ~= nil and e == nil, "parse returns (unit,nil): " .. (src:gsub("%s+", " "):sub(1, 36)))
    return u
end
local function body(src) return parse(src).ast.body end
local function first(src) return body(src)[1] end

-- ── local declarations (+ attributes) ────────────────────────────────
do
    local s = first("local x = 1")
    eq(s.kind, "local_declaration", "local: kind")
    eq(s.names[1].name, "x", "local: name"); eq(#s.values, 1, "local: 1 value")
    eq(s.values[1].kind, "literal", "local: value literal")
    eq(#first("local a, b = 1, 2").names, 2, "local: two names")
    eq(first("local x <const> = 1").names[1].attrib, "const", "local: <const>")
    eq(first("local h <close> = f()").names[1].attrib, "close", "local: <close>")
    eq(#first("local y").values, 0, "local: no initializer")
end

-- ── assignment / call statement ───────────────────────────────────────
do
    local a = first("x = 1")
    eq(a.kind, "assignment", "assign: kind"); eq(a.targets[1].kind, "name", "assign: name target")
    local a2 = first("a.b, c[d] = 1, 2")
    eq(#a2.targets, 2, "assign: 2 targets")
    eq(a2.targets[1].kind, "field", "assign: field target"); eq(a2.targets[2].kind, "index", "assign: index target")
    eq(first("f(x)").kind, "call_statement", "callstat: kind")
    eq(first("f(x)").call.kind, "call", "callstat: call node")
    eq(first("obj:m()").call.kind, "method_call", "callstat: method call")
end

-- ── function declarations (global / dotted / method / local) ──────────
do
    local f = first("function foo() end")
    eq(f.kind, "function_declaration", "func: kind"); eq(f.is_local, false, "func: not local")
    eq(f.name.name, "foo", "func: name")
    eq(first("function a.b.c() end").name.kind, "field", "func: dotted name -> field chain")
    eq(first("function a:m() end").is_method, true, "func: method")
    eq(first("local function g() end").is_local, true, "func: local function")
    eq(first("local function v(...) end").is_vararg, true, "func: vararg")

    local u = parse("function sq(x) return x * x end")           -- real body now parses
    eq(#u.diagnostics, 0, "func: body parses clean (no lua.unsupported)")
    local sq = u.ast.body[1]
    eq(#sq.params, 1, "func: 1 param"); eq(sq.params[1].name, "x", "func: param name")
    eq(sq.body[1].kind, "return", "func: body has return")
end

-- ── control flow ──────────────────────────────────────────────────────
do
    local i = first("if a then x() elseif b then y() else z() end")
    eq(i.kind, "if", "if: kind"); eq(#i.clauses, 3, "if: 3 clauses")
    ok(i.clauses[1].cond ~= nil, "if: clause1 has cond"); ok(i.clauses[3].cond == nil, "if: else has no cond")

    local w = first("while c do g() end")
    eq(w.kind, "while", "while: kind"); ok(w.cond ~= nil, "while: cond"); eq(#w.body, 1, "while: body")
    local r = first("repeat h() until done")
    eq(r.kind, "repeat", "repeat: kind"); ok(r.cond ~= nil, "repeat: cond")

    local nf = first("for i = 1, 10, 2 do f(i) end")
    eq(nf.kind, "numeric_for", "numfor: kind")
    eq(nf.var.name, "i", "numfor: var.name (range-bearing record)")
    ok(nf.var.range ~= nil and nf.var.range.start ~= nil, "numfor: var.range present")
    ok(nf.from and nf.to and nf.step, "numfor: from/to/step")
    local gf = first("for k, v in pairs(t) do end")
    eq(gf.kind, "generic_for", "genfor: kind"); eq(#gf.names, 2, "genfor: 2 names")
    eq(gf.names[1].name, "k", "genfor: name k"); eq(#gf.exprs, 1, "genfor: exprs")
    eq(first("do local x = 1 end").kind, "do", "do: kind")
end

-- ── break / goto / label / return ─────────────────────────────────────
do
    eq(body("while true do break end")[1].body[1].kind, "break", "break: kind")
    local g = first("goto done"); eq(g.kind, "goto", "goto: kind"); eq(g.label, "done", "goto: label")
    local l = first("::done::"); eq(l.kind, "label", "label: kind"); eq(l.name, "done", "label: name")
    local ret = body("return 1, 2")[1]; eq(ret.kind, "return", "return: kind"); eq(#ret.values, 2, "return: 2 values")
    eq(#body("return")[1].values, 0, "return: bare")
end

-- ── walk / is (§18/§19) ───────────────────────────────────────────────
do
    local u = parse("local x = f(1) + g(2)")
    local seq = {}
    lua.walk(u.ast, function(n) seq[#seq + 1] = n.kind end)
    eq(seq[1], "chunk", "walk: root (chunk) visited first (pre-order)")
    local ncall = 0
    lua.walk(u.ast, function(n) if lua.is(n, "call") then ncall = ncall + 1 end end)
    eq(ncall, 2, "walk: visits both calls")
    -- deterministic: two walks -> identical kind sequence
    local a, b = {}, {}
    lua.walk(u.ast, function(n) a[#a + 1] = n.kind end)
    lua.walk(u.ast, function(n) b[#b + 1] = n.kind end)
    eq(table.concat(a, ","), table.concat(b, ","), "walk: deterministic")
    ok(not lua.is(nil, "call") and not lua.is(42, "call"), "is: safe on non-nodes")
end

-- ── §41 acceptance example (structure via the Hull-owned API only) ─────
do
    local src = "---@compute\n" ..
        "---@param x f64\n---@param y f64\n---@return f64\n" ..
        "local function score(x, y)\n    return x * x + y * y\nend\n\n" ..
        "---@query\nlocal active = query {\n    from = \"trips\"\n}\n"
    local u = parse(src)
    eq(#u.diagnostics, 0, "acceptance: parses clean")

    local score
    lua.walk(u.ast, function(n)
        if lua.is(n, "function_declaration") and n.name and n.name.name == "score" then score = n end
    end)
    ok(score ~= nil, "acceptance: found `local function score`")
    eq(score.is_local, true, "acceptance: score is local")
    eq(#score.params, 2, "acceptance: score has 2 params")
    eq(score.body[1].kind, "return", "acceptance: score returns")
    ok(u:text(score):find("^local function score") ~= nil, "acceptance: score range starts at declaration")

    local active = u.ast.body[2]
    eq(active.kind, "local_declaration", "acceptance: active local_declaration")
    eq(active.names[1].name, "active", "acceptance: active name")
    eq(active.values[1].kind, "call", "acceptance: query call")
    eq(active.values[1].args[1].kind, "table", "acceptance: query table arg")
end

-- ── best-effort recovery: a bad statement does not abort the chunk ─────
do
    local u = parse("local x = ; local y = 2")   -- ';' where a value is expected
    ok(#u.diagnostics >= 1, "recovery: bad statement -> diagnostic")
    local found_y = false
    lua.walk(u.ast, function(n)
        if lua.is(n, "local_declaration") then
            for _, nm in ipairs(n.names) do if nm.name == "y" then found_y = true end end
        end
    end)
    ok(found_y, "recovery: parsing continued to `local y`")
end

-- ── boundary: parse never raises / returns (nil,nil) on nasty sources ─
do
    local nasty = {
        "", "end", "do", "if", "function", "local", "for", "return return",
        "local x =", "function f(", "::", "goto", "a.b.", "1 = 2",
        string.rep("do ", 300), string.rep("(", 300),
    }
    local safe = true
    for _, s in ipairs(nasty) do
        local okc, uu, e = pcall(lua.parse, s)
        if not okc then safe = false; print("  RAISED: " .. s:sub(1, 20)) end
        if okc and uu == nil and e == nil then safe = false; print("  (nil,nil): " .. s:sub(1, 20)) end
    end
    ok(safe, "boundary: parse never raises / (nil,nil)")
    -- deep statement nesting is bounded (no stack overflow)
    local u = parse(string.rep("do ", 100) .. "return" .. string.rep(" end", 100))
    ok(u ~= nil, "boundary: deep nesting parses within default max_depth")
end

-- ── assignment target validation ──────────────────────────────────────
do
    for _, s in ipairs({ "x = 1", "a.b = 1", "a[b] = 1", "f().x = 1", "a().b[c] = 1" }) do
        eq(#parse(s).diagnostics, 0, "target valid (name/field/index): " .. s)
    end
    for _, s in ipairs({ "f() = 1", "a, g() = 1, 2", "1 = 2", "(x) = 1" }) do
        local u = lua.parse(s, { path = "t.lua" })
        local hit = false
        for _, d in ipairs(u.diagnostics) do if d.code == "lua.syntax" then hit = true end end
        ok(hit, "target invalid -> lua.syntax: " .. s)
        local has_assign = false
        lua.walk(u.ast, function(n) if lua.is(n, "assignment") then has_assign = true end end)
        ok(has_assign, "target invalid: assignment node retained for recovery: " .. s)
    end
end

-- ── local attribute must be const/close (text preserved either way) ────
do
    eq(first("local x <const> = 1").names[1].attrib, "const", "attr: const")
    eq(first("local x <close> = f()").names[1].attrib, "close", "attr: close")
    eq(#parse("local x <const> = 1").diagnostics, 0, "attr: valid -> no diagnostic")
    local u = lua.parse("local x <other> = 1", { path = "t.lua" })
    eq(u.ast.body[1].names[1].attrib, "other", "attr: text preserved for invalid")
    local hit = false
    for _, d in ipairs(u.diagnostics) do if d.code == "lua.syntax" then hit = true end end
    ok(hit, "attr: invalid attribute -> lua.syntax")
end

-- ── max_diagnostics is an AUTHORITATIVE total bound (ordinary) ─────────
do
    local u = lua.parse("@ @ @ @ @\nf() = 1\ng() = 2\nh() = 3", { path = "t.lua", limits = { max_diagnostics = 3 } })
    local ordinary = 0
    for _, d in ipairs(u.diagnostics) do if not d.code:match("^lua%.limit%.") then ordinary = ordinary + 1 end end
    ok(ordinary <= 3, "budget: total ordinary diagnostics <= max_diagnostics (" .. ordinary .. ")")
    -- terminal limit diagnostics always survive, even at max_diagnostics = 0
    local u2 = lua.parse("a b c d e", { path = "t.lua", limits = { max_tokens = 2, max_diagnostics = 0 } })
    local has_terminal = false
    for _, d in ipairs(u2.diagnostics) do if d.code:match("^lua%.limit%.") then has_terminal = true end end
    ok(has_terminal, "budget: terminal limit diagnostic kept at max_diagnostics=0")
end

print(string.format("test_statements: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail, failures = failures }
