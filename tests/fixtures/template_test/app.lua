--
-- Template engine test app (Lua)
-- Each route tests a different template feature and returns JSON pass/fail.
--

local template = require("hull.template")

-- Helper: compare expected vs actual
local function check(name, actual, expected)
    if actual == expected then
        return { test = name, ok = true }
    else
        return { test = name, ok = false, expected = expected, actual = actual }
    end
end

app.get("/health", function(req, res)
    res:json({ status = "ok" })
end)

-- Test 1: plain text passthrough
app.get("/test/text", function(req, res)
    local html = template.render_string("Hello World", {})
    res:json(check("text", html, "Hello World"))
end)

-- Test 2: variable interpolation with HTML escaping
app.get("/test/var-escape", function(req, res)
    local html = template.render_string("{{ name }}", { name = '<script>alert("xss")</script>' })
    res:json(check("var-escape", html, '&lt;script&gt;alert(&quot;xss&quot;)&lt;/script&gt;'))
end)

-- Test 3: raw output (no escaping)
app.get("/test/raw", function(req, res)
    local html = template.render_string("{{{ html }}}", { html = "<b>bold</b>" })
    res:json(check("raw", html, "<b>bold</b>"))
end)

-- Test 4: dot path access
app.get("/test/dot-path", function(req, res)
    local html = template.render_string("{{ user.name }}", { user = { name = "Alice" } })
    res:json(check("dot-path", html, "Alice"))
end)

-- Test 5: nil-safe dot path (missing intermediate)
app.get("/test/nil-path", function(req, res)
    local html = template.render_string("{{ user.name }}", {})
    res:json(check("nil-path", html, ""))
end)

-- Test 6: if/else (true branch)
app.get("/test/if-true", function(req, res)
    local html = template.render_string("{% if show %}YES{% else %}NO{% end %}", { show = true })
    res:json(check("if-true", html, "YES"))
end)

-- Test 7: if/else (false branch)
app.get("/test/if-false", function(req, res)
    local html = template.render_string("{% if show %}YES{% else %}NO{% end %}", { show = false })
    res:json(check("if-false", html, "NO"))
end)

-- Test 8: if/elif/else
app.get("/test/elif", function(req, res)
    local html = template.render_string(
        "{% if a %}A{% elif b %}B{% else %}C{% end %}",
        { a = false, b = true })
    res:json(check("elif", html, "B"))
end)

-- Test 9: if not
app.get("/test/if-not", function(req, res)
    local html = template.render_string("{% if not hide %}VISIBLE{% end %}", { hide = false })
    res:json(check("if-not", html, "VISIBLE"))
end)

-- Test 10: for loop with array
app.get("/test/for", function(req, res)
    local html = template.render_string(
        "{% for item in items %}{{ item }},{% end %}",
        { items = { "a", "b", "c" } })
    res:json(check("for", html, "a,b,c,"))
end)

-- Test 11: for loop with dot path on loop variable
app.get("/test/for-dot", function(req, res)
    local html = template.render_string(
        "{% for u in users %}{{ u.name }},{% end %}",
        { users = { { name = "Alice" }, { name = "Bob" } } })
    res:json(check("for-dot", html, "Alice,Bob,"))
end)

-- Test 12: for loop over nil (should produce empty)
app.get("/test/for-nil", function(req, res)
    local html = template.render_string("{% for x in missing %}{{ x }}{% end %}", {})
    res:json(check("for-nil", html, ""))
end)

-- Test 13: nested for + if
app.get("/test/nested", function(req, res)
    local html = template.render_string(
        "{% for u in users %}{% if u.active %}{{ u.name }},{% end %}{% end %}",
        { users = { { name = "Alice", active = true }, { name = "Bob", active = false }, { name = "Carol", active = true } } })
    res:json(check("nested", html, "Alice,Carol,"))
end)

-- Test 14: filters — upper, lower, trim, length
app.get("/test/filters", function(req, res)
    local results = {}
    results[#results + 1] = check("upper", template.render_string("{{ x | upper }}", { x = "hello" }), "HELLO")
    results[#results + 1] = check("lower", template.render_string("{{ x | lower }}", { x = "HELLO" }), "hello")
    results[#results + 1] = check("trim", template.render_string("{{ x | trim }}", { x = "  hi  " }), "hi")
    results[#results + 1] = check("length", template.render_string("{{ items | length }}", { items = { 1, 2, 3 } }), "3")
    results[#results + 1] = check("default-nil", template.render_string("{{ x | default: \"fallback\" }}", {}), "fallback")
    results[#results + 1] = check("default-set", template.render_string("{{ x | default: \"fallback\" }}", { x = "value" }), "value")
    results[#results + 1] = check("json", template.render_string("{{ x | json }}", { x = { 1, 2 } }), "[1,2]")
    res:json(results)
end)

-- Test 15: comment stripping
app.get("/test/comment", function(req, res)
    local html = template.render_string("A{# this is a comment #}B", {})
    res:json(check("comment", html, "AB"))
end)

-- Test 16: template inheritance (extends + block)
app.get("/test/extends", function(req, res)
    local html = template.render("pages/child.html", { year = "2026" })
    res:json(check("extends", html, "HEADER-Child Title-Child Body-FOOTER 2026"))
end)

-- Test 17: include
app.get("/test/include", function(req, res)
    local html = template.render("pages/with_include.html", { name = "World" })
    res:json(check("include", html, "Before-Hello World!-After"))
end)

-- Test 18: for_kv (key, value pairs)
app.get("/test/for-kv", function(req, res)
    -- Note: pairs() order is not guaranteed, so we just check it works
    local html = template.render_string(
        "{% for k, v in data %}{{ k }}={{ v }};{% end %}",
        { data = { x = 1 } })
    res:json(check("for-kv", html, "x=1;"))
end)

-- Test 19: if/else with for inside if
app.get("/test/if-for-else", function(req, res)
    -- With items
    local html1 = template.render_string(
        "{% if show %}{% for i in items %}{{ i }}{% end %}{% else %}empty{% end %}",
        { show = true, items = { "a", "b" } })
    -- Without items
    local html2 = template.render_string(
        "{% if show %}content{% else %}empty{% end %}",
        { show = false })
    local results = {}
    results[#results + 1] = check("if-for-else-true", html1, "ab")
    results[#results + 1] = check("if-for-else-false", html2, "empty")
    res:json(results)
end)

-- Test 20: XSS safety — all {{ }} output is escaped
app.get("/test/xss", function(req, res)
    local html = template.render_string("{{ input }}", { input = '"><img src=x onerror=alert(1)>' })
    local safe = not html:find("<img")
    res:json(check("xss", safe, true))
end)
