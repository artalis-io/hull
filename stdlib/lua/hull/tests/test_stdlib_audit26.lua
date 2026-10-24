-- test_stdlib_audit26.lua - stdlib regressions from audit 26 (Lua stdlib).
--
-- template: safe_url percent-encodes the characters that could end an
-- attribute or open a tag, so raw output ({{{ u | safe_url }}}) is inert;
-- a dot path reads only table fields (rawget) and never renders a function,
-- so `{{ title.rep }}` no longer prints string.rep's native address.
-- search: snippet / highlight HTML-escape the indexed text; only the
-- caller's before / after / ellipsis markers are inserted as given.
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

-- ── template: safe_url ─────────────────────────────────────────────

test("safe_url: raw output of an https URL cannot break out", function()
    local out = template.render_string("{{{ u | safe_url }}}",
        { u = 'https://a"><script>x</script>' })
    assert_eq(out, "https://a%22%3E%3Cscript%3Ex%3C/script%3E")
end)

test("safe_url: quotes, backtick, whitespace and controls encoded", function()
    local out = template.render_string("{{{ u | safe_url }}}",
        { u = "/p?a='b` c\t\n\1" })
    assert_eq(out, "/p?a=%27b%60%20c%09%0A%01")
end)

test("safe_url: escaped output of an encoded URL is unchanged", function()
    local out = template.render_string("{{ u | safe_url }}",
        { u = 'mailto:x@y.test?subject="hi"' })
    assert_eq(out, "mailto:x@y.test?subject=%22hi%22")
end)

test("safe_url: an ordinary URL passes through", function()
    assert_eq(template.render_string("{{{ u | safe_url }}}",
        { u = "https://x.test/a?b=1&c=2#f" }), "https://x.test/a?b=1&c=2#f")
end)

test("safe_url: javascript: still refused", function()
    assert_eq(template.render_string("{{{ u | safe_url }}}",
        { u = "javascript:alert(1)" }), "#")
end)

-- ── template: dot paths ────────────────────────────────────────────

test("dot path: a string method renders nothing", function()
    local out = template.render_string("[{{ title.rep }}][{{{ title.len }}}]",
        { title = "hello" })
    assert_eq(out, "[][]")
end)

test("dot path: a function field renders nothing", function()
    local out = template.render_string("[{{ f }}][{{ o.g }}]",
        { f = print, o = { g = function() end } })
    assert_eq(out, "[][]")
end)

test("dot path: __index is not followed", function()
    local o = setmetatable({}, { __index = { secret = "s" } })
    assert_eq(template.render_string("[{{ o.secret }}]", { o = o }), "[]")
end)

test("dot path: nested fields and loop locals still work", function()
    local out = template.render_string(
        "{{ user.name }}:{% for it in items %}{{ it.v }}{% end %}"
        .. "{% if user.admin %}!{% end %}",
        { user = { name = "ann", admin = true }, items = { { v = 1 }, { v = 2 } } })
    assert_eq(out, "ann:12!")
end)

test("dot path: a missing intermediate is nil, not an error", function()
    assert_eq(template.render_string("[{{ a.b.c }}][{{ s.x.y }}]",
        { s = "str" }), "[][]")
end)

-- ── search: snippet / highlight escaping ───────────────────────────

search.create_index("a26docs", { "title", "body" })
search.index("a26docs", "1", {
    title = "t",
    body = 'needle <img src=x onerror=alert(1)> & "q" \'s\'',
})

test("search snippet: indexed markup is escaped, markers inserted", function()
    local rows = search.query("a26docs", "needle",
        { snippet = { column = 2, tokens = 32 } })
    assert_eq(#rows, 1)
    local s = rows[1].snippet
    assert_eq(s:find("<img", 1, true), nil, "raw markup leaked: " .. s)
    assert_eq(s:find("<b>needle</b>", 1, true) ~= nil, true, "markers: " .. s)
    assert_eq(s:find("&lt;img src=x onerror=alert(1)&gt; &amp; &quot;q&quot; &#39;s&#39;", 1, true) ~= nil,
              true, "escaped text: " .. s)
end)

test("search snippet: caller markers are used as given", function()
    local rows = search.query("a26docs", "needle",
        { snippet = { column = 2, tokens = 32, before = "<mark class=\"h\">", after = "</mark>" } })
    local s = rows[1].snippet
    assert_eq(s:find('<mark class="h">needle</mark>', 1, true) ~= nil, true, s)
    assert_eq(s:find("\238\128", 1, true), nil, "sentinel left: " .. s)
end)

test("search highlight: indexed markup is escaped", function()
    local rows = search.query("a26docs", "needle", { highlight = { column = 2 } })
    local h = rows[1].highlight
    assert_eq(h:sub(1, 13), "<b>needle</b>", h)
    assert_eq(h:find("<img", 1, true), nil, "raw markup leaked: " .. h)
end)

print(string.format("stdlib audit 26: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }
