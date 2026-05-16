# Hull — Next Features Roadmap

Status: **Active** | Last reviewed: 2026-05-16

Companion to [`roadmap.md`](roadmap.md). That doc records what's built;
this one tracks the **next** feature batches in priority order.

For completed historical roadmaps see [`archive/roadmaps/`](archive/roadmaps/).

---

## Shipped since this file was last revised

- ✅ **WebSocket support** — `app.ws(path, handlers)` server + `ws.connect(url, …)` client, broadcast, per-connection state, host allowlist on client. Documented in [`../CLAUDE.md`](../CLAUDE.md) "WebSocket Endpoints".
- ✅ **SSE support** — `app.sse(path, handler)` with `stream:event(name, data, [id])` and `stream:close()`.
- ✅ **`hull deploy`** — Dockerfile, systemd, fly.toml targets; `hull agent deploy` JSON readiness analysis.
- ✅ **Extended `hull agent`** — 16 new subcommands (Phase 6) covering manifest preview, request preview, single-file validate, eval, schema-diff, sql-named, vfs/compute/gpu/perf/logs/template/compute-call. Wired into MCP. See [`agent_guide.md`](agent_guide.md) §5.

---

## 1. PostgreSQL backend (HlDbBackend)

**Priority:** Medium-High — first non-SQLite backend; validates the DB-vtable
abstraction that already powers `HL_ENABLE_DB=0` compute-only builds.

**Approach:**

- `HlDbBackend` implementation using libpq.
- Connection string via `--db postgres://…` or per-handler config.
- Statement caching via `PQprepare`.
- Async queries via libpq's async protocol (not worker threads — avoids the
  extra hop and gives us pipelining).
- Hull internals (`_hull_*` tables) **stay on embedded SQLite** so apps can
  be ported incrementally; only application tables move to Postgres.

**Tasks:**

- [ ] Vendor or dynamic-link libpq (choose: more deps but real prod use vs.
      simpler distribution)
- [ ] `src/hull/cap/db_postgres.c` — backend vtable impl
- [ ] Connection pooling (single connection vs. pool — start with pool)
- [ ] Parameter binding (Hull's `?` placeholder → Postgres `$1, $2…` rewrite)
- [ ] Type mapping (HlValue ↔ Postgres OIDs)
- [ ] Migration runner compatibility (Postgres dialect for `_hull_migrations`)

**Out of scope:** transactions across SQLite + Postgres (no XA / two-phase
commit). Apps that mix both must accept eventual consistency.

---

## 2. Background job queue (`hull.jobs`)

**Priority:** Low — the existing transactional outbox + inbox patterns cover
most reliable side-effect use cases. Add this when an explicit user need
appears (background image processing, scheduled cleanup, async chains).

**API sketch:**

```lua
local jobs = require("hull.jobs")
jobs.init()  -- creates _hull_jobs table

-- Enqueue
jobs.enqueue("send_email", { to = "user@example.com", subject = "Hello" })

-- Process (called from app.every() timer)
jobs.process(function(job)
    if job.type == "send_email" then return require("hull.email").send(job.data) end
end, { batch = 10, retry = 3 })
```

**Tasks:**

- [ ] `_hull_jobs` table schema (type, data, status, attempts, scheduled_at, last_error)
- [ ] `jobs.enqueue()` — insert with optional delay
- [ ] `jobs.process()` — atomic claim + execute + update status
- [ ] Retry with exponential backoff (reuse `outbox.backoffDelay` math)
- [ ] Dead-letter queue for permanently-failed jobs
- [ ] JS parity

---

## 3. Email retry/backoff (Phase 7 candidate from Phase 6 audit)

**Priority:** Low — Phase 6 wrapped `email.js` / `email.lua` providers in
try/catch; now that errors surface cleanly as `{ok:false, error}`, retry on
transient failures is a small follow-up.

**Approach:** wrap `email.send` itself (not each provider) in an
`opts.retry = { max_attempts, base_delay_ms }` envelope. Use the same
exponential backoff math as `outbox.backoffDelay`.

---

## 4. Test coverage gaps surfaced by audits

Three items the audits flagged as deserving unit tests (currently e2e-only):

- [ ] **`hl_migrate_*`** — `src/hull/migrate.c` has no unit-test suite. Edge cases (checksum mismatch, missing migrations table, concurrent attempts) deserve in-process tests.
- [ ] **Sandbox profile builder** (`sandbox.c::build_seatbelt_profile` / unveil-path builder) — only e2e-covered today, and only on the platforms CI runs. A unit test calling the profile-build helper and asserting on the generated SBPL/unveil list would catch regressions on platforms CI doesn't run.
- [ ] **`hl_snprintf_append` helper** — Phase 5 audit recommendation. Replaces the brittle `req_len += snprintf(...)` idiom that recurs in `agent/request.c`, `cap/smtp.c`, and template codegen. Land the helper + tests + sweep the call sites.

---

## 5. Strategic / pre-v0.1.0 (not actually engineering)

Five operational steps that gate the v0.1.0 tag. They need a human (key
material), not Claude:

1. Generate Ed25519 release keypair (`openssl genpkey -algorithm ed25519`).
2. Embed pubkey hex into `HL_RELEASE_PUBKEY_HEX` at `include/hull/release.h:18`.
3. Set GitHub secret `HULL_RELEASE_SIGNING_KEY` on the repo.
4. Back up the private key (offline / password manager).
5. `git tag v0.1.0 && git push --tags` → triggers `.github/workflows/release.yml`.

See [`release_signing.md`](release_signing.md) for the full flow.
