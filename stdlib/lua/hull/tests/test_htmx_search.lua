-- test_htmx_search.lua. Tests for hull.web.htmx.search helpers.

local search = require('hull.web.htmx.search')

local pass, fail = 0, 0
local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then pass = pass + 1
    else fail = fail + 1; print("FAIL: " .. name .. ": " .. tostring(err)) end
end
local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end
local function assert_match(s, sub, msg)
    if not s:find(sub, 1, true) then
        error((msg or "") .. " expected '" .. sub .. "' in '" .. s .. "'")
    end
end
local function assert_no_match(s, sub, msg)
    if s:find(sub, 1, true) then
        error((msg or "") .. " did not expect '" .. sub .. "' in '" .. s .. "'")
    end
end

-- ── input_attrs() ──────────────────────────────────────────────────

test("input_attrs emits type=search + autocomplete=off", function()
    local s = search.input_attrs({ url = "/x" })
    assert_match(s, 'type="search"')
    assert_match(s, 'autocomplete="off"')
end)

test("input_attrs default method is GET", function()
    local s = search.input_attrs({ url = "/search" })
    assert_match(s, 'hx-get="/search"')
    assert_no_match(s, 'hx-post')
end)

test("input_attrs method=post emits hx-post", function()
    local s = search.input_attrs({ url = "/search", method = "post" })
    assert_match(s, 'hx-post="/search"')
    assert_no_match(s, 'hx-get')
end)

test("input_attrs default debounce is 300ms", function()
    local s = search.input_attrs({ url = "/x" })
    assert_match(s, 'hx-trigger="input changed delay:300ms"')
end)

test("input_attrs custom delay_ms takes effect", function()
    local s = search.input_attrs({ url = "/x", delay_ms = 500 })
    assert_match(s, 'delay:500ms')
end)

test("input_attrs default name is q", function()
    assert_match(search.input_attrs({ url = "/x" }), 'name="q"')
end)

test("input_attrs custom name overrides q", function()
    assert_match(search.input_attrs({ url = "/x", name = "query" }), 'name="query"')
end)

test("input_attrs default target is hull-search-results", function()
    assert_match(search.input_attrs({ url = "/x" }), 'hx-target="#hull-search-results"')
end)

test("input_attrs custom target", function()
    assert_match(
        search.input_attrs({ url = "/x", target = "#asset-rows" }),
        'hx-target="#asset-rows"')
end)

test("input_attrs push_url=true emits hx-push-url", function()
    assert_match(search.input_attrs({ url = "/x", push_url = true }), 'hx-push-url="true"')
end)

test("input_attrs push_url=false (default) omits hx-push-url", function()
    assert_no_match(search.input_attrs({ url = "/x" }), 'hx-push-url')
end)

test("input_attrs indicator emits hx-indicator", function()
    assert_match(
        search.input_attrs({ url = "/x", indicator = "#spinner" }),
        'hx-indicator="#spinner"')
end)

test("input_attrs escapes url (no attribute break)", function()
    local s = search.input_attrs({ url = '/x?a="b"' })
    assert_match(s, 'hx-get="/x?a=&quot;b&quot;"')
end)

-- ── results_attrs() ────────────────────────────────────────────────

test("results_attrs default id matches input_attrs default target", function()
    local s = search.results_attrs()
    assert_match(s, 'id="hull-search-results"')
end)

test("results_attrs always sets aria-live=polite", function()
    assert_match(search.results_attrs(), 'aria-live="polite"')
end)

test("results_attrs custom id", function()
    assert_match(search.results_attrs({ id = "asset-rows" }), 'id="asset-rows"')
end)

-- ── pair contract: input_attrs default target == results_attrs default id

test("default input target and default results id pair correctly", function()
    local input   = search.input_attrs({ url = "/x" })
    local results = search.results_attrs()
    assert_match(input,   'hx-target="#hull-search-results"')
    assert_match(results, 'id="hull-search-results"')
end)

-- ── Edge cases ─────────────────────────────────────────────────────

test("input_attrs with no opts is well-defined (url defaults to empty)", function()
    -- No assertion of behavior beyond "doesn't crash and emits
    -- the basic shape." The empty hx-get is intentionally cheap;
    -- caller bugs (forgetting url) surface as a 404 on first
    -- search rather than a hidden init-time error.
    local s = search.input_attrs()
    assert_match(s, 'type="search"')
    assert_match(s, 'hx-get=""')
end)

test("input_attrs method=POST (uppercase) normalizes to post", function()
    -- Case-insensitive method per the L4 audit fix.
    local s = search.input_attrs({ url = "/x", method = "POST" })
    assert_match(s, 'hx-post="/x"')
    assert_no_match(s, 'hx-get')
end)

test("input_attrs invalid method raises a clear error", function()
    local ok, err = pcall(search.input_attrs, { url = "/x", method = "put" })
    assert_eq(ok, false, "should have errored")
    if not tostring(err):find("'get' or 'post'", 1, true) then
        error("expected error to mention valid options, got: " .. tostring(err))
    end
end)

print(string.format("\n%d/%d htmx-search tests passed", pass, pass + fail))
if fail > 0 then os.exit(1) end
