-- Webhooks — Hull + Lua example
--
-- Run: hull app.lua -p 3000
-- Webhook delivery and receipt with HMAC-SHA256 signatures
--
-- Register webhook endpoints, fire events that deliver to them,
-- and receive/verify incoming webhooks.

-- Manifest: allow outbound HTTP to localhost for webhook delivery
app.manifest({
    hosts = {"127.0.0.1"},
    env = {},
})

local SIGNING_SECRET = "whsec_change-me-in-production"

-- ── Schema ──────────────────────────────────────────────────────────

db.exec([[CREATE TABLE IF NOT EXISTS webhooks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    url TEXT NOT NULL,
    events TEXT NOT NULL,
    active INTEGER DEFAULT 1,
    created_at INTEGER
)]])

db.exec([[CREATE TABLE IF NOT EXISTS event_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_type TEXT NOT NULL,
    payload TEXT NOT NULL,
    created_at INTEGER
)]])

db.exec([[CREATE TABLE IF NOT EXISTS deliveries (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    webhook_id INTEGER NOT NULL,
    event_id INTEGER NOT NULL,
    status INTEGER,
    response_body TEXT,
    created_at INTEGER,
    FOREIGN KEY (webhook_id) REFERENCES webhooks(id)
)]])

-- ── Helpers ─────────────────────────────────────────────────────────

--- Convert a string to hex for use as HMAC key.
local function str_to_hex(s)
    local hex = {}
    for i = 1, #s do
        hex[i] = string.format("%02x", string.byte(s, i))
    end
    return table.concat(hex)
end

--- Sign a payload string with HMAC-SHA256, return hex signature.
local function sign_payload(payload_str)
    local key_hex = str_to_hex(SIGNING_SECRET)
    return crypto.hmac_sha256(payload_str, key_hex)
end

--- Deliver a webhook: POST the payload with signature header.
local function deliver_webhook(webhook, event_type, payload_str, event_id)
    local sig = sign_payload(payload_str)

    local ok, result = pcall(function()
        return http.post(webhook.url, payload_str, {
            headers = {
                ["Content-Type"] = "application/json",
                ["X-Webhook-Event"] = event_type,
                ["X-Webhook-Signature"] = "sha256=" .. sig,
            }
        })
    end)

    local status = 0
    local resp_body = ""
    if ok and result then
        status = result.status
        resp_body = result.body or ""
    else
        resp_body = tostring(result)
    end

    db.exec("INSERT INTO deliveries (webhook_id, event_id, status, response_body, created_at) VALUES (?, ?, ?, ?, ?)",
            { webhook.id, event_id, status, resp_body, time.now() })

    return status
end

-- ── Routes ──────────────────────────────────────────────────────────

app.get("/health", function(_req, res)
    res:json({ status = "ok" })
end)

-- Register a webhook
app.post("/webhooks", function(req, res)
    local body = json.decode(req.body)
    if not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local url = body.url
    local events = body.events  -- comma-separated event types, e.g. "user.created,order.placed"

    if not url or url == "" then
        return res:status(400):json({ error = "url is required" })
    end
    if not events or events == "" then
        return res:status(400):json({ error = "events is required" })
    end

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

-- Fire an event — delivers to all matching active webhooks
app.post("/events", function(req, res)
    local body = json.decode(req.body)
    if not body then
        return res:status(400):json({ error = "invalid JSON" })
    end

    local event_type = body.event
    local data = body.data

    if not event_type or event_type == "" then
        return res:status(400):json({ error = "event is required" })
    end

    local payload_str = json.encode({ event = event_type, data = data, timestamp = time.now() })

    -- Log the event
    db.exec("INSERT INTO event_log (event_type, payload, created_at) VALUES (?, ?, ?)",
            { event_type, payload_str, time.now() })
    local event_id = db.last_id()

    -- Find matching webhooks
    local webhooks = db.query("SELECT * FROM webhooks WHERE active = 1")
    local results = {}

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
            local status = deliver_webhook(wh, event_type, payload_str, event_id)
            results[#results + 1] = { webhook_id = wh.id, url = wh.url, status = status }
        end
    end

    res:json({ event_id = event_id, deliveries = results })
end)

-- List events
app.get("/events", function(_req, res)
    local rows = db.query("SELECT id, event_type, payload, created_at FROM event_log ORDER BY id DESC LIMIT 50")
    res:json(rows)
end)

-- List deliveries for a webhook
app.get("/webhooks/:id/deliveries", function(req, res)
    local rows = db.query("SELECT id, event_id, status, response_body, created_at FROM deliveries WHERE webhook_id = ? ORDER BY id DESC LIMIT 50",
                          { req.params.id })
    res:json(rows)
end)

-- ── Webhook receiver (verify incoming signatures) ───────────────────

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

    -- Compute expected signature and compare
    local expected_sig = sign_payload(req.body)
    if provided_sig ~= expected_sig then
        return res:status(401):json({ error = "invalid signature" })
    end

    local body = json.decode(req.body)
    log.info(string.format("Webhook received: %s", body and body.event or "unknown"))

    res:json({ received = true, event = body and body.event })
end)

log.info("Webhooks example loaded — routes registered")
