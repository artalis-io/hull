-- Tests for chat example
-- Run: hull test examples/chat/

test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.eq(res.json.status, "ok")
end)

test("GET /ws/connections returns count", function()
    local res = test.get("/ws/connections")
    test.eq(res.status, 200)
    test.eq(res.json.count, 0)
end)
