-- test_htmx_pagination.lua. Tests for hull.web.htmx.pagination.nav.

local pag = require('hull.web.htmx.pagination')

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

test("nav returns empty string when only one page", function()
    assert_eq(pag.nav(5, { per_page = 20, target = "#x" }), "")
    assert_eq(pag.nav(0, { per_page = 20, target = "#x" }), "")
end)

test("nav emits a <nav> with aria-label when pages > 1", function()
    local s = pag.nav(100, { per_page = 10, page = 1, target = "#x" })
    assert_match(s, '<nav')
    assert_match(s, 'aria-label="Pagination"')
end)

test("nav links carry hx-get + hx-target", function()
    local s = pag.nav(100, {
        per_page = 10, page = 1, base_url = "/items", target = "#feed",
    })
    -- Page 2 link (and prev/next links) should have htmx attrs
    assert_match(s, 'hx-get="/items?page=2"')
    assert_match(s, 'hx-target="#feed"')
end)

test("nav active page has no link (just current-page marker)", function()
    local s = pag.nav(100, { per_page = 10, page = 5, base_url = "/x", target = "#f" })
    assert_match(s, 'aria-current="page"')
    assert_match(s, 'class="hull-pagination-active">5</a>')
end)

test("nav has prev/next arrows when applicable", function()
    -- page 5 of 10 → both prev and next
    local s = pag.nav(100, { per_page = 10, page = 5, base_url = "/x", target = "#f" })
    assert_match(s, 'rel="prev"')
    assert_match(s, 'rel="next"')
end)

test("nav omits prev arrow on page 1", function()
    local s = pag.nav(100, { per_page = 10, page = 1, base_url = "/x", target = "#f" })
    if s:find('rel="prev"', 1, true) then
        error("page 1 should not have a prev arrow")
    end
    assert_match(s, 'rel="next"')
end)

test("nav default push_url=true", function()
    local s = pag.nav(100, { per_page = 10, page = 1, base_url = "/x", target = "#f" })
    assert_match(s, 'hx-push-url="true"')
end)

test("nav push_url=false omits hx-push-url", function()
    local s = pag.nav(100, {
        per_page = 10, page = 1, base_url = "/x", target = "#f", push_url = false,
    })
    if s:find('hx-push-url', 1, true) then
        error("hx-push-url should not be present")
    end
end)

test("nav default target is body", function()
    -- No target opt → falls back to "body"; works but apps usually
    -- want a container.
    local s = pag.nav(100, { per_page = 10, page = 1, base_url = "/x" })
    assert_match(s, 'hx-target="body"')
end)

print(string.format("\n%d/%d htmx-pagination tests passed", pass, pass + fail))
if fail > 0 then os.exit(1) end
