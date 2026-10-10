-- test_kv_cache.lua - hull.cache / hull.kv memory + SQL stores, hull rbac names
--
-- Audit 5 (DA-L3..L6). Needs the capability layer (time, db), so it runs via
-- run_lua_test_in_runtime (tests/hull/runtime/lua/test_lua.c).

local cache = require("hull.cache")
local kv    = require("hull.kv")
local rbac  = require("hull.web.middleware.rbac")
local db    = require("hull.db").default()

local pass, fail = 0, 0

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

local function assert_true(v, msg)
    if not v then error((msg or "") .. " expected true", 2) end
end

local function err_code(f)
    local ok, e = pcall(f)
    if ok then return nil end
    return type(e) == "table" and e.code or tostring(e)
end

-- ── cache.new: LRU order, O(1) linked list ──────────────────────────

test("cache.new evicts the least recently used", function()
    local c = cache.new({ max_entries = 3 })
    c.set("a", 1); c.set("b", 2); c.set("c", 3)
    assert_eq(c.get("a"), 1)          -- a is now most recent; b is LRU
    c.set("d", 4)
    assert_eq(c.get("b"), nil, "b evicted")
    assert_eq(c.get("a"), 1); assert_eq(c.get("c"), 3); assert_eq(c.get("d"), 4)
    assert_eq(c.size(), 3)
end)

test("cache.new overwrite keeps count and refreshes recency", function()
    local c = cache.new({ max_entries = 2 })
    c.set("a", 1); c.set("b", 2)
    c.set("a", 10)                    -- a most recent; b LRU
    c.set("c", 3)
    assert_eq(c.get("b"), nil)
    assert_eq(c.get("a"), 10)
    assert_eq(c.size(), 2)
end)

test("cache.new delete / clear keep the list consistent", function()
    local c = cache.new({ max_entries = 2 })
    c.set("a", 1); c.set("b", 2)
    assert_true(c.delete("a"))
    assert_true(not c.delete("a"))
    c.set("c", 3); c.set("d", 4)      -- evicts b (LRU)
    assert_eq(c.get("b"), nil); assert_eq(c.get("c"), 3); assert_eq(c.get("d"), 4)
    c.clear()
    assert_eq(c.size(), 0)
    c.set("e", 5); c.set("f", 6); c.set("g", 7)
    assert_eq(c.get("e"), nil); assert_eq(c.get("g"), 7); assert_eq(c.size(), 2)
end)

test("cache.new flood of distinct keys stays bounded", function()
    local c = cache.new({ max_entries = 100 })
    for i = 1, 5000 do c.set("k" .. i, i) end
    assert_eq(c.size(), 100)
    assert_eq(c.get("k4901"), 4901)
    assert_eq(c.get("k4900"), nil)
end)

test("cache.new fetch touches", function()
    local c = cache.new({ max_entries = 2 })
    c.set("a", 1); c.set("b", 2)
    assert_eq(c.fetch("a", function() return 99 end), 1)
    c.set("c", 3)
    assert_eq(c.get("b"), nil)
    assert_eq(c.get("a"), 1)
end)

-- ── memory store (cache.open memory): overwrite larger than the budget ──

test("memstore overwrite never evicts its own key (DA-L3)", function()
    -- entry bytes = #k + #v + 48. Budget 1000: "a" with 600 bytes = 649.
    local h = cache.open({ backend = "memory", namespace = "t5-own", max_bytes = 1000 })
    h:set("a", string.rep("x", 600))
    -- 1100 bytes alone exceeds the budget: refused, and the old value stays.
    assert_eq(err_code(function() h:set("a", string.rep("y", 1100)) end),
              "capacity_exceeded")
    assert_eq(h:get("a"), string.rep("x", 600))
    local st = h._s:stats()
    assert_eq(st.items, 1); assert_eq(st.bytes, 1 + 600 + 48)
    -- a growing overwrite that fits once others go evicts the others, not it
    h:set("b", string.rep("b", 200))
    h:set("a", string.rep("z", 900))
    assert_eq(h:get("a"), string.rep("z", 900))
    assert_eq(h:get("b"), nil, "b evicted to make room")
    st = h._s:stats()
    assert_eq(st.items, 1); assert_eq(st.bytes, 1 + 900 + 48)
    assert_true(st.bytes >= 0)
end)

test("memstore LRU evicts by access order", function()
    local h = cache.open({ backend = "memory", namespace = "t5-lru", max_items = 3 })
    h:set("a", "1"); h:set("b", "2"); h:set("c", "3")
    assert_eq(h:get("a"), "1")
    h:set("d", "4")
    assert_eq(h:get("b"), nil)
    assert_eq(h:get("a"), "1"); assert_eq(h:get("c"), "3"); assert_eq(h:get("d"), "4")
    assert_eq(h._s:stats().items, 3)
end)

test("kv memory (no eviction) still refuses when full", function()
    local h = kv.open({ backend = "memory", namespace = "t5-kvfull", max_items = 1 })
    h:set("a", "1")
    assert_eq(err_code(function() h:set("b", "2") end), "capacity_exceeded")
    h:set("a", "22")                  -- an overwrite still fits
    assert_eq(h:get("a"), "22")
end)

-- ── SQL cache: expired rows first, amortised count (DA-L5) ──────────

test("sql cache purges expired rows before evicting live ones", function()
    local h = cache.open({ backend = "sqlite", database = db,
                           namespace = "t5-sql", max_items = 5 })
    -- The live rows are the OLDEST writes: evicting by write order without
    -- purging first dropped them and kept the dead rows written after.
    for i = 1, 5 do h:set("live" .. i, "v") end
    for i = 1, 6 do h:set("dead" .. i, "v", { ttl = 0 }) end   -- expired at once
    for i = 1, 5 do assert_eq(h:get("live" .. i), "v", "live" .. i) end
    assert_eq(h._s:stats().items, 5, "only the live rows are live")
end)

test("sql cache stays bounded and evicts the oldest writes", function()
    local h = cache.open({ backend = "sqlite", database = db,
                           namespace = "t5-sql2", max_items = 10 })
    for i = 1, 40 do h:set("k" .. i, tostring(i)) end
    local n = h._s:stats().items   -- the test is app code: no _hull_kv SELECT
    assert_true(n <= 10, "bounded: " .. tostring(n))
    assert_eq(h:get("k40"), "40")
end)

test("sql cache counts keys cas and incr create (audit 6 L4)", function()
    local h = cache.open({ backend = "sqlite", database = db,
                           namespace = "t6-sql3", max_items = 10 })
    for i = 1, 40 do h._s:incr("c" .. i, 1) end
    for i = 1, 40 do h._s:cas("l" .. i, nil, "x") end
    local n = h._s:stats().items
    assert_true(n <= 10, "bounded: " .. tostring(n))
end)

-- ── bounds: NaN / infinity / negative refused (audit 12) ────────────

test("cache.new refuses a bad max_entries / default_ttl / ttl", function()
    for _, m in ipairs({ 0/0, 1/0, -1, 0, 1.5, "10" }) do
        assert_true(not pcall(cache.new, { max_entries = m }), "max_entries " .. tostring(m))
    end
    for _, t in ipairs({ 0/0, 1/0, -1, "5" }) do
        assert_true(not pcall(cache.new, { default_ttl = t }), "default_ttl " .. tostring(t))
    end
    local c = cache.new({ max_entries = 2, default_ttl = 60 })
    for _, t in ipairs({ 0/0, 1/0, -5 }) do
        assert_true(not pcall(c.set, "k", 1, t), "ttl " .. tostring(t))
    end
    assert_eq(c.size(), 0, "nothing stored by a refused set")
    c.set("k", 1, 0); assert_eq(c.get("k"), nil, "ttl 0 expires at once")
    c.set("k", 2); assert_eq(c.get("k"), 2)
end)

test("kv / cache.open refuse a bad ttl, default_ttl, max_items, max_bytes", function()
    local h = kv.open({ backend = "memory", namespace = "t12-ttl" })
    for _, t in ipairs({ 0/0, 1/0, -1 }) do
        assert_eq(err_code(function() h:set("k", "v", { ttl = t }) end), "invalid_argument",
            "ttl " .. tostring(t))
    end
    assert_eq(h:get("k"), nil)
    h:set("k", "v", { ttl = false }); assert_eq(h:get("k"), "v", "false = no expiry")
    local nbad = 0
    for _, o in ipairs({ { default_ttl = 0/0 }, { default_ttl = -1 }, { default_ttl = 1/0 },
                         { max_items = 1/0 }, { max_bytes = 1/0 }, { max_items = 0/0 },
                         { max_items = -1 } }) do
        assert_eq(err_code(function()
            nbad = nbad + 1; o.backend = "memory"; o.namespace = "t12-bad-" .. nbad
            cache.open(o)
        end), "invalid_argument")
    end
    assert_eq(err_code(function()
        cache.open({ backend = "sqlite", database = db, namespace = "t12-sql",
                     default_ttl = 0/0 })
    end), "invalid_argument")
end)

-- ── rbac names (DA-L6) ──────────────────────────────────────────────

test("rbac refuses a missing / non-string / over-long name", function()
    rbac.init()
    assert_true(not pcall(rbac.assign, "u1", nil), "nil role")
    assert_true(not pcall(rbac.assign, "u1", 1), "numeric role")
    assert_true(not pcall(rbac.assign, "u1", ""), "empty role")
    assert_true(not pcall(rbac.define_role, string.rep("r", 256)), "256 bytes")
    assert_true(not pcall(rbac.define_role, "ok", { 5 }), "numeric permission")
    assert_true(not pcall(rbac.grant, "admin", nil), "nil permission")
    assert_true(pcall(rbac.define_role, string.rep("r", 255)), "255 bytes ok")
    -- Counted by character, as JS counts (audit 6 L6): 255 x 2-byte chars.
    assert_true(pcall(rbac.define_role, string.rep("\u{e9}", 255)), "255 chars ok")
    assert_true(not pcall(rbac.define_role, string.rep("\u{e9}", 256)), "256 chars")
    rbac.assign("u1", "editor")
    assert_true(rbac.has_role("u1", "editor"))
    assert_eq(rbac.has_role("u1", nil), false)
    assert_eq(rbac.has_role("u1", 1), false)
    assert_eq(rbac.has_permission("u1", nil), false)
end)

return { pass = pass, fail = fail }
