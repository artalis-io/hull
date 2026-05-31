-- test_pagination.lua — Tests for hull.web.pagination

local pagination = require('hull.web.pagination')

local pass = 0
local fail = 0

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

-- ── from_query ──────────────────────────────────────────────────────

test("from_query defaults to page=1 per_page=20 when no query", function()
    local p = pagination.from_query({})
    assert_eq(p.page, 1)
    assert_eq(p.per_page, 20)
    assert_eq(p.offset, 0)
    assert_eq(p.limit, 20)
end)

test("from_query reads ?page=3", function()
    local p = pagination.from_query({ query = { page = "3" } })
    assert_eq(p.page, 3)
    assert_eq(p.offset, 40)  -- (3-1) * 20
end)

test("from_query reads ?per_page=5", function()
    local p = pagination.from_query({ query = { per_page = "5" } })
    assert_eq(p.per_page, 5)
    assert_eq(p.limit, 5)
end)

test("from_query caps per_page at max_per_page", function()
    local p = pagination.from_query({ query = { per_page = "5000" } },
                                     { max_per_page = 50 })
    assert_eq(p.per_page, 50)
end)

test("from_query honors default_per_page", function()
    local p = pagination.from_query({}, { default_per_page = 10 })
    assert_eq(p.per_page, 10)
end)

test("from_query rejects invalid page values, falls back", function()
    local p1 = pagination.from_query({ query = { page = "abc" } })
    assert_eq(p1.page, 1)
    local p2 = pagination.from_query({ query = { page = "-5" } })
    assert_eq(p2.page, 1)
    local p3 = pagination.from_query({ query = { page = "3.5" } })
    assert_eq(p3.page, 1)
end)

test("from_query clamps page to at least 1", function()
    local p = pagination.from_query({ query = { page = "0" } })
    assert_eq(p.page, 1)
end)

-- ── render ──────────────────────────────────────────────────────────

test("render with total=0 has show=false, pages=1", function()
    local r = pagination.render(0, { page = 1, per_page = 20 })
    assert_eq(r.show, false)
    assert_eq(r.pages, 1)
    assert_eq(r.has_prev, false)
    assert_eq(r.has_next, false)
end)

test("render single page (total < per_page) has show=false", function()
    local r = pagination.render(5, { page = 1, per_page = 20 })
    assert_eq(r.pages, 1)
    assert_eq(r.show, false)
end)

test("render computes pages from ceiling", function()
    local r = pagination.render(21, { page = 1, per_page = 10 })
    assert_eq(r.pages, 3)  -- 21/10 rounded up
end)

test("render page=1 of 5: links are [1*,2,3,4,5], no ellipsis", function()
    local r = pagination.render(100, { page = 1, per_page = 20, window = 2 })
    assert_eq(r.pages, 5)
    assert_eq(#r.links, 5)
    assert_eq(r.links[1].active, true)
    assert_eq(r.links[1].page, 1)
    assert_eq(r.links[5].page, 5)
    -- no ellipsis (window=2 covers 1..5 entirely)
    for _, link in ipairs(r.links) do
        assert(not link.ellipsis, "no ellipsis expected")
    end
end)

test("render large total inserts ellipses around current", function()
    local r = pagination.render(400, {
        page = 10, per_page = 20, window = 2
    })
    assert_eq(r.pages, 20)
    -- Expected layout: 1 ... 8 9 10* 11 12 ... 20
    -- That's 9 entries (2 ellipsis + 7 page links)
    assert_eq(#r.links, 9)
    assert_eq(r.links[1].page, 1)
    assert_eq(r.links[2].ellipsis, true)
    -- find the active link
    local active_idx
    for i, link in ipairs(r.links) do
        if link.active then active_idx = i end
    end
    assert(active_idx, "active link present")
    assert_eq(r.links[active_idx].page, 10)
end)

test("render has_prev/has_next reflect boundaries", function()
    local first = pagination.render(100, { page = 1, per_page = 20 })
    assert_eq(first.has_prev, false)
    assert_eq(first.has_next, true)
    local last = pagination.render(100, { page = 5, per_page = 20 })
    assert_eq(last.has_prev, true)
    assert_eq(last.has_next, false)
end)

test("render prev/next URLs include base_url", function()
    local r = pagination.render(100, {
        page = 3, per_page = 20, base_url = "/todos"
    })
    assert_eq(r.prev, "/todos?page=2")
    assert_eq(r.next, "/todos?page=4")
end)

test("render URL preserves existing ? in base_url", function()
    local r = pagination.render(100, {
        page = 3, per_page = 20, base_url = "/search?q=foo"
    })
    assert_eq(r.next, "/search?q=foo&page=4")
end)

test("render URL omits per_page when matching default_per_page", function()
    local r = pagination.render(100, {
        page = 2, per_page = 10, default_per_page = 10, base_url = "/x"
    })
    assert_eq(r.next, "/x?page=3")
end)

test("render URL includes per_page when differing from default", function()
    local r = pagination.render(100, {
        page = 2, per_page = 5, default_per_page = 20, base_url = "/x"
    })
    assert_eq(r.next, "/x?page=3&per_page=5")
end)

test("render clamps page to [1, pages]", function()
    local r = pagination.render(50, { page = 99, per_page = 10 })
    assert_eq(r.page, 5)  -- clamped to max
    local r2 = pagination.render(50, { page = 0, per_page = 10 })
    assert_eq(r2.page, 1)
end)

return { pass = pass, fail = fail }
