-- test_stdlib_audit49.lua - stdlib regressions from audit 49 (Lua stdlib).
--
-- template: a loop variable bound to a function renders nothing, as a dot
-- path does since audit 26 ({{ it }} printed "function: 0x...").
-- search: snippet / highlight markers are drawn per query, so indexed text
-- holding U+E000-U+E002 no longer gets the caller's markup.
--
-- Runs in the caps-bearing context (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c): search needs the db.

local template = require("hull.template")
local search   = require("hull.search")

local pass = 0
local fail = 0

local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a), 2)
    end
end

-- ── template: loop variables ───────────────────────────────────────

test("template_audit49.loop_var_function", function()
    local out = template.render_string(
        "{% for it in list %}[{{ it }}|{{{ it }}}]{% end %}",
        { list = { string.rep, 1, print } })
    assert_eq(out, "[|][1|1][|]")
end)

test("template_audit49.loop_kv_function", function()
    local out = template.render_string(
        "{% for k, v in obj %}<{{ k }}={{{ v }}}>{% end %}",
        { obj = { greet = function() end } })
    assert_eq(out, "<greet=>")
end)

test("template_audit49.loop_var_filter_input", function()
    local out = template.render_string(
        "{% for it in list %}[{{ it | upper }}|{{ it | default: \"d\" }}]{% end %}",
        { list = { print } })
    assert_eq(out, "[|d]")
end)

test("template_audit49.loop_var_condition", function()
    local out = template.render_string(
        "{% for it in list %}{% if it %}T{% else %}F{% end %}{% end %}",
        { list = { print, "x" } })
    assert_eq(out, "FT")
end)

test("template_audit49.loop_var_values_still_render", function()
    local out = template.render_string(
        "{% for it in list %}{{ it.name }}{% for x in it.tags %},{{ x }}{% end %};{% end %}",
        { list = { { name = "a<", tags = { "t1", "t2" } } } })
    assert_eq(out, "a&lt;,t1,t2;")
end)

-- ── search: planted markers ────────────────────────────────────────

search.create_index("a49docs", { "body" })
search.index("a49docs", "1",
    { body = "hello \u{E001}\u{E001} world \u{E000} more \u{E002} tail" })

test("search_audit49.highlight_planted_marker", function()
    local rows = search.query("a49docs", "hello",
        { highlight = { column = 1, before = "<mark>", after = "</mark>" } })
    assert_eq(rows[1].highlight,
        "<mark>hello</mark> \u{E001}\u{E001} world \u{E000} more \u{E002} tail")
end)

test("search_audit49.snippet_planted_marker", function()
    local rows = search.query("a49docs", "tail",
        { snippet = { column = 1, tokens = 64, before = "<b>", after = "</b>",
                      ellipsis = "~" } })
    assert_eq(rows[1].snippet,
        "hello \u{E001}\u{E001} world \u{E000} more \u{E002} <b>tail</b>")
end)

search.drop_index("a49docs")

return { pass = pass, fail = fail }
