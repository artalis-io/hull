-- Tests for health check + ETag middleware
-- Run: hull test examples/health_etag

local health = require("hull.web.middleware.health")
local etag = require("hull.web.middleware.etag")

-- ═══════════════════════════════════════════════════════════════════
-- Health Check Tests
-- ═══════════════════════════════════════════════════════════════════

-- Note: health middleware routes (/health, /ready) don't run in hull test
-- (middleware is not dispatched in test mode). Test the module API directly.
-- HTTP endpoints are tested via E2E tests.

test("health.register adds custom check", function()
    health.register("test_check", function() return true end)
    local result = health.run_checks()
    test.eq(result.checks.test_check.status, "ok")
    test.eq(result.all_ok, true)
    health.unregister("test_check")
end)

test("health.register failing check", function()
    health.register("bad_check", function() error("connection refused") end)
    local result = health.run_checks()
    test.eq(result.checks.bad_check.status, "fail")
    test.ok(result.checks.bad_check.error:find("connection refused"), "has error message")
    test.eq(result.all_ok, false)
    health.unregister("bad_check")
end)

test("health.register check returning false", function()
    health.register("false_check", function() return false end)
    local result = health.run_checks()
    test.eq(result.checks.false_check.status, "fail")
    test.eq(result.all_ok, false)
    health.unregister("false_check")
end)

test("health.unregister removes check", function()
    health.register("temp", function() return true end)
    health.unregister("temp")
    local result = health.run_checks()
    test.eq(result.checks.temp, nil)
end)

test("health.run_checks with db_check=false skips DB", function()
    local result = health.run_checks({ db_check = false })
    test.eq(result.checks.db, nil)
end)

test("health.run_checks includes DB by default", function()
    local result = health.run_checks()
    test.ok(result.checks.db, "has db check")
    test.eq(result.checks.db.status, "ok")
    test.ok(result.checks.db.latency_ms ~= nil, "db has latency")
    test.eq(result.all_ok, true)
end)

-- ═══════════════════════════════════════════════════════════════════
-- ETag Tests
-- ═══════════════════════════════════════════════════════════════════

test("etag.compute returns weak ETag", function()
    local tag = etag.compute("hello world")
    test.ok(tag, "returns a value")
    test.ok(tag:match('^W/"[a-f0-9]+"%s*$'), "weak ETag format: " .. tag)
    test.eq(#tag, 20) -- W/" + 16 hex + "
end)

test("etag.compute is deterministic", function()
    local tag1 = etag.compute("test data")
    local tag2 = etag.compute("test data")
    test.eq(tag1, tag2)
end)

test("etag.compute returns nil for empty", function()
    test.eq(etag.compute(""), nil)
    test.eq(etag.compute(nil), nil)
end)

test("etag.compute different for different input", function()
    local tag1 = etag.compute("hello")
    local tag2 = etag.compute("world")
    test.ok(tag1 ~= tag2, "different inputs produce different ETags")
end)

test("etag.matches returns false for no header", function()
    local req = { headers = {} }
    test.eq(etag.matches(req, 'W/"abc"'), false)
end)

test("etag.matches returns true for exact match", function()
    local tag = 'W/"a1b2c3d4e5f6a7b8"'
    local req = { headers = { ["if-none-match"] = tag } }
    test.eq(etag.matches(req, tag), true)
end)

test("etag.matches handles comma-separated list", function()
    local tag = 'W/"a1b2c3d4e5f6a7b8"'
    local req = { headers = { ["if-none-match"] = 'W/"other", ' .. tag .. ', W/"third"' } }
    test.eq(etag.matches(req, tag), true)
end)

test("etag.matches handles wildcard", function()
    local req = { headers = { ["if-none-match"] = "*" } }
    test.eq(etag.matches(req, 'W/"anything"'), true)
end)

test("etag.matches returns false for mismatch", function()
    local req = { headers = { ["if-none-match"] = 'W/"other"' } }
    test.eq(etag.matches(req, 'W/"different"'), false)
end)

test("GET /api/items returns ETag header", function()
    local res = test.get("/api/items")
    test.eq(res.status, 200)
    test.ok(res.json.items, "has items")
    local tag = res.headers["etag"]
    test.ok(tag, "has ETag header")
    test.ok(tag:match('^W/"[a-f0-9]+"'), "weak ETag format")
end)

test("GET /api/items with matching If-None-Match returns 304", function()
    -- First request: get the ETag
    local res1 = test.get("/api/items")
    test.eq(res1.status, 200)
    local tag = res1.headers["etag"]
    test.ok(tag, "first request has ETag")

    -- Second request: send If-None-Match with same ETag
    local res2 = test.get("/api/items", {
        headers = { ["If-None-Match"] = tag }
    })
    test.eq(res2.status, 304)
end)

test("GET /api/items with wrong If-None-Match returns 200", function()
    local res = test.get("/api/items", {
        headers = { ["If-None-Match"] = 'W/"wrongwrongwrong1"' }
    })
    test.eq(res.status, 200)
    test.ok(res.json.items, "has body")
end)

test("GET /api/greeting returns ETag", function()
    local res = test.get("/api/greeting?name=Test")
    test.eq(res.status, 200)
    test.ok(res.headers["etag"], "has ETag")
end)

test("GET /api/greeting 304 on match", function()
    local res1 = test.get("/api/greeting?name=Test")
    local tag = res1.headers["etag"]
    local res2 = test.get("/api/greeting?name=Test", {
        headers = { ["If-None-Match"] = tag }
    })
    test.eq(res2.status, 304)
end)

test("GET /api/greeting different name changes ETag", function()
    local res1 = test.get("/api/greeting?name=Alice")
    local res2 = test.get("/api/greeting?name=Bob")
    test.ok(res1.headers["etag"] ~= res2.headers["etag"], "different ETags for different content")
end)

test("GET /api/page returns ETag for HTML", function()
    local res = test.get("/api/page")
    test.eq(res.status, 200)
    test.ok(res.headers["etag"], "has ETag")
end)

test("POST /api/items does not return ETag", function()
    local res = test.post("/api/items", {
        body = '{"name":"TestItem"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 201)
    -- ETag is only for GET/HEAD
    test.eq(res.headers["etag"], nil)
end)

test("ETag changes when data changes", function()
    local res1 = test.get("/api/items")
    local tag1 = res1.headers["etag"]

    -- Insert new item
    test.post("/api/items", {
        body = '{"name":"NewItem"}',
        headers = { ["Content-Type"] = "application/json" },
    })

    local res2 = test.get("/api/items")
    local tag2 = res2.headers["etag"]

    test.ok(tag1 ~= tag2, "ETag changed after data modification")
end)
