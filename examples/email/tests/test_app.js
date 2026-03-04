// Tests for email example (JS)
// Run: hull test examples/email/
//
// Note: smtp.send() will fail in test mode (no SMTP server running),
// but the error is caught and logged to the database. Validation and
// DB logging can be tested without a real SMTP server.

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});

// ── Validation ────────────────────────────────────────────────────

test("POST /send with valid body returns 200", () => {
    const res = test.post("/send", {
        body: '{"to":"user@example.com","subject":"Hello","body":"Test message"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 200);
    test.ok(res.json.id, "has id");
    // smtp.send fails in test mode, but we still get a response
});

test("POST /send missing to returns 400", () => {
    const res = test.post("/send", {
        body: '{"subject":"Hello","body":"Test"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
});

test("POST /send missing subject returns 400", () => {
    const res = test.post("/send", {
        body: '{"to":"user@example.com","body":"Test"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
});

test("POST /send missing body returns 400", () => {
    const res = test.post("/send", {
        body: '{"to":"user@example.com","subject":"Hello"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
});

test("POST /send invalid JSON returns 400", () => {
    const res = test.post("/send", {
        body: "not json",
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
    test.eq(res.json.error, "invalid JSON");
});

// ── Email log ─────────────────────────────────────────────────────

test("GET /sent returns array", () => {
    const res = test.get("/sent");
    test.eq(res.status, 200);
    test.ok(res.body, "has body");
});

test("GET /sent/:id returns 404 for missing", () => {
    const res = test.get("/sent/99999");
    test.eq(res.status, 404);
    test.eq(res.json.error, "not found");
});
