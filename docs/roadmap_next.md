# Hull — Next Features Roadmap

Status: **Active** | Last reviewed: 2026-05-27

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
- ✅ **v0.1.2 batch** — `hull tools install/list/uninstall` (first tool: wamrc), shared `release_io.{c,h}` extracted from `commands/update.c`, top-level `hull help`, audit fixes (OOB defense, JSON escape, fsync/close checks, constant-time SHA-256 compare), agent surface expansion (`hull agent tools/overview` + `agent context --list` + wamrc state in agent compute), six new opinionated context docs (orientation, quickstart × 3, gpu, tools), discoverability breadcrumbs in `hull --help` + bare-hull + install.sh, `build-wamrc` CI matrix. See [`../CHANGELOG.md#012`](../CHANGELOG.md).

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

## 2. `hull tools install` — side-loaded optional tools  ✅ Shipped (v0.1.2)

**Design:** [`tools_install.md`](tools_install.md). What landed:

- `hull tools install <name>` / `list [--json]` / `uninstall` subcommands.
- Tools land in `$HOME/.hull/tools/` (mode 0755), isolated from PATH.
- Trust chain reuses the same Ed25519-signed `hull.sha256` manifest as
  `hull update` — no new keys, no new ceremonies.
- Version-coupled: pulls from the SAME release as the running hull
  binary (not "latest"), so e.g. wamrc stays at the WAMR commit hull
  was compiled against.
- First concrete tool: `wamrc` (WAMR AOT compiler), published for
  linux-x86_64 / linux-aarch64 / darwin-arm64. Cosmo unsupported
  (LLVM doesn't fit a fat APE binary) — cosmo users `make wamrc`.
- Shared `release_io.{c,h}` extracted from `commands/update.c` so both
  self-update and tool-install paths share the same HTTPS / SHA-256 /
  signature-verification / atomic-rename plumbing.
- `tool.find_tool()` Lua binding so `build.lua`'s wamrc resolver
  consults the canonical install location without reimplementing the
  4-step lookup in script.
- `hull doctor` reports installed / managed / unmanaged state.
- Audit fixes shipped together: OOB-read defense in
  `hl_release_io_find_checksum`, JSON-string escaper for
  `tools list --json` descriptions, fsync/close error checks in
  atomic-write, constant-time SHA-256 hex compares (both `tools` and
  `update` paths).

**Out of scope (deferred):** wgpu-native (needs runtime dlopen
architecture change), system-wide install path (stay user-scoped),
`hull update --with-tools` auto-refresh.

---

## 3. Platform-sig completion — make `HL_PLATFORM_PUBKEY_HEX` meaningful

**Status: SHIPPED in v0.1.3 (six commits, landed on `main`).** The
release pipeline now signs the per-arch `libhull_platform.a`
manifest with `HULL_PLATFORM_KEY` at release time, `hull build`
cross-checks the embedded `.a` against the inherited signed
manifest and writes it into `package.sig.platform.gethull`, and
both `hull verify` and runtime `--verify-sig` enforce the
gethull-layer signature against the real
`HL_PLATFORM_PUBKEY_HEX`. Browser verifier (`site/verify.html`)
matches. Escape valve `--no-verify-platform` exists on every
consumer for dev hulls and forks. Honest-scorecard bullet moved
from "Not yet" → "Ships"; new explicit out-of-scope note for
post-install binary integrity (an OS-layer concern).

The original plan and execution order are preserved below as
historical context for future readers tracing similar trust-chain
work.

**Priority:** High for v0.1.3 — this was the loudest remaining gap on
the v0.1.x "honest scorecard" and the symmetric companion to the
release-sig trust chain that shipped fully in v0.1.0 (release-side)
and matured in v0.1.2 (audit-hardened constant-time compare, OOB
defense, `release_io.{c,h}` shared between `hull update` and
`hull tools install`).

**Target:** v0.1.3 (shipped).

**Current state:** the cryptographic primitives, the embedded
pubkeys (`HL_PLATFORM_PUBKEY_HEX` in `signature.h`,
`GETHULL_DEV_PLATFORM_KEY` in `site/verify.html`), and the verifier
code paths (`hull verify`, browser verifier) all exist. But:

- `HL_PLATFORM_PUBKEY_HEX` is the all-zeros placeholder. v0.1.1
  reverted it from the real key after a test-only override Makefile
  rule got removed — the real key is in the GH secrets, just not
  embedded.
- No signed platform artefact is produced at release time, so
  `package.sig`'s `platform` field is empty in every built app and
  the verifier has nothing to check against.
- The browser verifier at `gethull.dev/verify.html` enforces the
  platform layer; the CLI and runtime do not.

### Why v0.1.2 unblocks this

v0.1.2 established the patterns this work needs to copy:

| v0.1.2 shipped | Reused here |
|---|---|
| `release_io.{c,h}` — HTTPS GET, signed-manifest verify, SHA-256, atomic write | Same module verifies the embedded platform-sig blob (no new code paths) |
| Audit-hardened trust chain (constant-time compare, OOB defense, fsync checks) | Platform-sig path is implemented against the same hardened helpers |
| `release.yml` matrix: build artifact → sha256 → Ed25519 sign → publish | Same shape applies to per-arch platform archives |
| Hex pubkey override via `-DHL_*_PUBKEY_HEX=…` in `release.h` | Already in `signature.h` too; tests can flip back to placeholder |

The architecture is right. What's missing is the wire format and
the release-time signing step.

### Locked design decisions

After scoping discussion 2026-05-27, three design questions were
resolved:

- **Verification strength: strong measurement.** Hull-binary build
  (in CI) emits per-arch `.a` SHA-256 as embedded constants in the
  hull binary text, alongside the signed manifest blob. `hull build`
  computes the SHA-256 of the `libhull_platform.a` it's actually
  embedding and cross-checks against the manifest entry before
  writing `package.sig.platform`. Runtime verify validates the
  signed-blob signature against `HL_PLATFORM_PUBKEY_HEX`. Stronger
  claim than transitive-trust; doesn't prove the linked code matches
  at runtime (that's reproducible-builds territory, Phase 9), but
  makes the trust path explicit in artifacts.
- **No canary.** Skipped entirely. The signed manifest + per-arch
  SHA-256 do all the integrity work the canary was hypothesized
  for. Avoids Makefile post-link gymnastics across cosmocc + Mach-O
  + ELF. Smaller diff, simpler reasoning.
- **Hard reject on empty/invalid `package.sig.platform`** in the
  explicit verify paths (`hull verify <app>` and `--verify-sig` at
  startup). Default `hull <app>` startup (no verify flag) keeps
  running unsigned apps — same opt-in behavior as today, so legacy
  v0.1.0–v0.1.2 apps continue working at runtime.

### Manifest format

Mirror `hull.sha256`'s shape — line-based text, not JSON:

```
0000000000000000000000000000000000000000000000000000000000000001  linux-x86_64
0000000000000000000000000000000000000000000000000000000000000002  linux-aarch64
0000000000000000000000000000000000000000000000000000000000000003  darwin-arm64
0000000000000000000000000000000000000000000000000000000000000004  cosmo-x86_64
0000000000000000000000000000000000000000000000000000000000000005  cosmo-aarch64
```

Why: avoids JSON canonicalization headaches entirely (deterministic
key order, whitespace, escaping). Signed against the file bytes.
Reuses every helper from `release_io.{c,h}` —
`hl_release_io_find_checksum`, `hl_release_io_sha256_hex`,
`hl_release_verify_manifest_sig`. Zero new format code.

### Three pieces, in execution order

**(A) Restore the real `HL_PLATFORM_PUBKEY_HEX`.** Replace the
all-zeros placeholder with the actual pubkey embedded for v0.1.0
(the secret half is already in the `HULL_PLATFORM_KEY` GH secret
and the pubkey is already in `site/verify.html`). Keep the
`#ifndef HL_PLATFORM_PUBKEY_HEX` override guard so tests can flip
back to placeholder.

**(B) Signed platform manifest produced at release time.** Extend
`release.yml` per Option A: reorganize so `build-platform` runs as
a matrix uploading per-arch `.a` artifacts → new
`sign-platform-manifest` single-Linux job downloads them, computes
SHA-256s, emits `platform-manifest.txt`, signs with
`HULL_PLATFORM_KEY`, `xxd`-embeds both into
`build/embedded_platform_sig.h` (signed manifest) and
`build/embedded_platform_hashes.h` (per-arch SHA-256 C constants)
→ `build-native` + `build-cosmo` (matrix) depend on the headers
artifact and include both during hull build. Bootstrap check:
fail loudly if `HULL_PLATFORM_KEY` is empty in CI.

**(C) `hull build` cross-check + `hull verify`/`--verify-sig`
enforce.** `hull build` computes SHA-256 of the
`libhull_platform.a` it's embedding, calls
`hl_platform_sig_extract_for_arch(this_arch)` (new C helper) to
get the expected hash + the signed blob, hard-rejects on mismatch
unless `--no-verify-platform` is passed, writes
`(manifest + sig + arch_hash)` into `package.sig.platform`. App
runtime verify against `HL_PLATFORM_PUBKEY_HEX` short-circuits to
hard-reject on empty (legacy apps) or invalid signature.

### Behavior matrix

| Scenario | Behavior |
|---|---|
| Release-built hull, embedded `.a` matches manifest | `hull build` writes signed platform-sig; runtime `--verify-sig` ✅ |
| Release-built hull, `.a` SHA-256 mismatch (user re-ran `make platform` locally on a release hull) | `hull build` **hard rejects** with: `"libhull_platform.a hash does not match the embedded signed manifest"` (use `--no-verify-platform` to override) |
| Self-built hull (no embedded manifest, dev workflow) | `hull build` **hard rejects** with: `"this hull was built locally and has no embedded platform manifest"` (use `--no-verify-platform` to build anyway; runtime verify will fail) |
| App with empty `package.sig.platform` at runtime + `--verify-sig` | **Hard reject**: `"app was built without platform-sig (rebuild against a release-built hull >=0.1.3)"` |
| App with empty `package.sig.platform` at runtime WITHOUT `--verify-sig` | No change from today — runs as-is. Default `hull <app>` doesn't verify signatures unless asked. |
| `--no-verify-platform` passed at any step | Skip the check, log once at info level. |

The `--no-verify-platform` flag exists on both `hull build` and the
runtime serve path. It's the documented escape valve for
dev-built hulls and for forensic-mode operation; expected to be
rare in production. Without it, self-built dev hulls can't build
production-ready apps — an acceptable strict-default tradeoff
matching the v0.1.2 audit-hardening posture.

### Six commits, ordered

The original draft of this plan put "restore the real pubkey" first
as a small spike commit. That sequencing was wrong: restoring the
pubkey activates `hl_verify_startup`'s platform-key pinning, which
hard-rejects any app whose `package.sig.platform.public_key_hex`
doesn't match the embedded key. Today's
`hull sign-platform` + `hull build --sign` developer flow (exercised
by `e2e_build.sh` Step 14) signs platforms with the developer's own
key — so the moment the real gethull pubkey is embedded, every
existing dev-signed app fails verify. The same failure mode triggered
the `ff0a39b` reversion during v0.1.1. C1 is therefore a *dependent*
commit, not an independent one.

The corrected order lands the chain bottom-up: helpers → CI →
build-side cross-check + opt-out flag → runtime enforcement →
THEN flip the pubkey + update the dev-flow e2e to use the
`--no-verify-platform` opt-out. Each step compiles and passes its
own tests; restoring the pubkey becomes safe only after every
consumer of the pin can opt out of it.

| # | Prefix | Summary | Effort |
|---|---|---|---|
| 1 | `sig:` | `src/hull/platform_sig.{c,h}` — manifest builder, signer, verifier, `extract_for_arch` helper. Pure data; reuses `release_io` helpers (`find_checksum`, `verify_manifest_sig`, `sha256_hex`). Unit tests with synthetic hashes including mismatch + tamper cases. Standalone — no runtime behavior change. | 1d |
| 2 | `ci:` | `release.yml` reorg per Option A: `build-platform` (matrix, uploads `.a`) → `sign-platform-manifest` (single Linux job, signs with `HULL_PLATFORM_KEY`, emits `build/embedded_platform_sig.h` + `build/embedded_platform_hashes.h`, uploads as artifact) → `build-native` + `build-cosmo` (matrix, depend on the headers artifact, include them in hull build). Bootstrap check fails the workflow if `HULL_PLATFORM_KEY` is empty in CI. Hull binaries now embed signed platform metadata but nothing reads it yet. | 1.5d |
| 3 | `build:` | `hull build` integration: new `--no-verify-platform` flag (works on both `hull build` and runtime), `tool.platform_sig_get()` Lua binding wrapping `hl_platform_sig_extract_for_arch()`, `build.lua` computes SHA-256 of embedded `.a`, cross-checks against manifest entry, writes verified `(manifest + sig + arch_hash)` into `package.sig.platform`. Hard-reject paths with the messages in the behavior matrix above. | 1d |
| 4 | `sig:` | `hull verify` + `--verify-sig` enforce platform layer at startup. Hard reject on empty/invalid with clear messages. `--no-verify-platform` opt-out at runtime mirrors the build-time flag. E2E test: build an app via the full release pipeline, mutate embedded `.a` bytes via hex editor, expect verify fails non-zero with the specific message. | 0.5d |
| 5 | `sig:` | Restore real `HL_PLATFORM_PUBKEY_HEX` (revert the v0.1.1 placeholder's hex value; keep the `#ifndef` override guard). Update `test_signature.c`'s `create_test_package_sig` to support a `platform_kind` parameter so `verify_startup_good` can emit `platform: null` (the unit test doesn't intend to exercise pinning). Update `tests/e2e_build.sh` Step 14 to add `--no-verify-platform` when running developer-signed apps under `--verify-sig` (the test is acting as a fork developer, not a gethull-signed app). This is the commit that *activates* the pin. | 0.5d |
| 6 | `docs:` | `docs/security.md §6` "shipped", `site/index.html` scorecard updates (move platform-sig bullet from "Not yet" to "Ships"), `site/verify.html` fixture with a real v0.1.3 example, roadmap section 3 → Shipped (v0.1.3), CHANGELOG entry. | 0.5d |

**Total: ~4.5 days.** Slightly less than the prior 5-day estimate
because the pubkey-restoration commit (C5) is bundled with the
narrow e2e_build.sh update it depends on, rather than being a
standalone 0.5d spike that turned out to need 0.5d of dependency
work anyway. CI reorg in C2 remains the highest-risk piece.

### Release-time validation

Same pattern as v0.1.2: tag `v0.1.3-rc1` first, watch the workflow,
run `tests/release_smoke.sh` (extended to also `hull verify` the
published binaries and confirm the platform layer reports valid).
Only tag clean `v0.1.3` after rc1 is green.

### Tasks (in dependency order — mirrors the commit table)

- [ ] `src/hull/platform_sig.{c,h}` — manifest builder + signer +
      verifier + `extract_for_arch` helper. Unit tests including
      mismatch + tamper cases. **[C1]**
- [ ] `release.yml` reorg: `build-platform` matrix +
      `sign-platform-manifest` job + dependency on
      `build-native`/`build-cosmo` jobs + bootstrap check on
      `HULL_PLATFORM_KEY` presence. Generates
      `embedded_platform_sig.h` + `embedded_platform_hashes.h`. **[C2]**
- [ ] `--no-verify-platform` flag on `hull build` + runtime.
      `tool.platform_sig_get()` Lua binding + `build.lua`
      integration writing `package.sig.platform` with the verified
      `(manifest + sig + arch_hash)`. **[C3]**
- [ ] `hull verify` + `--verify-sig` runtime enforcement;
      hard-reject paths with the documented error messages.
      E2E test: build app, mutate embedded `.a` bytes, expect
      verify fails non-zero with specific message. **[C4]**
- [ ] Restore real `HL_PLATFORM_PUBKEY_HEX` (revert the v0.1.1
      placeholder's hex value; keep the `#ifndef` guard). Update
      `test_signature.c` to support `platform_kind` param so the
      verify_startup unit test can emit `platform: null`. Update
      `e2e_build.sh` Step 14 to add `--no-verify-platform` for the
      developer-signed app path. **[C5]**
- [ ] Audit pass (mirror v0.1.2): OOB defense on
      `hl_platform_sig_extract_for_arch`, constant-time SHA-256
      compare in the cross-check path, fsync/close on any new
      atomic writes.
- [ ] Update `docs/security.md §6` to flip "platform layer
      inactive" → "shipped".
- [ ] Update `site/index.html` honest-scorecard: move
      platform-sig bullet from "Not yet" to "Ships".
- [ ] Update `site/verify.html` fixture with a real v0.1.3
      example.
- [ ] Post-release smoke: extend `tests/release_smoke.sh` to run
      `hull verify` against the published artifacts and confirm
      the platform layer reports valid.

### Out of scope

- **Reproducible builds** (Phase 9). Platform-sig proves bytes were
  endorsed by whoever holds the platform key; reproducible builds
  prove WHAT got endorsed. Separate concern.
- **Key rotation tooling.** The current model assumes the platform
  key doesn't rotate within a major version. If rotation is needed
  before v0.2, that's a separate commit batch (extending
  `HL_PLATFORM_PUBKEY_HEX` to a small array of accepted keys).
- **Backwards compatibility with v0.1.0/v0.1.1/v0.1.2 apps.** Those
  apps were built with hull versions that emit an empty
  `package.sig.platform`. They continue to run on v0.1.3+ hull
  normally (default `hull <app>` doesn't signature-check). They
  only fail under `hull verify` or `--verify-sig`, which is the
  documented intended behavior — opt-in stricter verification.
  Rebuild against v0.1.3+ hull to make those apps pass.

### Effort

Realistic estimate: **5 engineering days** (split roughly 4
engineering + 1 release-engineering, matching the v0.1.2 shape).
The CI reorg in step 3 is the highest-risk piece; the rc1 → smoke →
clean tag dance from v0.1.2 applies here too.

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
