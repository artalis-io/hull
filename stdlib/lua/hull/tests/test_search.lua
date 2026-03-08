-- test_search.lua — Tests for hull.search
--
-- Tests FTS5 full-text search wrapper.
-- Requires db global (run via C test harness with caps).

local search = require('hull.search')

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
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end

local function assert_true(v, msg)
    if not v then
        error((msg or "") .. " expected true, got " .. tostring(v))
    end
end

-- ── create_index and basic operations ───────────────────────────────

test("create_index: creates FTS5 table", function()
    search.create_index("articles", {"title", "body"})
    -- Should not error; table created
end)

test("index: insert document", function()
    search.index("articles", "1", {title = "Hello World", body = "This is a test article about searching"})
    search.index("articles", "2", {title = "Lua Programming", body = "Learn Lua programming language"})
    search.index("articles", "3", {title = "World News", body = "Breaking news from around the world"})
end)

test("query: basic text search", function()
    local results = search.query("articles", "world")
    assert_true(#results >= 2, "expected at least 2 results")
    -- Results should have id and rank
    assert_true(results[1].id ~= nil, "expected id field")
    assert_true(results[1].rank ~= nil, "expected rank field")
end)

test("query: specific term", function()
    local results = search.query("articles", "lua")
    assert_eq(#results, 1)
    assert_eq(results[1].id, "2")
end)

test("query: no results", function()
    local results = search.query("articles", "xyznonexistent")
    assert_eq(#results, 0)
end)

test("query: with limit", function()
    local results = search.query("articles", "world", { limit = 1 })
    assert_eq(#results, 1)
end)

-- ── remove ──────────────────────────────────────────────────────────

test("remove: deletes document", function()
    search.remove("articles", "3")
    local results = search.query("articles", "news")
    assert_eq(#results, 0)
end)

-- ── drop_index ──────────────────────────────────────────────────────

test("drop_index: removes table", function()
    search.drop_index("articles")
    -- Querying after drop should error
    local ok = pcall(search.query, "articles", "test")
    assert_eq(ok, false)
end)

-- ── validation ──────────────────────────────────────────────────────

test("create_index: rejects invalid name", function()
    local ok = pcall(search.create_index, "bad name!", {"col"})
    assert_eq(ok, false)
end)

test("create_index: rejects _hull_ prefix", function()
    local ok = pcall(search.create_index, "_hull_test", {"col"})
    assert_eq(ok, false)
end)

test("create_index: rejects invalid column names", function()
    local ok = pcall(search.create_index, "test", {"bad col!"})
    assert_eq(ok, false)
end)

-- ── snippet and highlight ───────────────────────────────────────────

test("query: with snippet", function()
    search.create_index("docs", {"title", "content"})
    search.index("docs", "1", {title = "Guide", content = "This is a comprehensive guide to full text searching in databases"})
    local results = search.query("docs", "guide", {
        snippet = { column = 2, tokens = 10, before = "<b>", after = "</b>" }
    })
    assert_true(#results >= 1, "expected at least 1 result")
    search.drop_index("docs")
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
