-- Tests for middleware example
-- Run: hull test examples/middleware/
--
-- Note: middleware registered via app.use() does not run during hull test
-- dispatch. This means request IDs, rate limiting, and CORS are not active
-- in these tests. We test the route handlers directly.

test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.eq(res.json.status, "ok")
end)

test("GET / returns message", function()
    local res = test.get("/")
    test.eq(res.status, 200)
    test.eq(res.json.message, "Middleware example")
end)

test("GET /api/items returns items list", function()
    local res = test.get("/api/items")
    test.eq(res.status, 200)
    test.ok(res.json.items, "has items")
end)

test("POST /api/items creates item", function()
    local res = test.post("/api/items", {
        body = '{"name":"test"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 201)
    test.ok(res.json.created, "has created field")
end)

test("GET /api/debug returns debug info", function()
    local res = test.get("/api/debug")
    test.eq(res.status, 200)
    test.ok(res.json.total_requests ~= nil, "has total_requests")
end)
