// Tests for timers example
// Run: hull test examples/timers/
//
// Note: hull test dispatches requests in-process without running the
// event loop, so background timers don't fire during tests. These tests
// verify the routes work correctly; timer behavior is tested in e2e.

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});

test("GET /heartbeats returns array", () => {
    const res = test.get("/heartbeats");
    test.eq(res.status, 200);
    test.ok(res.body, "response has body");
});

test("GET /counter returns array", () => {
    const res = test.get("/counter");
    test.eq(res.status, 200);
    test.ok(res.body, "response has body");
});
