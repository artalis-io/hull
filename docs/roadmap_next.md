# Hull — Next Features Roadmap

Status: **Active** | Last reviewed: 2026-05-25

Companion to [`roadmap.md`](roadmap.md). That doc records what's built;
this one tracks the **next** feature batches in priority order.

For completed historical roadmaps see [`archive/roadmaps/`](archive/roadmaps/).

---

## Shipped since this file was last revised

- ✅ **v0.1.0 release** — Ed25519-signed `hull.sha256` manifest, four native+APE binaries (linux-x86_64, linux-aarch64, darwin-arm64, cosmo), one-line install script, `hull update` with signature verification. See [`../CHANGELOG.md`](../CHANGELOG.md).
- ✅ **gethull.dev landing site** — S3 + CloudFront, deploy-site workflow, browser verifier at `/verify.html` covering all three signature tiers, trust-chain section + honest scorecard.
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

## 2. `hull tools install` — side-loaded optional tools

**Priority:** Medium — closes the "wamrc not bundled" friction without
inflating the main hull binary, and establishes the pattern for
future optional tooling (wgpu-native, future analytics agents).

**Target:** v0.1.2

**Full design:** [`tools_install.md`](tools_install.md). Key shape:

- `hull tools install <name>` / `list` / `uninstall` subcommands.
- Tools land in `$HOME/.hull/tools/`, isolated from the user's PATH.
- Same release pubkey + `hull.sha256` signature covers tool binaries
  (no new keys, no new ceremonies).
- Version-coupled: `hull tools install` pulls from the SAME release
  as the running hull binary, so e.g. wamrc stays at the WAMR commit
  hull was compiled against.
- First concrete tool: `wamrc` (WAMR AOT compiler).
- Cosmo unsupported for tools that need LLVM (Cosmo users
  `make wamrc` from source).

**Tasks:**

- [ ] `src/hull/tools_install.c` — `hl_tools_dir()` + lookup helper.
- [ ] `src/hull/commands/tools.c` — dispatcher for `install` /
      `list` / `uninstall`, reusing `hl_release_verify_manifest_sig()`.
- [ ] Tool registry (static array — one entry today, easy to grow).
- [ ] Wire `cap/wasm.c` AOT path to consult `$HOME/.hull/tools/wamrc`
      before falling back to PATH lookup.
- [ ] Doctor section: WASM row reflects installed/outdated/missing
      states from the registry.
- [ ] Release workflow: `build-wamrc` matrix job (linux-x86_64,
      linux-aarch64, darwin-arm64), extend flatten / sha256 /
      gh-release-create steps.
- [ ] e2e test: install → exercise → uninstall.
- [ ] Docs: this entry, CLAUDE.md "Tools and side-loading" section,
      site mention.

**Out of scope (deferred):** wgpu-native (needs runtime dlopen
architecture change), system-wide install path (stay user-scoped),
`hull update --with-tools` auto-refresh.

---

## 3. Platform-sig completion — make `HL_PLATFORM_PUBKEY_HEX` meaningful

**Priority:** Medium — the cryptographic primitives, the embedded
pubkeys (`HL_PLATFORM_PUBKEY_HEX` in `signature.h`,
`GETHULL_DEV_PLATFORM_KEY` in `site/verify.html`), and the verifier
code paths (`hull verify`, browser verifier) all exist. But no signed
platform artefact is produced at release time, so `package.sig`'s
`platform` field is empty in every built app and the verifier has
nothing to check against. Closes the loudest gap on the v0.1.0
"honest scorecard" section of gethull.dev.

The release-key half of the same trust model shipped fully in v0.1.0
(release.yml signs `hull.sha256` → `hull update` verifies against
`HL_RELEASE_PUBKEY_HEX`). Platform-sig is the symmetric companion;
the architecture treats them as peers.

**Four missing pieces, in execution order:**

1. **`HULL_PLATFORM_CANARY` magic byte sequence actually emitted into
   the linked `libhull_platform.a`.** Structural prerequisite —
   without the canary present in the linked artefact, the scanner
   has nothing to find regardless of signing.

   Either a linker section (`__attribute__((section(...)))` with a
   well-known SHA-256 placeholder that a post-link tool rewrites) or
   a generated `canary.c` whose contents come from a SHA-256 of the
   rest of the .a. The latter is portable, easier to reason about,
   and matches the "compute first, embed second" pattern Hull
   already uses for `embedded_tcc.h`.

2. **A signed platform manifest produced at release time.** The
   release workflow runs `make platform` / `make platform-cosmo`,
   computes the SHA-256 + canary of each per-arch
   `libhull_platform.*.a`, builds a canonical JSON like

       {
         "platforms": {
           "linux-x86_64":   { "canary": "<hex>", "hash": "<hex>" },
           "linux-aarch64":  { "canary": "<hex>", "hash": "<hex>" },
           "darwin-arm64":   { "canary": "<hex>", "hash": "<hex>" },
           "cosmo-x86_64":   { "canary": "<hex>", "hash": "<hex>" },
           "cosmo-aarch64":  { "canary": "<hex>", "hash": "<hex>" }
         },
         "public_key": "<hex>",
         "signature":  "<hex>"
       }

   signs the canonical payload with `HULL_PLATFORM_KEY` (already in
   the GH secrets), and embeds the signed blob into the hull binary
   alongside the existing `.a` files (analogous to how the CA
   bundle gets embedded today).

3. **`hull build` passes the embedded platform-sig blob through into
   every built app's `package.sig`.** Today the `platform` field is
   mostly empty / placeholder. After (2), `hull build` extracts the
   embedded signed blob and writes it verbatim into
   `package.sig.platform` so downstream verifiers see something to
   check.

4. **`hull verify` and `--verify-sig` enforce platform-sig at
   startup.** Today both verify the app layer (outer). They should
   also verify `package.sig.platform.signature` against
   `HL_PLATFORM_PUBKEY_HEX` and refuse to start if it fails. The
   browser verifier already enforces this — it's just the CLI /
   runtime that need to catch up.

**Tasks:**

- [ ] `src/hull/platform_canary.c` (or linker-section equivalent) +
      post-link `compute_canary` step in the Makefile.
- [ ] Release workflow step: build platforms, compute canaries,
      emit `platform-manifest.json`, sign with
      `HULL_PLATFORM_KEY`, embed via `xxd`/`bin2c` into
      `embedded_platform_sig.h`.
- [ ] `hull build` reads the embedded blob, writes it into
      `package.sig.platform`.
- [ ] `src/hull/signature.c` — verify `package.sig.platform` against
      `HL_PLATFORM_PUBKEY_HEX`. Add to `hull verify` + `--verify-sig`
      runtime path.
- [ ] e2e test: build an app, mutate the embedded `.a` bytes, expect
      verify to fail with a clear message.
- [ ] Update `verify/index.html` test fixture + roadmap entry in
      `docs/security.md §6` to reflect that the platform layer is
      now active.

**Out of scope (defer to reproducible-builds Phase 9):** proving the
`.a` was built from the published source rather than a tampered
fork. Platform-sig only proves the bytes were endorsed by whoever
holds the platform key; reproducible builds prove WHAT got endorsed.

**Effort:** ~2-3 day of engineering + 1 day of release-engineering
once (3) lands and the rotation/test paths are exercised.
Realistically v0.1.2 or v0.2.0 depending on what else gets bundled.
Not a v0.1.0 retrofit — the wire format is what's missing, not the
trust roots, so existing v0.1.0 installs continue to work unchanged.

---

## 4. Background job queue (`hull.jobs`)

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

## 5. Email retry/backoff (Phase 7 candidate from Phase 6 audit)

**Priority:** Low — Phase 6 wrapped `email.js` / `email.lua` providers in
try/catch; now that errors surface cleanly as `{ok:false, error}`, retry on
transient failures is a small follow-up.

**Approach:** wrap `email.send` itself (not each provider) in an
`opts.retry = { max_attempts, base_delay_ms }` envelope. Use the same
exponential backoff math as `outbox.backoffDelay`.

---

## 6. Test coverage gaps surfaced by audits

Three items the audits flagged as deserving unit tests (currently e2e-only):

- [ ] **`hl_migrate_*`** — `src/hull/migrate.c` has no unit-test suite. Edge cases (checksum mismatch, missing migrations table, concurrent attempts) deserve in-process tests.
- [ ] **Sandbox profile builder** (`sandbox.c::build_seatbelt_profile` / unveil-path builder) — only e2e-covered today, and only on the platforms CI runs. A unit test calling the profile-build helper and asserting on the generated SBPL/unveil list would catch regressions on platforms CI doesn't run.
- [ ] **`hl_snprintf_append` helper** — Phase 5 audit recommendation. Replaces the brittle `req_len += snprintf(...)` idiom that recurs in `agent/request.c`, `cap/smtp.c`, and template codegen. Land the helper + tests + sweep the call sites.

---

## ✅ Done: pre-v0.1.0 release gate

Five operational steps that gated the v0.1.0 tag. All executed
2026-05-25:

1. ✅ Release keypair generated via `hull keygen release` and backed
   up offline.
2. ✅ Pubkey embedded into `HL_RELEASE_PUBKEY_HEX` at
   `include/hull/release.h` (commit `869f18a`).
3. ✅ `HULL_RELEASE_KEY` GitHub secret installed.
4. ✅ Platform keypair generated symmetrically and embedded as
   `HL_PLATFORM_PUBKEY_HEX` in `include/hull/signature.h` (commit
   `bad31b9`). `HULL_PLATFORM_KEY` secret installed too, even though
   the platform-sig wire format isn't active yet — see section 2.
5. ✅ `v0.1.0` tagged 2026-05-25, release workflow signed
   `hull.sha256` and published all six artefacts.

See [`release_signing.md`](release_signing.md) for the full flow and
the "Release Process" section of [`../CLAUDE.md`](../CLAUDE.md) for the
operational checklist.
