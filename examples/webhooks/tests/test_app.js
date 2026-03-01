// Tests for webhooks example (JS)
// Run: hull test examples/webhooks/
//
// Note: outbound HTTP delivery will fail in test mode (no actual server to
// call), but webhook registration, listing, and signature verification
// on the receive endpoint can be tested.

test("GET /health returns ok", () => {
    const res = test.get("/health");
    test.eq(res.status, 200);
    test.eq(res.json.status, "ok");
});

// ── Webhook registration ────────────────────────────────────────────

test("POST /webhooks registers a webhook", () => {
    const res = test.post("/webhooks", {
        body: '{"url":"http://127.0.0.1:9999/hook","events":"user.created"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 201);
    test.ok(res.json.id, "has id");
    test.eq(res.json.url, "http://127.0.0.1:9999/hook");
    test.eq(res.json.events, "user.created");
    test.eq(res.json.active, 1);
});

test("POST /webhooks requires url", () => {
    const res = test.post("/webhooks", {
        body: '{"events":"user.created"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
});

test("POST /webhooks requires events", () => {
    const res = test.post("/webhooks", {
        body: '{"url":"http://127.0.0.1:9999/hook"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
});

test("GET /webhooks lists registered webhooks", () => {
    // Register one
    test.post("/webhooks", {
        body: '{"url":"http://127.0.0.1:9999/list","events":"*"}',
        headers: { "Content-Type": "application/json" },
    });

    const res = test.get("/webhooks");
    test.eq(res.status, 200);
    test.ok(res.body, "has body");
});

test("DELETE /webhooks/:id deletes a webhook", () => {
    const create = test.post("/webhooks", {
        body: '{"url":"http://127.0.0.1:9999/del","events":"*"}',
        headers: { "Content-Type": "application/json" },
    });
    const id = create.json.id;

    const res = test.delete(`/webhooks/${id}`);
    test.eq(res.status, 200);
    test.eq(res.json.ok, true);
});

test("DELETE /webhooks/:id returns 404 for missing", () => {
    const res = test.delete("/webhooks/99999");
    test.eq(res.status, 404);
});

// ── Events ──────────────────────────────────────────────────────────

test("POST /events requires event field", () => {
    const res = test.post("/events", {
        body: '{"data":"test"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 400);
});

test("GET /events returns event log", () => {
    const res = test.get("/events");
    test.eq(res.status, 200);
});

// ── Webhook receiver (signature verification) ──────────────────────

test("POST /webhooks/receive rejects missing signature", () => {
    const res = test.post("/webhooks/receive", {
        body: '{"event":"test"}',
        headers: { "Content-Type": "application/json" },
    });
    test.eq(res.status, 401);
    test.eq(res.json.error, "missing signature");
});

test("POST /webhooks/receive rejects invalid signature", () => {
    const res = test.post("/webhooks/receive", {
        body: '{"event":"test"}',
        headers: {
            "Content-Type": "application/json",
            "X-Webhook-Signature": "sha256=0000000000000000000000000000000000000000000000000000000000000000",
        },
    });
    test.eq(res.status, 401);
    test.eq(res.json.error, "invalid signature");
});

test("POST /webhooks/receive rejects bad format", () => {
    const res = test.post("/webhooks/receive", {
        body: '{"event":"test"}',
        headers: {
            "Content-Type": "application/json",
            "X-Webhook-Signature": "invalid",
        },
    });
    test.eq(res.status, 401);
    test.eq(res.json.error, "invalid signature format");
});
