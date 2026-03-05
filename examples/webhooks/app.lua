-- Webhooks — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- Webhook delivery and receipt with HMAC-SHA256 signatures
--
-- Features:
--   - Transactional outbox: event insert + delivery enqueue are atomic
--   - Inbox deduplication: incoming webhook receipts are deduplicated
--   - Idempotency keys: POST /events supports Idempotency-Key header
--   - Retry with exponential backoff via outbox.flush()

local validate    = require("hull.validate")
local idempotency = require("hull.middleware.idempotency")
local outbox      = require("hull.middleware.outbox")
local inbox       = require("hull.middleware.inbox")

-- Manifest: allow outbound HTTP to localhost for webhook delivery
app.manifest({
    env = {"WEBHOOK_SECRET"},
    hosts = {"127.0.0.1"},
})

-- env.get() is unavailable at load time (env_cfg wired after manifest extraction),
-- so fall back to a dev default.  Set WEBHOOK_SECRET in production.
local _ok, _val = pcall(env.get, "WEBHOOK_SECRET")
local SIGNING_SECRET = (_ok and _val) or "whsec_change-me-in-production"

-- ── Initialize middleware tables ──────────────────────────────────────

idempotency.init({ ttl = 86400 })
outbox.init({ max_attempts = 5 })
inbox.init({ ttl = 604800 })  -- 7 days

-- ── Post-body middleware ──────────────────────────────────────────────

-- Idempotency on POST /events (the critical non-idempotent endpoint)
app.use_post("POST", "/events", idempotency.middleware())

-- ── Helpers ─────────────────────────────────────────────────────────

--- Convert a string to hex for use as HMAC key.
local function str_to_hex(s)
    local hex = {}
    for i = 1, #s do
        hex[i] = string.format("%02x", string.byte(s, i))
    end
    return table.concat(hex)
end

local SECRET_HEX = str_to_hex(SIGNING_SECRET)

--- Sign a payload string with HMAC-SHA256, return hex signature.
local function sign_payload(payload_str)
    return crypto.hmac_sha256(payload_str, SECRET_HEX)
end

-- ── Routes ──────────────────────────────────────────────────────────

app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- Register a webhook
app.post("/webhooks", function(req, res)
    local decode_ok, body = pcall(json.decode, req.body)
    if not decode_ok or not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local ok, errors = validate.check(body, {
        url    = { required = true },
        events = { required = true },  -- comma-separated event types, e.g. "user.created,order.placed"
    })
    if not ok then
        return res:status(400):json({ errors = errors })
    end

    local url = body.url
    local events = body.events

    db.exec("INSERT INTO webhooks (url, events, created_at) VALUES (?, ?, ?)",
            { url, events, time.now() })
    local id = db.last_id()

    res:status(201):json({ id = id, url = url, events = events, active = 1 })
end)

-- List webhooks
app.get("/webhooks", function(_req, res)
    local rows = db.query("SELECT id, url, events, active, created_at FROM webhooks ORDER BY id")
    res:json(rows)
end)

-- Delete a webhook
app.del("/webhooks/:id", function(req, res)
    local changes = db.exec("DELETE FROM webhooks WHERE id = ?", { req.params.id })
    if changes == 0 then
        return res:status(404):json({ error = "webhook not found" })
    end
    res:json({ ok = true })
end)

-- Fire an event — atomically inserts event + enqueues deliveries via outbox
app.post("/events", function(req, res)
    local decode_ok, body = pcall(json.decode, req.body)
    if not decode_ok or not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local ok, errors = validate.check(body, {
        event = { required = true },
    })
    if not ok then
        return res:status(400):json({ errors = errors })
    end

    local event_type = body.event
    local data = body.data

    local payload_str = json.encode({ event = event_type, data = data, timestamp = time.now() })
    local sig = sign_payload(payload_str)

    local event_id
    local queued_count = 0

    -- Atomically: insert event + enqueue outbox deliveries in one transaction
    db.batch(function()
        -- Log the event
        db.exec("INSERT INTO event_log (event_type, payload, created_at) VALUES (?, ?, ?)",
                { event_type, payload_str, time.now() })
        event_id = db.last_id()

        -- Find matching webhooks and enqueue deliveries
        local webhooks = db.query("SELECT * FROM webhooks WHERE active = 1")

        for _, wh in ipairs(webhooks) do
            -- Check if webhook subscribes to this event type
            local match = false
            for evt in wh.events:gmatch("[^,]+") do
                local trimmed = evt:match("^%s*(.-)%s*$")
                if trimmed == event_type or trimmed == "*" then
                    match = true
                    break
                end
            end

            if match then
                outbox.enqueue({
                    kind = "webhook",
                    destination = wh.url,
                    payload = payload_str,
                    headers = json.encode({
                        ["Content-Type"] = "application/json",
                        ["X-Webhook-Event"] = event_type,
                        ["X-Webhook-Signature"] = "sha256=" .. sig,
                    }),
                    idempotency_key = "evt-" .. event_id .. "-wh-" .. wh.id,
                })
                queued_count = queued_count + 1
            end
        end
    end)

    -- Respond (cached by idempotency middleware if key was provided)
    idempotency.respond(req, res, 200, {
        event_id = event_id,
        webhooks_queued = queued_count,
    })

    -- Flush outbox: deliver enqueued webhooks (after commit)
    outbox.flush()
end)

-- Manually trigger outbox flush (for crash recovery or cron)
app.post("/outbox/flush", function(_req, res)
    local result = outbox.flush()
    res:json(result)
end)

-- Outbox stats
app.get("/outbox/stats", function(_req, res)
    res:json(outbox.stats())
end)

-- List events
app.get("/events", function(_req, res)
    local rows = db.query("SELECT id, event_type, payload, created_at FROM event_log ORDER BY id DESC LIMIT 50")
    res:json(rows)
end)

-- List deliveries for a webhook (from outbox)
app.get("/webhooks/:id/deliveries", function(req, res)
    local wh = db.query("SELECT url FROM webhooks WHERE id = ?", { req.params.id })
    if #wh == 0 then
        return res:status(404):json({ error = "webhook not found" })
    end
    local rows = db.query(
        "SELECT id, state, attempts, last_error, created_at, delivered_at FROM _hull_outbox WHERE destination = ? ORDER BY id DESC LIMIT 50",
        { wh[1].url }
    )
    res:json(rows)
end)

-- ── Webhook receiver (verify incoming signatures + inbox dedupe) ─────

app.post("/webhooks/receive", function(req, res)
    local sig_header = req.headers["x-webhook-signature"]
    if not sig_header then
        return res:status(401):json({ error = "missing signature" })
    end

    -- Extract hex signature from "sha256=<hex>"
    local provided_sig = sig_header:match("^sha256=(.+)$")
    if not provided_sig then
        return res:status(401):json({ error = "invalid signature format" })
    end

    -- Compute expected signature and compare (constant-time)
    local expected_sig = sign_payload(req.body)
    if #provided_sig ~= #expected_sig then
        return res:status(401):json({ error = "invalid signature" })
    end
    local diff = 0
    for i = 1, #provided_sig do
        diff = diff | (string.byte(provided_sig, i) ~ string.byte(expected_sig, i))
    end
    if diff ~= 0 then
        return res:status(401):json({ error = "invalid signature" })
    end

    local bd_ok, body_data = pcall(json.decode, req.body)
    local event_name = (bd_ok and body_data) and body_data.event or "unknown"

    -- Inbox deduplication: use webhook event header as message ID
    local event_id_header = req.headers["x-webhook-event-id"]
    if event_id_header then
        if inbox.check_and_mark(event_id_header, "webhook") then
            log.info(string.format("Webhook duplicate skipped: %s (id=%s)", event_name, event_id_header))
            return res:json({ received = true, duplicate = true })
        end
    end

    log.info(string.format("Webhook received: %s", event_name))
    res:json({ received = true, event = event_name })
end)

log.info("Webhooks example loaded — routes registered (outbox + inbox + idempotency)")
