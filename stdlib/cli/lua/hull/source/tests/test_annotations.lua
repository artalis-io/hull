--
-- test_annotations.lua — slice-4 tests for the `---@` annotation layer.
--
-- Covers hull.source.annotations.parse_comment (the generic scanner) and the
-- declaration-attachment run via the PUBLIC lua.parse() -> node.annotation_list /
-- node.annotations + lua.annotation() + unit:annotations_for(). Pure Lua; run by
-- tests/hull/source/test_lua_source.c.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local lua = require("hull.source.lua")
local annotations = require("hull.source.annotations")

local pass, fail, failures = 0, 0, {}
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; failures[#failures + 1] = name; print("FAIL: " .. name) end
end
local function eq(a, b, name)
    ok(a == b, name .. " (expected " .. tostring(b) .. ", got " .. tostring(a) .. ")")
end

-- A synthetic line-comment token for parse_comment unit tests.
local function line_comment(text) return { kind = "line", text = text, range = { start = 1, stop = #text + 1 } } end
local function pc(text) return annotations.parse_comment(line_comment(text)) end

-- ── scanner: the generic `---@name(args) text` shapes ─────────────────
do
    local a = pc("---@compute")
    ok(a ~= nil, "scan: name-only recognized")
    eq(a.name, "compute", "scan: name-only name"); ok(a.args == nil, "scan: name-only no args")
    ok(a.text == nil, "scan: name-only no text"); eq(a.raw, "---@compute", "scan: raw preserved")

    local p = pc("---@param x f64")
    eq(p.name, "param", "scan: param name"); ok(p.args == nil, "scan: param no parens")
    eq(p.text, "x f64", "scan: param trailing text")

    local d = pc("---@derive(json)")
    eq(d.name, "derive", "scan: derive name"); eq(d.args, "json", "scan: paren args")
    ok(d.text == nil, "scan: derive no trailing text")

    local r = pc('---@route(GET, "/x")  register')
    eq(r.name, "route", "scan: route name")
    eq(r.args, 'GET, "/x"', "scan: multi-arg paren content verbatim")
    eq(r.text, "register", "scan: text after paren, trimmed")

    -- whitelist-free: an arbitrary vendor tag is captured like any other
    local v = pc("---@acme_widget_v2 hello")
    eq(v.name, "acme_widget_v2", "scan: arbitrary tag name captured")
    eq(v.text, "hello", "scan: arbitrary tag text")

    -- extra hyphens + spacing tolerated
    eq(pc("----@ note here").name, "note", "scan: 4 hyphens + space before @")
    eq(pc("---@spaced   trailing").text, "trailing", "scan: inner whitespace collapsed at edges")
end

-- ── scanner: non-annotations return nil ───────────────────────────────
do
    ok(pc("-- ordinary comment") == nil, "scan: 2-hyphen comment is not an annotation")
    ok(pc("--@twohyphen") == nil, "scan: --@ (two hyphens) is not an annotation")
    ok(pc("--- doc without tag") == nil, "scan: --- doc with no @ is not an annotation")
    ok(pc("---@") == nil, "scan: bare ---@ (no name) is nil")
    ok(pc("---@123 bad") == nil, "scan: @ + digit-led name is nil")
    ok(annotations.parse_comment({ kind = "long", text = "--[[ ---@x ]]", range = { start = 1, stop = 2 } }) == nil,
        "scan: a long comment is never an annotation")
end

-- ── attachment: contiguous leading run over a declaration ─────────────
do
    local u = lua.parse("---@compute\n---@pure\nlocal function f() end\n", { path = "t.lua" })
    eq(#u.diagnostics, 0, "attach: annotated source parses clean")
    local f = u.ast.body[1]
    ok(f.annotation_list ~= nil, "attach: node got annotation_list")
    eq(#f.annotation_list, 2, "attach: both leading annotations attached")
    eq(f.annotation_list[1].name, "compute", "attach: order top->down (compute first)")
    eq(f.annotation_list[2].name, "pure", "attach: order top->down (pure second)")
    eq(lua.annotation(f, "compute").name, "compute", "attach: lua.annotation lookup")
    ok(lua.annotation(f, "nope") == nil, "attach: lua.annotation miss -> nil")
    eq(#u:annotations_for(f), 2, "attach: unit:annotations_for returns the list")
end

-- ── attachment: repeat tags kept in list; annotations[name] = first ────
do
    local u = lua.parse("---@param x f64\n---@param y f64\n---@return f64\nlocal function g(x, y) return x end\n",
        { path = "t.lua" })
    local g = u.ast.body[1]
    eq(#g.annotation_list, 3, "repeat: all three annotations in the list")
    eq(g.annotations.param.text, "x f64", "repeat: annotations[name] is the FIRST @param")
    eq(g.annotations["return"].name, "return", "repeat: @return also reachable by name")
    -- annotation_list keeps BOTH @param copies
    local nparam = 0
    for _, a in ipairs(g.annotation_list) do if a.name == "param" then nparam = nparam + 1 end end
    eq(nparam, 2, "repeat: annotation_list retains both @param copies")
end

-- ── attachment: a blank line breaks the run ───────────────────────────
do
    local u = lua.parse("---@compute\n\nlocal x = 1\n", { path = "t.lua" })
    local x = u.ast.body[1]
    ok(x.annotation_list == nil, "blank: blank line between comment and decl -> no attach")
end

-- ── attachment: a trailing comment on a code line does not attach down ─
do
    local u = lua.parse("local a = 1  ---@nope\nlocal b = 2\n", { path = "t.lua" })
    local b = u.ast.body[2]
    ok(b.annotation_list == nil, "trailing: comment on the prior code line does not lead the next decl")
end

-- ── attachment: nested statement inside a function body ───────────────
do
    local u = lua.parse("local function outer()\n  ---@inner\n  local y = 2\nend\n", { path = "t.lua" })
    local inner
    lua.walk(u.ast, function(n)
        if lua.is(n, "local_declaration") then
            for _, nm in ipairs(n.names) do if nm.name == "y" then inner = n end end
        end
    end)
    ok(inner ~= nil, "nested: found the inner local")
    ok(inner.annotation_list ~= nil and inner.annotation_list[1].name == "inner",
        "nested: leading annotation attaches to the nested declaration")
    -- and NOT to the enclosing function_declaration
    ok(u.ast.body[1].annotation_list == nil, "nested: enclosing function has no annotation")
end

-- ── attachment: a plain block comment above a decl is harmless ────────
do
    local u = lua.parse("--[[ a block comment ]]\nlocal z = 1\n", { path = "t.lua" })
    eq(#u.diagnostics, 0, "block: parses clean")
    ok(u.ast.body[1].annotation_list == nil, "block: non-annotation leading comment yields no annotations")
end

-- ── ranges: annotation.range slices back to the exact comment text ────
do
    local src = "---@derive(json)\nlocal rec = {}\n"
    local u = lua.parse(src, { path = "t.lua" })
    local a = u.ast.body[1].annotation_list[1]
    eq(u:text(a), "---@derive(json)", "range: annotation range recovers the comment verbatim")
    eq(a.args, "json", "range: derive args")
end

-- ── unit.comments kind is retagged to "annotation" (leading or trailing) ─
do
    local u = lua.parse("---@compute\nlocal a = 1  ---@trailing\n-- plain\n--[[ block ]]\n", { path = "t.lua" })
    local kinds = {}
    for _, c in ipairs(u.comments) do kinds[#kinds + 1] = c.kind end
    -- ---@compute (annotation), ---@trailing (annotation), -- plain (line), block (long)
    eq(kinds[1], "annotation", "kind: leading ---@ tagged annotation")
    eq(kinds[2], "annotation", "kind: trailing ---@ also tagged annotation")
    eq(kinds[3], "line", "kind: plain comment stays line")
    eq(kinds[4], "long", "kind: block comment stays long")
    ok(u.comments[1].annotation ~= nil and u.comments[1].annotation.name == "compute",
        "kind: parsed record hung off the annotation comment")
end

-- ── §41 acceptance example: full attachment through the public API ────
do
    local src = "---@compute\n" ..
        "---@param x f64\n---@param y f64\n---@return f64\n" ..
        "local function score(x, y)\n    return x * x + y * y\nend\n\n" ..
        "---@query\nlocal active = query {\n    from = \"trips\"\n}\n"
    local u = lua.parse(src, { path = "t.lua" })
    eq(#u.diagnostics, 0, "acceptance: parses clean")

    local score = u.ast.body[1]
    eq(score.name.name, "score", "acceptance: first decl is score")
    eq(#score.annotation_list, 4, "acceptance: score has 4 leading annotations")
    eq(lua.annotation(score, "compute").name, "compute", "acceptance: @compute on score")
    eq(#(function() local t = {} for _, a in ipairs(score.annotation_list) do if a.name == "param" then t[#t+1]=a end end return t end)(),
        2, "acceptance: two @param annotations on score")

    local active = u.ast.body[2]
    eq(active.names[1].name, "active", "acceptance: second decl is active")
    eq(#active.annotation_list, 1, "acceptance: active has 1 annotation")
    eq(active.annotation_list[1].name, "query", "acceptance: @query on active")
end

-- ── boundary: attachment never raises on nasty / annotation-only input ─
do
    local nasty = {
        "---@compute", "---@compute\n", "---@x\n---@y", "\n\n---@a\nlocal q=1",
        "---@only annotations and no code following at all",
        string.rep("---@a\n", 50) .. "local w = 1",
    }
    local safe = true
    for _, s in ipairs(nasty) do
        local okc, u = pcall(lua.parse, s, { path = "t.lua" })
        if not okc or u == nil then safe = false; print("  RAISED/nil: " .. s:sub(1, 24)) end
    end
    ok(safe, "boundary: annotation attachment never raises")
end

print(string.format("test_annotations: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail, failures = failures }
