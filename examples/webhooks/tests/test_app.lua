-- Tests for webhooks example
-- Run: hull test examples/webhooks/
--
-- Note: outbound HTTP delivery will fail in test mode (no actual server to
-- call), but webhook registration, listing, and signature verification
-- on the receive endpoint can be tested.

test("GET /health returns ok", function()
    local res = test.get("/health")
    test.eq(res.status, 200)
    test.eq(res.json.status, "ok")
end)

-- ── Webhook registration ────────────────────────────────────────────

test("POST /webhooks registers a webhook", function()
    local res = test.post("/webhooks", {
        body = '{"url":"http://127.0.0.1:9999/hook","events":"user.created"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 201)
    test.ok(res.json.id, "has id")
    test.eq(res.json.url, "http://127.0.0.1:9999/hook")
    test.eq(res.json.events, "user.created")
    test.eq(res.json.active, 1)
end)

test("POST /webhooks requires url", function()
    local res = test.post("/webhooks", {
        body = '{"events":"user.created"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
end)

test("POST /webhooks requires events", function()
    local res = test.post("/webhooks", {
        body = '{"url":"http://127.0.0.1:9999/hook"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
end)

test("GET /webhooks lists registered webhooks", function()
    -- Register one
    test.post("/webhooks", {
        body = '{"url":"http://127.0.0.1:9999/list","events":"*"}',
        headers = { ["Content-Type"] = "application/json" },
    })

    local res = test.get("/webhooks")
    test.eq(res.status, 200)
    test.ok(res.body, "has body")
end)

test("DELETE /webhooks/:id deletes a webhook", function()
    local create = test.post("/webhooks", {
        body = '{"url":"http://127.0.0.1:9999/del","events":"*"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    local id = create.json.id

    local res = test.delete("/webhooks/" .. id)
    test.eq(res.status, 200)
    test.eq(res.json.ok, true)
end)

test("DELETE /webhooks/:id returns 404 for missing", function()
    local res = test.delete("/webhooks/99999")
    test.eq(res.status, 404)
end)

-- ── Events ──────────────────────────────────────────────────────────

test("POST /events requires event field", function()
    local res = test.post("/events", {
        body = '{"data":"test"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 400)
end)

test("GET /events returns event log", function()
    local res = test.get("/events")
    test.eq(res.status, 200)
end)

-- ── Webhook receiver (signature verification) ──────────────────────

test("POST /webhooks/receive rejects missing signature", function()
    local res = test.post("/webhooks/receive", {
        body = '{"event":"test"}',
        headers = { ["Content-Type"] = "application/json" },
    })
    test.eq(res.status, 401)
    test.eq(res.json.error, "missing signature")
end)

test("POST /webhooks/receive rejects invalid signature", function()
    local res = test.post("/webhooks/receive", {
        body = '{"event":"test"}',
        headers = {
            ["Content-Type"] = "application/json",
            ["X-Webhook-Signature"] = "sha256=0000000000000000000000000000000000000000000000000000000000000000",
        },
    })
    test.eq(res.status, 401)
    test.eq(res.json.error, "invalid signature")
end)

test("POST /webhooks/receive rejects bad format", function()
    local res = test.post("/webhooks/receive", {
        body = '{"event":"test"}',
        headers = {
            ["Content-Type"] = "application/json",
            ["X-Webhook-Signature"] = "invalid",
        },
    })
    test.eq(res.status, 401)
    test.eq(res.json.error, "invalid signature format")
end)
