--
-- test_parser.lua — slice-2 tests for hull.source.parser (expression grammar).
--
-- Lexes then parses a single expression; asserts AST structure, exact half-open
-- byte ranges, and Lua 5.4 precedence/associativity. Pure Lua; run by
-- tests/hull/source/test_lua_source.c. Returns { pass, fail }.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local lexer = require("hull.source.lexer")
local parser = require("hull.source.parser")

local pass, fail, failures = 0, 0, {}
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; failures[#failures + 1] = name; print("FAIL: " .. name) end
end
local function eq(a, b, name)
    ok(a == b, name .. " (expected " .. tostring(b) .. ", got " .. tostring(a) .. ")")
end

local function parse_expr(src, opts)
    local lx = lexer.tokenize(src, opts)
    local node, pdiags = parser.parse_expression(lx.tokens, src, opts)
    local all = {}
    for _, d in ipairs(lx.diagnostics) do all[#all + 1] = d end
    for _, d in ipairs(pdiags) do all[#all + 1] = d end
    return node, all
end
local function slice(src, node) return src:sub(node.range.start, node.range.stop - 1) end
local function n_diag(all, code)
    local c = 0
    for _, d in ipairs(all) do if d.code == code then c = c + 1 end end
    return c
end

-- ── literals + vararg ─────────────────────────────────────────────────
do
    for _, c in ipairs({
        { "42", "number" }, { '"hi"', "string" }, { "nil", "nil" },
        { "true", "boolean" }, { "false", "boolean" }, { "0x1.8p3", "number" },
    }) do
        local node, all = parse_expr(c[1])
        eq(node.kind, "literal", "literal kind: " .. c[1])
        eq(node.subtype, c[2], "literal subtype: " .. c[1])
        eq(slice(c[1], node), c[1], "literal range: " .. c[1])
        eq(#all, 0, "literal clean: " .. c[1])
    end
    local v = parse_expr("...")
    eq(v.kind, "vararg", "vararg kind")

    -- LOCKED literal shapes: every literal has `text`; booleans add boolean
    -- `value`; nil carries NO value.
    local num = parse_expr("42");   eq(num.text, "42", "num.text"); ok(num.value == nil, "num: no value field")
    local str = parse_expr('"hi"'); eq(str.text, '"hi"', "str.text")
    local nl = parse_expr("nil");   eq(nl.subtype, "nil", "nil.subtype"); eq(nl.text, "nil", "nil.text"); ok(nl.value == nil, "nil: no value")
    local tr = parse_expr("true");  eq(tr.subtype, "boolean", "true.subtype"); eq(tr.text, "true", "true.text"); eq(tr.value, true, "true.value")
    local fa = parse_expr("false"); eq(fa.text, "false", "false.text"); eq(fa.value, false, "false.value")
end

-- ── function-body balanced skip: nested constructs, one `end` each ────
do
    local bodies = {
        "function() for i = 1, 3 do f(i) end end",             -- for + its do = one end
        "function() if x then a() else b() end end",           -- if/elseif/else = one end
        "function() while c do g() end end",                   -- while + its do = one end
        "function() do local x = 1 end end",                   -- STANDALONE do
        "function() repeat h() until done end",                -- repeat/until (no end)
        "function() local f = function() return 1 end; return f end", -- nested function
        "function() for a in pairs(t) do if a then break end end end", -- nested if in for
    }
    for _, src in ipairs(bodies) do
        local node, all = parse_expr(src)
        eq(node.kind, "function_expr", "body: function_expr for " .. src)
        eq(n_diag(all, "lua.syntax"), 0, "body: no spurious 'unterminated' for " .. src)
        eq(n_diag(all, "lua.unsupported"), 0, "body: parses (no deferred notice) for " .. src)
        eq(slice(src, node), src, "body: full range for " .. src)
    end
end

-- ── parser diagnostics respect the configured max_diagnostics ─────────
do
    local _, all = parse_expr("f(+, +, +, +, +)", { limits = { max_diagnostics = 2 } })
    ok(#all <= 2, "parser: max_diagnostics caps normal diagnostics (" .. #all .. ")")
end

-- ── names / field / index / call / method / call-forms ────────────────
do
    local node, all = parse_expr("foo(1, 2)")
    eq(node.kind, "call", "call kind")
    eq(node.callee.kind, "name", "call callee name")
    eq(node.callee.name, "foo", "call callee text")
    eq(#node.args, 2, "call arg count")
    eq(slice("foo(1, 2)", node), "foo(1, 2)", "call range covers whole")
    eq(node.callee.range.start, 1, "callee.start"); eq(node.callee.range.stop, 4, "callee.stop [half-open)")
    eq(node.args[1].range.start, 5, "arg1.start"); eq(node.args[2].range.start, 8, "arg2.start")
    eq(#all, 0, "call clean")

    eq(parse_expr("a.b").kind, "field", "field kind")
    eq(parse_expr("a.b").name, "b", "field name")
    eq(parse_expr("a[b]").kind, "index", "index kind")
    local m = parse_expr("obj:m(x)")
    eq(m.kind, "method_call", "method_call kind"); eq(m.method, "m", "method name")
    eq(parse_expr('f"x"').kind, "call", "call with string arg")
    eq(parse_expr("f{}").kind, "call", "call with table arg")

    -- chained suffixes: a.b.c(1)[2]:m()
    local ch = parse_expr("a.b.c(1)[2]:m()")
    eq(ch.kind, "method_call", "chain: outermost is method_call")
    eq(slice("a.b.c(1)[2]:m()", ch), "a.b.c(1)[2]:m()", "chain: full range")
end

-- ── table constructors ────────────────────────────────────────────────
do
    eq(#parse_expr("{}").fields, 0, "table: empty")
    local t = parse_expr("{ 1, 2, 3 }")
    eq(t.kind, "table", "table kind"); eq(#t.fields, 3, "table: 3 items")
    eq(t.fields[1].kind, "field_item", "table: positional")
    local t2 = parse_expr('{ a = 1, ["b"] = 2, 3; 4 }')
    eq(#t2.fields, 4, "table: mixed field count")
    eq(t2.fields[1].kind, "field_name", "table: name field")
    eq(t2.fields[1].name, "a", "table: name field key")
    eq(t2.fields[2].kind, "field_expr", "table: [expr] field")
    eq(t2.fields[3].kind, "field_item", "table: item after ,")
    eq(t2.fields[4].kind, "field_item", "table: item after ;")
end

-- ── unary ─────────────────────────────────────────────────────────────
do
    for _, c in ipairs({ { "-x", "-" }, { "not y", "not" }, { "#t", "#" }, { "~n", "~" } }) do
        local u = parse_expr(c[1])
        eq(u.kind, "unary", "unary kind: " .. c[1]); eq(u.op, c[2], "unary op: " .. c[1])
    end
end

-- ── precedence / associativity (assert TREE shape, not just no-crash) ─
do
    local a = parse_expr("1 + 2 * 3")           -- +(1, *(2,3))
    eq(a.kind, "binary", "prec: root"); eq(a.op, "+", "prec: root +")
    eq(a.rhs.kind, "binary", "prec: rhs is *"); eq(a.rhs.op, "*", "prec: * under +")

    local b = parse_expr("2 ^ 2 ^ 3")           -- right assoc: ^(2, ^(2,3))
    eq(b.op, "^", "assoc: ^ root"); eq(b.rhs.op, "^", "assoc: ^ nests right")

    local c = parse_expr("a .. b .. c")         -- right assoc: ..(a, ..(b,c))
    eq(c.op, "..", "assoc: .. root"); eq(c.rhs.op, "..", "assoc: .. nests right")

    local d = parse_expr("1 - 2 - 3")           -- left assoc: -(-(1,2),3)
    eq(d.op, "-", "assoc: - root"); eq(d.lhs.kind, "binary", "assoc: - nests left")

    local e = parse_expr("-2 ^ 2")              -- unary looser than ^: -(^(2,2))
    eq(e.kind, "unary", "prec: unary root"); eq(e.operand.op, "^", "prec: ^ under unary -")

    local f = parse_expr("(a + b) * c")         -- paren groups
    eq(f.op, "*", "paren: * root"); eq(f.lhs.kind, "paren", "paren: lhs parenthesized")
end

-- ── function expression (params now; body deferred to slice 3) ────────
do
    local empty, all = parse_expr("function() end")
    eq(empty.kind, "function_expr", "func: kind"); eq(#empty.params, 0, "func: no params")
    eq(empty.is_vararg, false, "func: not vararg"); eq(#all, 0, "func: empty body clean")

    local va = parse_expr("function(...) end")
    eq(va.is_vararg, true, "func: vararg")

    local body, ball = parse_expr("function(x, y) return x end")
    eq(body.kind, "function_expr", "func: with params kind")
    eq(#body.params, 2, "func: 2 params"); eq(body.params[1].name, "x", "func: param name")
    eq(n_diag(ball, "lua.unsupported"), 0, "func: non-empty body parses (slice 3)")
    eq(n_diag(ball, "lua.syntax"), 0, "func: body clean")
    ok(type(body.body) == "table" and body.body[1].kind == "return", "func: parsed body has return")
    eq(slice("function(x, y) return x end", body), "function(x, y) return x end", "func: full range")
end

-- ── errors: diagnostics, never a raise; no partial garbage ────────────
do
    local _, a1 = parse_expr("1 +")            -- trailing operator
    ok(n_diag(a1, "lua.syntax") >= 1, "error: trailing operator -> diagnostic")
    local _, a2 = parse_expr("(1")             -- unterminated paren
    ok(n_diag(a2, "lua.syntax") >= 1, "error: unterminated paren -> diagnostic")
    local _, a3 = parse_expr("1 2")            -- trailing tokens
    ok(n_diag(a3, "lua.syntax") >= 1, "error: trailing tokens -> diagnostic")
    -- never raises on nasty input
    local nasty = { "", "(", ")", "{", "}", "[", ".", ":", "..", "a.", "a:", "f(", "{,}", "1+*2" }
    local safe = true
    for _, s in ipairs(nasty) do
        local okc = pcall(parse_expr, s)
        if not okc then safe = false; print("  parser RAISED on: " .. tostring(s)) end
    end
    ok(safe, "error: parser never raises on malformed expressions")
end

-- ── max_depth bounds nesting (no stack overflow) ──────────────────────
do
    local deep = string.rep("(", 100) .. "1" .. string.rep(")", 100)
    local _, all = parse_expr(deep, { limits = { max_depth = 20 } })
    ok(n_diag(all, "lua.limit.max_depth") >= 1, "depth: max_depth trips")
end

print(string.format("test_parser: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail, failures = failures }
