// Tests for hello example (JS)
// Run: hull test examples/hello/

test("GET / returns message", () => {
    const res = test.get("/");
    test.eq(res.status, 200);
    test.ok(res.json.message, "has message");
});

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});

test("GET /visits returns array", () => {
    const res = test.get("/visits");
    test.eq(res.status, 200);
    test.ok(res.body, "has body");
});

test("POST /echo returns body", () => {
    const res = test.post("/echo", {
        body: "hello world",
        headers: { "Content-Type": "text/plain" },
    });
    test.eq(res.status, 200);
    test.eq(res.json.body, "hello world");
});

test("GET /greet/:name returns greeting", () => {
    const res = test.get("/greet/World");
    test.eq(res.status, 200);
    test.eq(res.json.greeting, "Hello, World!");
});

test("POST /greet/:name returns greeting with body", () => {
    const res = test.post("/greet/World", {
        body: "payload",
        headers: { "Content-Type": "text/plain" },
    });
    test.eq(res.status, 200);
    test.eq(res.json.greeting, "Hello, World!");
    test.eq(res.json.body, "payload");
});
