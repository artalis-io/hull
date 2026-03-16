-- Tests for cors_manifest example
-- Run: hull test examples/cors_manifest/
--
-- Note: CORS middleware from app.manifest() does not run during hull test
-- dispatch (same as app.use). We test the route handlers directly.

test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.eq(res.json.status, "ok")
end)

test("GET /api/data returns stats fields", function()
    local res = test.get("/api/data")
    test.eq(res.status, 200)
    test.ok(res.json.connections ~= nil, "has connections")
    test.ok(res.json.max ~= nil, "has max")
end)

test("POST /api/data creates item", function()
    local res = test.post("/api/data", {
        body = '{"name":"test"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 201)
    test.ok(res.json.created, "has created field")
end)

test("POST /api/data rejects invalid JSON", function()
    local res = test.post("/api/data", {
        body = "not json",
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
    test.eq(res.json.error, "invalid JSON")
end)
