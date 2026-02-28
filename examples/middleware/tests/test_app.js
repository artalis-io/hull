// Tests for middleware example (JS)
// Run: hull test examples/middleware/
//
// Note: middleware registered via app.use() does not run during hull test
// dispatch. This means request IDs, rate limiting, and CORS are not active
// in these tests. We test the route handlers directly.

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});

test("GET / returns message", () => {
    const res = test.get("/");
    test.eq(res.status, 200);
    test.eq(res.json.message, "Middleware example");
});

test("GET /api/items returns items list", () => {
    const res = test.get("/api/items");
    test.eq(res.status, 200);
    test.ok(res.json.items, "has items");
});

test("POST /api/items creates item", () => {
    const res = test.post("/api/items", {
        body: '{"name":"test"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 201);
    test.ok(res.json.created, "has created field");
});

test("GET /api/debug returns debug info", () => {
    const res = test.get("/api/debug");
    test.eq(res.status, 200);
    test.ok(res.json.total_requests !== undefined, "has total_requests");
});
