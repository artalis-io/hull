// Tests for chat example (JavaScript)
// Run: hull test examples/chat/

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});

test("GET /ws/connections returns count", () => {
    const res = test.get("/ws/connections");
    test.eq(res.status, 200);
    test.eq(res.json.count, 0);
});
