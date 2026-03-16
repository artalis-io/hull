// Tests for cors_manifest example (JS)
// Run: hull test examples/cors_manifest/
//
// Note: CORS middleware from app.manifest() does not run during hull test
// dispatch (same as app.use). We test the route handlers directly.

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});

test("GET /api/data returns stats fields", () => {
    const res = test.get("/api/data");
    test.eq(res.status, 200);
    test.ok(res.json.connections !== undefined, "has connections");
    test.ok(res.json.max !== undefined, "has max");
});

test("POST /api/data creates item", () => {
    const res = test.post("/api/data", {
        body: '{"name":"test"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 201);
    test.ok(res.json.created, "has created field");
});

test("POST /api/data rejects invalid JSON", () => {
    const res = test.post("/api/data", {
        body: "not json",
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
    test.eq(res.json.error, "invalid JSON");
});
