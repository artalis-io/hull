// Webhooks — Hull + QuickJS example
//
// Run: hull app.js -p 3000
// Webhook delivery and receipt with HMAC-SHA256 signatures
//
// Features:
//   - Transactional outbox: event insert + delivery enqueue are atomic
//   - Inbox deduplication: incoming webhook receipts are deduplicated
//   - Idempotency keys: POST /events supports Idempotency-Key header
//   - Retry with exponential backoff via outbox.flush()

import { app } from "hull:app";
import { crypto } from "hull:crypto";
import { db } from "hull:db";
import { env } from "hull:env";
import { log } from "hull:log";
import { idempotency } from "hull:middleware:idempotency";
import { inbox } from "hull:middleware:inbox";
import { outbox } from "hull:middleware:outbox";
import { time } from "hull:time";
import { validate } from "hull:validate";

// Manifest: allow outbound HTTP to localhost for webhook delivery
app.manifest({
    env: ["WEBHOOK_SECRET"],
    hosts: ["127.0.0.1"],
});

let SIGNING_SECRET = "whsec_change-me-in-production";
try { const v = env.get("WEBHOOK_SECRET"); if (v) SIGNING_SECRET = v; } catch (_e) {}

// ── Initialize middleware tables ──────────────────────────────────────

idempotency.init({ ttl: 86400 });
outbox.init({ maxAttempts: 5 });
inbox.init({ ttl: 604800 });  // 7 days

// ── Post-body middleware ──────────────────────────────────────────────

// Idempotency on POST /events (the critical non-idempotent endpoint)
app.usePost("POST", "/events", idempotency.middleware());

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

    const [ok, errors] = validate.check(body, {
        url:    { required: true },
        events: { required: true },
    });
    if (!ok) {
        return res.status(400).json({ errors });
    }

    const { url, events } = body;

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

// Fire an event — atomically inserts event + enqueues deliveries via outbox
app.post("/events", (req, res) => {
    let body;
    try { body = JSON.parse(req.body); } catch (_e) {
        res.status(400);
        res.json({ error: "invalid JSON" });
        return;
    }

    const [evOk, evErrors] = validate.check(body, {
        event: { required: true },
    });
    if (!evOk) {
        return res.status(400).json({ errors: evErrors });
    }

    const { event, data } = body;

    const payloadStr = JSON.stringify({ event, data, timestamp: time.now() });
    const sig = signPayload(payloadStr);

    let eventId;
    let queuedCount = 0;

    // Atomically: insert event + enqueue outbox deliveries in one transaction
    db.batch(() => {
        // Log the event
        db.exec("INSERT INTO event_log (event_type, payload, created_at) VALUES (?, ?, ?)",
                [event, payloadStr, time.now()]);
        eventId = db.lastId();

        // Find matching webhooks and enqueue deliveries
        const webhooks = db.query("SELECT * FROM webhooks WHERE active = 1");

        for (let i = 0; i < webhooks.length; i++) {
            const wh = webhooks[i];
            // Check if webhook subscribes to this event type
            const subscribed = wh.events.split(",").map(s => s.trim());
            if (subscribed.indexOf(event) !== -1 || subscribed.indexOf("*") !== -1) {
                outbox.enqueue({
                    kind: "webhook",
                    destination: wh.url,
                    payload: payloadStr,
                    headers: JSON.stringify({
                        "Content-Type": "application/json",
                        "X-Webhook-Event": event,
                        "X-Webhook-Signature": `sha256=${sig}`,
                    }),
                    idempotencyKey: `evt-${eventId}-wh-${wh.id}`,
                });
                queuedCount++;
            }
        }
    });

    // Respond (cached by idempotency middleware if key was provided)
    idempotency.respond(req, res, 200, {
        event_id: eventId,
        webhooks_queued: queuedCount,
    });

    // Flush outbox: deliver enqueued webhooks (after commit)
    outbox.flush();
});

// Manually trigger outbox flush (for crash recovery or cron)
app.post("/outbox/flush", (_req, res) => {
    const result = outbox.flush();
    res.json(result);
});

// Outbox stats
app.get("/outbox/stats", (_req, res) => {
    res.json(outbox.stats());
});

// List events
app.get("/events", (_req, res) => {
    const rows = db.query("SELECT id, event_type, payload, created_at FROM event_log ORDER BY id DESC LIMIT 50");
    res.json(rows);
});

// List deliveries for a webhook (from outbox)
app.get("/webhooks/:id/deliveries", (req, res) => {
    const wh = db.query("SELECT url FROM webhooks WHERE id = ?", [req.params.id]);
    if (!wh || wh.length === 0) {
        return res.status(404).json({ error: "webhook not found" });
    }
    const rows = db.query(
        "SELECT id, state, attempts, last_error, created_at, delivered_at " +
        "FROM _hull_outbox WHERE destination = ? ORDER BY id DESC LIMIT 50",
        [wh[0].url]
    );
    res.json(rows);
});

// ── Webhook receiver (verify incoming signatures + inbox dedupe) ─────

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

    let bodyData;
    try { bodyData = JSON.parse(req.body); } catch (_e) {
        res.status(400);
        res.json({ error: "invalid JSON" });
        return;
    }
    const eventName = bodyData ? bodyData.event : "unknown";

    // Inbox deduplication: use webhook event header as message ID
    const eventIdHeader = req.header("x-webhook-event-id");
    if (eventIdHeader) {
        if (inbox.checkAndMark(eventIdHeader, "webhook")) {
            log.info(`Webhook duplicate skipped: ${eventName} (id=${eventIdHeader})`);
            return res.json({ received: true, duplicate: true });
        }
    }

    log.info(`Webhook received: ${eventName}`);
    res.json({ received: true, event: eventName });
});

log.info("Webhooks example loaded — routes registered (outbox + inbox + idempotency)");
