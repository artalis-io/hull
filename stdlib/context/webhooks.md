<!-- minimal -->
## Webhooks

Reliable webhook delivery (outbox), incoming deduplication (inbox), and response caching (idempotency).

```lua
-- Lua
local outbox = require("hull.web.middleware.outbox")
local inbox  = require("hull.web.middleware.inbox")

outbox.init()
inbox.init()

-- Send webhook reliably (inside a transaction)
db.batch(function()
    db.exec("INSERT INTO orders (id, total) VALUES (?, ?)", 1, 99)
    outbox.enqueue({ kind = "webhook", destination = "https://hooks.example.com/orders", payload = '{"order_id":1}' })
end)

-- Flush pending deliveries
outbox.flush()

-- Deduplicate incoming webhooks
if inbox.check_and_mark(message_id) then return end  -- duplicate, skip
```

```javascript
// JS
import { outbox } from "hull:web:middleware:outbox";
import { inbox } from "hull:web:middleware:inbox";

outbox.init();
inbox.init();

db.batch(() => {
    db.exec("INSERT INTO orders (id, total) VALUES (?, ?)", 1, 99);
    outbox.enqueue({ kind: "webhook", destination: "https://hooks.example.com/orders", payload: '{"order_id":1}' });
});

outbox.flush();
if (inbox.checkAndMark(messageId)) return; // duplicate
```

<!-- compact -->
## Transactional Outbox

Enqueue side-effects inside a database transaction. If the transaction rolls back, the outbox entry is also rolled back. No ghost deliveries.

**`outbox.init(opts?)`** — creates `_hull_outbox` table. `opts.max_attempts` (default: 5).

**`outbox.enqueue(opts)`:**
- `kind` — delivery type (e.g., `"webhook"`, `"email"`)
- `destination` — target URL or address
- `payload` — payload string (JSON, etc.)
- `headers` — JSON-encoded headers (optional)
- `idempotency_key` / `idempotencyKey` — dedup key for delivery (optional)

**`outbox.flush(opts?)`** — deliver pending items via HTTP. Exponential backoff on failure (`2^attempt * 10s`, capped at 1hr). Returns count of pending items.

**`outbox.stats()`** — `{ pending, delivered, failed }` counts.

**`outbox.cleanup(max_age)`** — delete delivered items older than `max_age` seconds.

## Inbox Deduplication

Prevents processing the same incoming webhook twice.

**`inbox.init(opts?)`** — creates `_hull_inbox_processed` table. `opts.ttl` (default: 86400).

- **`inbox.is_duplicate(message_id, source?)`** / `inbox.isDuplicate(...)` — check without marking
- **`inbox.mark_processed(message_id, source?, opts?)`** / `inbox.markProcessed(...)` — mark as processed
- **`inbox.check_and_mark(message_id, source?, opts?)`** / `inbox.checkAndMark(...)` — check + mark atomically. Returns `true` if duplicate, `false` if new.
- **`inbox.cleanup()`** — delete expired records, returns count.

## Idempotency Middleware

Caches POST responses by `Idempotency-Key` header. Replay the same key = get cached response.

```lua
local idempotency = require("hull.web.middleware.idempotency")
idempotency.init()
app.use_post("POST", "/api/*", idempotency.middleware())
```

- Cache hit + same request fingerprint: returns cached response (handler skipped)
- Cache hit + different fingerprint: returns 409 Conflict
- Fingerprint: `SHA-256(method + path + body)`
- Use `idempotency.respond(req, res, status, data)` to send + cache response
- Use `idempotency.complete(req)` to mark key processed without caching body

<!-- full -->
## Complete Webhook Pattern

```lua
local outbox      = require("hull.web.middleware.outbox")
local inbox       = require("hull.web.middleware.inbox")
local transaction = require("hull.web.middleware.transaction")

outbox.init()
inbox.init()

-- Periodic flush via background timer
app.every(30000, function()
    outbox.flush()
end)

-- Daily cleanup
app.daily("03:00", function()
    outbox.cleanup(86400 * 30)
    inbox.cleanup()
end)

-- Sending: enqueue in transaction for atomicity
app.post("/api/orders", function(req, res)
    transaction.run(function()
        db.exec("INSERT INTO orders (customer, total) VALUES (?, ?)",
            req.body.customer, req.body.total)
        local rows = db.query("SELECT last_insert_rowid() as id")
        local order_id = rows[1].id

        outbox.enqueue({
            kind = "webhook",
            destination = "https://hooks.example.com/orders",
            payload = string.format('{"order_id":%d,"total":%s}', order_id, req.body.total),
            headers = '{"Content-Type":"application/json"}',
        })
    end)
    res.status(201).json({ ok = true })
end)

-- Receiving: deduplicate incoming webhooks
app.post("/webhooks/payments", function(req, res)
    local msg_id = req.headers["x-webhook-id"]
    if not msg_id then
        return res.status(400).json({ error = "missing webhook ID" })
    end
    if inbox.check_and_mark(msg_id, "payments") then
        return res.json({ ok = true, duplicate = true })
    end
    -- Process the webhook (first time)
    db.exec("UPDATE orders SET paid = 1 WHERE id = ?", req.body.order_id)
    res.json({ ok = true })
end)
```

```javascript
import { outbox } from "hull:web:middleware:outbox";
import { inbox } from "hull:web:middleware:inbox";

outbox.init();
inbox.init();

app.every(30000, () => { outbox.flush(); });

app.post("/webhooks/payments", (req, res) => {
    const msgId = req.headers["x-webhook-id"];
    if (!msgId) return res.status(400).json({ error: "missing webhook ID" });
    if (inbox.checkAndMark(msgId, "payments")) {
        return res.json({ ok: true, duplicate: true });
    }
    db.exec("UPDATE orders SET paid = 1 WHERE id = ?", req.body.order_id);
    res.json({ ok: true });
});
```

## Outbox Retry Behavior

Failed deliveries are retried with exponential backoff: `2^attempt * 10s`, capped at 1 hour. After `max_attempts` (default 5), the item is marked as failed. Check with `outbox.stats()`.

## Manifest Requirement

Outbound HTTP requests require the destination host in the manifest:

```lua
app.manifest = {
    hosts = { "hooks.example.com" },
}
```
