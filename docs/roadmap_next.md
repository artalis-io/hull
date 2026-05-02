# Hull — Next Features Roadmap

Status: **Planned** | Created: 2026-05-01

## 1. WebSocket Support

**Priority:** High — #1 missing feature for real-time apps.

Keel already has WebSocket support at the C level (`src/websocket.c`,
`src/websocket_client.c`). Hull just needs scripting bindings.

**API:**
```lua
app.ws("/chat", {
    on_open = function(conn)
        log.info("connected: " .. conn.id)
    end,
    on_message = function(conn, msg)
        conn:send("echo: " .. msg)
        ws.broadcast("/chat", msg)  -- send to all connections on this path
    end,
    on_close = function(conn)
        log.info("disconnected: " .. conn.id)
    end,
})
```

**Tasks:**
- [ ] Lua bindings: `app.ws(path, handlers)`
- [ ] JS bindings: `app.ws(path, handlers)`
- [ ] Per-connection state management
- [ ] `ws.broadcast(path, message)` helper
- [ ] SSE support (`app.sse(path, handler)` — simpler alternative)
- [ ] Integration with async/event loop

## 2. PostgreSQL Backend

**Priority:** Medium — first non-SQLite backend using the DB vtable.

**Approach:**
- `HlDbBackend` implementation using libpq
- Connection string via `--db postgres://...`
- Statement caching via `PQprepare`
- Async queries via libpq async protocol (not worker threads)
- Hull internals (`_hull_*` tables) stay on embedded SQLite

**Tasks:**
- [ ] Vendor or link libpq
- [ ] `src/hull/cap/db_postgres.c` — PostgreSQL backend
- [ ] Connection pooling (single connection or pool?)
- [ ] Parameter binding (`$1` syntax vs `?`)
- [ ] Type mapping (HlValue ↔ Postgres types)

## 3. `hull deploy`

**Priority:** Medium — closes the dev-to-production loop.

**Options:**
```bash
hull deploy --docker          # Generate Dockerfile + build image
hull deploy --systemd         # Generate systemd unit file
hull deploy --cloudrun        # Generate cloud-run config
hull deploy --fly             # Generate fly.toml
```

**Tasks:**
- [ ] Dockerfile generation (FROM scratch, COPY hull binary, EXPOSE port)
- [ ] systemd unit template
- [ ] Cloud platform configs (optional)

## 4. Background Job Queue

**Priority:** Low — the outbox pattern covers most use cases.

**API:**
```lua
local jobs = require("hull.jobs")
jobs.init()  -- creates _hull_jobs table

-- Enqueue
jobs.enqueue("send_email", { to = "user@example.com", subject = "Hello" })

-- Process (called from app.every timer)
jobs.process(function(job)
    if job.type == "send_email" then
        smtp.send(job.data)
    end
end, { batch = 10, retry = 3 })
```

**Tasks:**
- [ ] `_hull_jobs` table schema (type, data, status, attempts, scheduled_at)
- [ ] `jobs.enqueue()` — insert with optional delay
- [ ] `jobs.process()` — claim + execute + update status
- [ ] Retry with exponential backoff
- [ ] Dead letter queue for failed jobs
