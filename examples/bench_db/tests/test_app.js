// Tests for bench_db example (JS)
// Run: hull test examples/bench_db/

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});

test("GET /read returns seeded rows", () => {
    const res = test.get("/read");
    test.eq(res.status, 200);
    test.ok(res.body, "has body");
});

test("POST /write inserts a row", () => {
    const res = test.post("/write");
    test.eq(res.status, 200);
    test.ok(res.json.inserted !== undefined, "has inserted count");
});

test("POST /write-batch inserts 10 rows", () => {
    const res = test.post("/write-batch");
    test.eq(res.status, 200);
    test.eq(res.json.inserted, 10);
});

test("GET /mixed does write + read", () => {
    const res = test.get("/mixed");
    test.eq(res.status, 200);
    test.ok(res.body, "has body");
});
