// Webhooks — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Webhook delivery and receipt with HMAC-SHA256 signatures
//
// Register webhook endpoints, fire events that deliver to them,
// and receive/verify incoming webhooks.

import { app } from "hull:app";
import { crypto } from "hull:crypto";
import { db } from "hull:db";
import { env } from "hull:env";
import { http } from "hull:http";
import { log } from "hull:log";
import { time } from "hull:time";

// Manifest: allow outbound HTTP to localhost for webhook delivery
app.manifest({
    env: ["WEBHOOK_SECRET"],
    hosts: ["127.0.0.1"],
});

let SIGNING_SECRET = "whsec_change-me-in-production";
try { const v = env.get("WEBHOOK_SECRET"); if (v) SIGNING_SECRET = v; } catch (_e) {}

// ── Helpers ─────────────────────────────────────────────────────────

function secretToHex(secret) {
    let hex = "";
    for (let i = 0; i < secret.length; i++)
        hex += secret.charCodeAt(i).toString(16).padStart(2, "0");
    return hex;
}

const SECRET_HEX = secretToHex(SIGNING_SECRET);

function signPayload(payloadStr) {
    return crypto.hmacSha256(payloadStr, SECRET_HEX);
}

function deliverWebhook(webhook, eventType, payloadStr, eventId) {
    const sig = signPayload(payloadStr);

    let status = 0;
    let respBody = "";

    try {
        const r = http.post(webhook.url, payloadStr, {
            headers: {
                "Content-Type": "application/json",
                "X-Webhook-Event": eventType,
                "X-Webhook-Signature": `sha256=${sig}`,
            }
        });
        status = r.status;
        respBody = r.body || "";
    } catch (e) {
        respBody = e.message || String(e);
    }

    db.exec("INSERT INTO deliveries (webhook_id, event_id, status, response_body, created_at) VALUES (?, ?, ?, ?, ?)",
            [webhook.id, eventId, status, respBody, time.now()]);

    return status;
}

// ── Routes ──────────────────────────────────────────────────────────

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// Register a webhook
app.post("/webhooks", (req, res) => {
    let body;
    try { body = JSON.parse(req.body); } catch (_e) {
        res.status(400);
        res.json({ error: "invalid JSON" });
        return;
    }

    const { url, events } = body;

    if (!url) {
        return res.status(400).json({ error: "url is required" });
    }
    if (!events) {
        return res.status(400).json({ error: "events is required" });
    }

    db.exec("INSERT INTO webhooks (url, events, created_at) VALUES (?, ?, ?)",
            [url, events, time.now()]);
    const id = db.lastId();

    res.status(201).json({ id, url, events, active: 1 });
});

// List webhooks
app.get("/webhooks", (_req, res) => {
    const rows = db.query("SELECT id, url, events, active, created_at FROM webhooks ORDER BY id");
    res.json(rows);
});

// Delete a webhook
app.del("/webhooks/:id", (req, res) => {
    const changes = db.exec("DELETE FROM webhooks WHERE id = ?", [req.params.id]);
    if (changes === 0) {
        return res.status(404).json({ error: "webhook not found" });
    }
    res.json({ ok: true });
});

// Fire an event — delivers to all matching active webhooks
app.post("/events", (req, res) => {
    let body;
    try { body = JSON.parse(req.body); } catch (_e) {
        res.status(400);
        res.json({ error: "invalid JSON" });
        return;
    }

    const { event, data } = body;

    if (!event) {
        return res.status(400).json({ error: "event is required" });
    }

    const payloadStr = JSON.stringify({ event, data, timestamp: time.now() });

    // Log the event
    db.exec("INSERT INTO event_log (event_type, payload, created_at) VALUES (?, ?, ?)",
            [event, payloadStr, time.now()]);
    const eventId = db.lastId();

    // Find matching webhooks
    const webhooks = db.query("SELECT * FROM webhooks WHERE active = 1");
    const results = [];

    for (let i = 0; i < webhooks.length; i++) {
        const wh = webhooks[i];
        // Check if webhook subscribes to this event type
        const subscribed = wh.events.split(",").map(s => s.trim());
        if (subscribed.indexOf(event) !== -1 || subscribed.indexOf("*") !== -1) {
            const status = deliverWebhook(wh, event, payloadStr, eventId);
            results.push({ webhook_id: wh.id, url: wh.url, status });
        }
    }

    res.json({ event_id: eventId, deliveries: results });
});

// List events
app.get("/events", (_req, res) => {
    const rows = db.query("SELECT id, event_type, payload, created_at FROM event_log ORDER BY id DESC LIMIT 50");
    res.json(rows);
});

// List deliveries for a webhook
app.get("/webhooks/:id/deliveries", (req, res) => {
    const rows = db.query("SELECT id, event_id, status, response_body, created_at FROM deliveries WHERE webhook_id = ? ORDER BY id DESC LIMIT 50",
                          [req.params.id]);
    res.json(rows);
});

// ── Webhook receiver (verify incoming signatures) ───────────────────

app.post("/webhooks/receive", (req, res) => {
    const sigHeader = req.headers["x-webhook-signature"];
    if (!sigHeader) {
        return res.status(401).json({ error: "missing signature" });
    }

    // Extract hex signature from "sha256=<hex>"
    if (!sigHeader.startsWith("sha256=")) {
        return res.status(401).json({ error: "invalid signature format" });
    }
    const providedSig = sigHeader.substring(7);

    // Compute expected signature and compare (constant-time)
    const expectedSig = signPayload(req.body);
    if (providedSig.length !== expectedSig.length) {
        return res.status(401).json({ error: "invalid signature" });
    }
    let diff = 0;
    for (let i = 0; i < providedSig.length; i++) {
        diff |= providedSig.charCodeAt(i) ^ expectedSig.charCodeAt(i);
    }
    if (diff !== 0) {
        return res.status(401).json({ error: "invalid signature" });
    }

    let body;
    try { body = JSON.parse(req.body); } catch (_e) {
        res.status(400);
        res.json({ error: "invalid JSON" });
        return;
    }
    log.info(`Webhook received: ${body ? body.event : "unknown"}`);

    res.json({ received: true, event: body ? body.event : null });
});

log.info("Webhooks example loaded — routes registered");
