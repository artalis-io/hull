# Hull. Next Features Roadmap

Status: **Active** | Last reviewed: 2026-05-28

Companion to [`roadmap.md`](roadmap.md). That doc records what's built;
this one tracks the **next** feature batches in priority order.

For completed historical roadmaps see [`archive/roadmaps/`](archive/roadmaps/).

---

## Shipped since this file was last revised

- ✅ **v0.1.0 release**. Ed25519-signed `hull.sha256` manifest, four native+APE binaries (linux-x86_64, linux-aarch64, darwin-arm64, cosmo), one-line install script, `hull update` with signature verification. See [`../CHANGELOG.md`](../CHANGELOG.md).
- ✅ **gethull.dev landing site**. S3 + CloudFront, deploy-site workflow, browser verifier at `/verify.html` covering all three signature tiers, trust-chain section + honest scorecard.
- ✅ **WebSocket support**. `app.ws(path, handlers)` server + `ws.connect(url, …)` client, broadcast, per-connection state, host allowlist on client. Documented in [`../CLAUDE.md`](../CLAUDE.md) "WebSocket Endpoints".
- ✅ **SSE support**. `app.sse(path, handler)` with `stream:event(name, data, [id])` and `stream:close()`.
- ✅ **`hull deploy`**. Dockerfile, systemd, fly.toml targets; `hull agent deploy` JSON readiness analysis.
- ✅ **Extended `hull agent`**. 16 new subcommands (Phase 6) covering manifest preview, request preview, single-file validate, eval, schema-diff, sql-named, vfs/compute/gpu/perf/logs/template/compute-call. Wired into MCP. See [`agent_guide.md`](agent_guide.md) §5.
- ✅ **v0.1.2 batch**. `hull tools install/list/uninstall` (first tool: wamrc), shared `release_io.{c,h}` extracted from `commands/update.c`, top-level `hull help`, audit fixes (OOB defense, JSON escape, fsync/close checks, constant-time SHA-256 compare), agent surface expansion (`hull agent tools/overview` + `agent context --list` + wamrc state in agent compute), six new opinionated context docs (orientation, quickstart × 3, gpu, tools), discoverability breadcrumbs in `hull --help` + bare-hull + install.sh, `build-wamrc` CI matrix. See [`../CHANGELOG.md#012`](../CHANGELOG.md).
- ✅ **v0.1.3 batch**. Embedded release pubkey activated (`HL_RELEASE_PUBKEY_HEX` no longer the placeholder); `hull update` enforces signature verification with no warn-and-skip bypass. Platform-sig chain wire-format active (gethull-layer + per-app-layer split). Five-step pre-v0.1.0 release-gate process executed and documented. See [`../CHANGELOG.md#013`](../CHANGELOG.md).
- ✅ **v0.1.4 batch (§3.1 + §3.2 + §3.3)**. Cosmo APE `hull build` works on Linux (sandbox unveils widened for cosmocc; `hl_compiler_select` auto-detects cosmocc; Makefile WAMR invoker selection reordered). `hull eject` and `hull sign-platform` work on installed binaries via auto-extracted embedded platform library. Platform-sign chain polish: agent JSON output, doctor reporting, four follow-ups. See [`../CHANGELOG.md#014`](../CHANGELOG.md).
- ✅ **v0.1.5 batch**. `hull sbom` and `hull agent sbom` (four formats: human / JSON / CycloneDX 1.5 / SPDX 2.3), per-build auto-refresh via Makefile-injected submodule SHAs, build-flag-gated entries, runtime SHA-256 over embedded CA bundle. macOS reproducibility CI gate (was Linux-only with "verified locally on macOS" caveat; now `make reproducible-check` matrix-tested on both). `docs/POSITIONING.md` operational messaging guide (153 lines). Canonical thesis ("Code became disposable. Trust is not.") + descriptor unified across every Hull-mentioning surface. Site self-hosted (no third-party CDNs). OG card PNG variant. `LICENSING.md` vendored-dependency table. Em-dash sweep across 1100+ instances in all prose files. See [`../CHANGELOG.md#015`](../CHANGELOG.md).
- ✅ **v0.1.6 batch (§0.3.4 + §0.3.5 + §0.3.6 + §0.3.8 + §0.3.9 + §0.3.11 + §0.3.15)**. Trust chain end-to-end verifiable three independent ways: (a) the existing Ed25519 chain (gethull keys), (b) Sigstore + Rekor transparency log entry per release (`cosign verify-blob`), (c) SLSA build-provenance attestation per binary (`gh attestation verify`). `hull verify-self` one-command binary verification. Signed SBOM published as release artifacts (`hull.sbom.json` / `.cdx.json` / `.spdx.json`) covered by all three signature layers. `binary_sha256` field in `hull sbom` output. All GitHub Actions invocations SHA-pinned. Fork playbook (`docs/fork_playbook.md`, 259 lines) with substantive "why most organisations shouldn't fork" framing. Closes 7 of 15 trust-chain hardening items in [§0.3](#03-trust-chain-hardening-post-v015-gap-analysis). See [`../CHANGELOG.md#016`](../CHANGELOG.md).

---

## Licensing tiers (planning convention)

Hull is dual-licensed (see [`../LICENSING.md`](../LICENSING.md)). Roadmap
items are tagged inline to indicate which tier they ship under:

- **No tag (default).** Community tier (AGPL-3.0-or-later). Lives in the
  public `artalis-io/hull` repo. Distributed as source under AGPL.
- **`[COMMERCIAL]`.** Enterprise tier. Lives in `artalis-io/hull-enterprise`
  (separate private repo, commercial license only). Statically linked
  into customer binaries via the same `HL_ENABLE_*` flag pattern; the OSS
  Hull binary runs without any commercial code.
- **`[HOSTED]`.** Hosted service. Lives in `artalis-io/hull-hosted-services`
  (separate private repo). Not distributed as source under any license.

The community tier never shrinks. Nothing in this roadmap that lacks a
`[COMMERCIAL]` or `[HOSTED]` tag will ever move behind a commercial gate.
Enterprise-tier features are *additions*, not *removals*.

The current enterprise-tier feature register is maintained in
[`../LICENSING.md`](../LICENSING.md) "What's in each tier". Items in this
roadmap that match that list carry the `[COMMERCIAL]` tag inline.

---

## 0. Audit-pass follow-ups (small, named separately because they came
##    out of cross-surface positioning audits, not feature planning)

### 0.1 `hull sbom` subcommand. Vendored-component manifest as a live command  ✅ Shipped (v0.1.5)

Shipped in v0.1.5 substantially as designed below: `hull sbom` with
four formats (human / json / cyclonedx / spdx), `hull agent sbom`
alias for the JSON shape, per-build auto-refresh via Makefile
`-D` defines, build-flag gating (compute-only builds omit SQLite),
runtime SHA-256 over embedded blobs (Mozilla CA bundle, cached),
and the orthogonality canary in the test link. 16 test cases.

Below is the original design note, kept for context.

Convert the static vendored-dependency table in `LICENSING.md` into a
live, verifiable command. Closes the loop for compliance buyers
(defense, regulated, sovereign-AI): instead of trusting a doc, they
run one command on the binary in their hand and get the complete
provenance chain.

**Surface:**

- `hull sbom` (default: human-readable table; mirrors LICENSING.md
  style).
- `hull sbom --format=json` (flat array, agent-friendly).
- `hull sbom --format=cyclonedx` (industry standard, NTIA-aligned,
  preferred by defense/government).
- `hull sbom --format=spdx` (industry standard, more common in OSS
  compliance).
- `hull agent sbom` (alias for `--format=json`, fits the existing
  `hull agent` family).

**Per-component fields:**

- Name
- Version (semver where applicable, else `n/a`)
- Pinned commit hash (git submodule SHA at build time, baked in via
  `-DHULL_VENDOR_<NAME>_COMMIT="..."`)
- License (SPDX identifier preferred)
- Upstream source URL
- Author / maintainer
- Optional: SHA-256 of the embedded blob (CA bundle, TCC bytes, etc.)
  for tamper detection

**Must-include entries (beyond third-party):**

- Hull itself: name, version from `HL_VERSION`, commit, AGPL-3.0,
  link to repo.
- If `EMBED_PLATFORM=1`: the platform library's signed manifest hash
  and the gethull pubkey fingerprint. That closes the provenance
  chain end-to-end.

**Generation strategy:**

- Vendor versions, commits, licenses, URLs in a single C array in
  `src/hull/sbom.c` (or alongside `build_assets.c`).
- Populated at build time by the Makefile via
  `-DHULL_VENDOR_<NAME>_COMMIT=$(shell git -C vendor/<name> rev-parse HEAD)`
  for each submodule, plus static fields (license, URL) from a
  table-of-record in the source.
- Build flags (`HL_ENABLE_WASM`, `HL_ENABLE_GPU`, `HL_ENABLE_DB`,
  etc.) gate which entries are reported. A compute-only build's SBOM
  correctly omits SQLite.

**Why a separate subcommand, not a flag on `hull doctor`:** `doctor`
is the "is this install ready to build/run?" check (toolchain, CA
bundle, embedded platform). `sbom` is "what provenance does this
binary have?" Different concern; mixing them dilutes both.

**Effort:** ~50-line subcommand + ~80-line static table + 3
Makefile lines per vendored dep. Small relative to the credibility
win for compliance personas.

**Priority:** Tier 1 audit-credibility booster. Enterprise/compliance
buyers (Lisa-the-Defense-Contractor persona) buy this directly.

### 0.2 Byte-reproducible builds  ✅ Shipped

Three reproducibility properties, all CI-gated on both Linux and
macOS as of v0.1.5:

1. `make` produces a byte-identical `build/hull` from the same
   source tree, between rebuilds.
2. `hull build` produces a byte-identical app binary from the same
   source + same hull version (built to the same output path; see
   below on why path matters).
3. `make self-build` proves hull is self-hostable across all
   platforms (hull builds hull2 builds hull3).

Investigation arc — recorded for posterity because the wrong turns
were instructive:

- Initial assumption: linker-embedded entropy (random LC_UUID on
  macOS, random Build-ID GUID on Linux) was the blocker.
  Wrong on both counts.
- Tried `-Wl,-no_uuid` on macOS — broke binary (modern dyld
  REQUIRES LC_UUID, aborts with "missing LC_UUID load command").
- Tried `-Wl,--build-id=none` in `hull build` link path — broke
  test_compiler's link tests (TCC backend delegates to system
  sys_link).
- Tried `ZERO_AR_DATE=1` for macOS ar mtimes — made vendored
  archives deterministic, but the final-link delta persisted.
- Real macOS finding: `ld64` hashes the **output path** into
  LC_UUID. Same input + same output path → same LC_UUID. The
  "~47 bytes differ" the test reported was because it built to
  two different output paths. Fix: same-path methodology.
- Real Linux finding: `hull build` creates a random tempdir per
  invocation (`/tmp/hull_XXXXXX/`). On Linux, the .o file's
  embedded source-name (STT_FILE symbol) hashes into GNU ld's
  Build-ID — different tempdir → different .o → different
  Build-ID → different binary. Fix: `-ffile-prefix-map=<srcdir>=.`
  in `sys_compile` to strip the tempdir from .o content. Also
  forces `--compiler=system` in the test because TCC doesn't have
  the equivalent flag (its determinism story is a smaller
  follow-up).
- libhull_platform.a was non-reproducible across rebuilds: BSD ar
  on macOS embeds mtimes by default. Fix: `export ZERO_AR_DATE := 1`
  in Hull's Makefile (no-op on Linux where GNU ar is already
  deterministic by distro default; effective on macOS).

Total: at least 6 commits across two audit rounds chasing this.
Final state: three CI gates, all green, MANIFESTO claim true at
all three layers.

**Lesson learned (the meta-one):** when an audit finding hinges on
a test result, verify the test's METHODOLOGY before declaring the
underlying system broken. The "macOS LC_UUID is unfixable" and
"Linux ar mtimes are the blocker" theories were both produced by
tests with subtle methodology bugs (wrong output paths, ignored
flags). Real determinism investigation needs same-input + same-
operation comparison; anything else measures the test, not the
system.

**TCC follow-up (minor):** the `--compiler=tcc` codepath still has
the tempdir-in-.o issue because TCC doesn't support
`-ffile-prefix-map`. The reproducibility CI test sidesteps this
with `--compiler=system`, which is the documented production path.
Closing TCC determinism would need either a TCC patch or a
post-compile path-strip pass.

---

### 0.3 Trust-chain hardening (post-v0.1.5 gap analysis)

The v0.1.5 trust story is stronger than 99% of dev-tool shipping
today: reproducible builds on Linux + macOS, three-layer Ed25519
signature chain, live SBOM, embedded CA bundle with runtime hash,
`hull update` with sig + SHA-256 verification, browser verifier.

These items separate "verifiable" from "verifiable against a
serious adversary." Procurement / compliance / sovereign-deployment
buyers WILL ask about most of them in the first conversation, so
prioritise accordingly. Items are grouped by load-bearing severity,
not implementation cost.

#### Tier 1. Load-bearing (the trust claim depends on these)

- [ ] **0.3.1. Pin the CI build environment.** `ubuntu-latest` and
  `macos-latest` are mutable labels. GitHub rotates the runner
  image without notice. Reproducibility passes today against
  today's image. Six months from now, a rebuild of v0.1.5 source
  against the new image may not reproduce. **Fix:** at minimum pin
  to a specific runner version (e.g. `ubuntu-24.04`-with-image-SHA),
  ideally move to a digest-pinned Docker base or a Nix flake. Without
  this, "reproducible" has a sliding expiry date and the
  reproducibility-from-clean-room claim is only true in a narrow
  time window. **Effort:** low for runner-pin; medium for Docker
  base; high for Nix flake. Start with runner-pin.

- [ ] **0.3.2. Bootstrap trust path.** The `curl ... | sh` install
  is TOFU on top of TLS to `raw.githubusercontent.com` + GitHub
  account integrity + `install.sh` content trust. A new user has
  no out-of-band way to verify they got the real installer before
  running it. Once installed, the chain is tight; getting there is
  not. **Fix:** publish a detached PGP signature on `install.sh`
  itself, or a Sigstore signature, with verification instructions
  on gethull.dev. Chicken-and-egg: the verification path can't
  require an existing hull. PGP via a well-known maintainer key
  (or a Sigstore identity tied to gethull's GitHub) is the realistic
  option. Document the "I don't trust GitHub: how do I verify
  install.sh" path. **Effort:** medium. Mostly social-engineering
  (key publication, KEYS file maintenance, docs) rather than code.

- [ ] **0.3.3. Move release signing off GitHub Actions secrets.**
  The release private key currently lives encrypted in GitHub's
  KMS as a workflow secret. Compromise of the gethull GitHub org
  gives an attacker the ability to sign arbitrary artifacts as
  gethull. No HSM, no hardware token, no offline ceremony, no
  multi-party signing. **Fix progression:** (a) document the
  current threat model honestly in `release_signing.md`; (b) move
  to a YubiKey OpenPGP / Ed25519 token operated by a person, with
  CI just preparing the manifest for human-attested signing; (c)
  multi-party signing for major releases. **Effort:** (a) hours,
  (b) days + operational discipline, (c) weeks + organisational
  process. Even (a) materially improves the story.

- [x] **0.3.4. Publish to a transparency log. ✅ Shipped.** Without an
  append-only public log of `(release_tag, commit_sha,
  manifest_sha256, signature)`, a future key compromise could
  backdate releases or sign retroactive artifacts and there's no
  proof-of-non-existence. **Fix:** publish each release triple to
  Sigstore's free public Rekor log on tag. Hull's own embedded
  pubkey continues to sign; Rekor provides the "we couldn't have
  signed this earlier" timestamp. **Effort:** low. One workflow
  step using `cosign sign-blob --rekor-url`.

#### Tier 2. Industry standards Hull doesn't meet yet

- [x] **0.3.5. SLSA build provenance. ✅ Shipped.** GitHub Actions natively
  supports `actions/attest-build-provenance` which produces a
  Sigstore-signed attestation tying a binary to a specific commit
  + workflow run + runner image. This is free, requires no Hull
  signing key, and gives anyone "GitHub built this from this
  commit" without trusting gethull's own keys. **Fix:** add one
  workflow step per release artifact. Publish attestations
  alongside `hull.sha256.sig`. **Effort:** hours. Genuinely
  low-hanging fruit and high credibility win.

- [x] **0.3.6. SHA-pin GitHub Actions invocations. ✅ Shipped.** Workflows
  use mutable tags (`actions/checkout@v4`, `actions/setup-node@v4`,
  etc.). A compromised action publisher could ship code under the
  existing tag and Hull's CI would silently pick it up. **Fix:**
  pin every action invocation to a commit SHA. Dependabot supports
  SHA-pinned actions natively and will PR updates. **Effort:** one
  pass through `.github/workflows/*.yml`.

- [ ] **0.3.7. Cosmo APE in the reproducibility matrix.** The
  current `Reproducible build` job matrixes Linux + macOS only.
  Cosmo is built and shipped (`hull-cosmo` is a release asset)
  but its byte-reproducibility is untested. Either add Cosmo to
  the matrix or explicitly document that Cosmo repro is out of
  scope. **Effort:** medium. Cosmo's two-arch link adds
  complexity; investigate whether `cosmocc` + `apelink` produce
  deterministic output before committing.

- [x] **0.3.8. `hull verify-self` command. ✅ Shipped.** A running hull binary
  can be tampered with on disk. There's no built-in command to
  verify the running binary against its own signed release
  manifest. Today users have to manually `hull verify-release` +
  `sha256sum` + manual hash compare. **Fix:** one subcommand that
  reads its own argv[0] path, computes SHA-256, fetches (or uses
  embedded) release manifest, verifies sig, compares hashes. Honest
  errors if the running binary is local-dev (not in the manifest).
  **Effort:** small. Reuses `release_io.{c,h}` plumbing.

- [x] **0.3.9. Sign the SBOM output. ✅ Shipped.** `hull sbom` produces JSON
  that's cryptographically unbounded. A tampered hull could lie
  about its SBOM. **Fix:** two options, not mutually exclusive:
  (a) publish `hull-<arch>.sbom.json` + `hull-<arch>.sbom.json.sig`
  as release artifacts so the SBOM is independently verifiable; or
  (b) include `hull_binary_sha256` and the release-signature
  bytes inside the SBOM itself, so the SBOM output cross-references
  the release manifest. Option (b) is more elegant; (a) is more
  inspectable. Do both. **Effort:** small. Extends the static
  entry table + adds a release-workflow step.

- [ ] **0.3.10. Key rotation and revocation procedure.**
  `docs/release_signing.md` describes how signing works but not
  what happens when (not if) the release or platform key is
  compromised. No documented rotation cadence, no kill-switch
  process, no chain-of-trust progression where the next pubkey is
  signed by the current pubkey. **Fix:** written rotation playbook
  + ideally a built-in mechanism for hull to accept a successor
  pubkey announcement signed by the current pubkey. Without this,
  a compromise is recovery-by-public-statement, which is fragile.
  **Effort:** medium for docs; high for the in-binary successor
  mechanism.

#### Tier 3. Polish (closes the loop but doesn't unblock claims)

- [x] **0.3.11. SBOM includes the hull binary's own SHA-256. ✅ Shipped.**
  `hl_sbom_set_binary_path()` wired into `hull sbom` + `hull agent sbom`;
  streams the binary through mbedTLS SHA-256 on first format call (cached;
  64 KiB chunks). Emitted as `binary_sha256` in JSON, `metadata.component.hashes`
  in CycloneDX, and `SPDXRef-Package-hull-binary.checksums` in SPDX. Pair
  with 0.3.9 (sign the SBOM itself, still pending).

- [x] **0.3.12. CPE strings in the SBOM. ✅ Shipped.** CycloneDX supports
  Common Platform Enumerators for automated CVE mapping.
  Downstream scanners currently have to fuzzy-match component
  names. **Fix:** add `cpe` field to the static entry table for
  the major deps (`cpe:2.3:a:lua:lua:5.4:*:*:*:*:*:*:*` etc.).
  **Effort:** small. One field per entry plus a CycloneDX
  formatter line.

- [ ] **0.3.13. Vendored static archives reproducibility check.**
  The submodule SHA pin is trusted; nobody independently verifies
  that rebuilding e.g. WAMR from `c3a78cd159e5` produces a
  byte-identical `.a`. Probably fine for most deps, but currently
  undocumented as either a goal or non-goal. **Fix:** either add
  a per-vendor `make verify-vendor-repro` target or explicitly
  document this as a non-goal. **Effort:** low for docs; medium
  for full per-dep verification.

- [x] **0.3.14. Published build-environment manifest. ✅ Partial.** Specify
  what ubuntu image, what Xcode version, what cosmocc commit, what
  wgpu-native commit was used to build each release. Some of this
  is in the SBOM now; the rest (Xcode, system libs at link time,
  CI runner image version) isn't. **Fix:** dump a `BUILD_ENV.json`
  alongside `hull.sha256` capturing the CI runner image SHA,
  toolchain versions, and link-time system library versions.
  Pairs with 0.3.1 and 0.3.5. **Effort:** small.

- [x] **0.3.15. Self-sovereignty fork playbook. ✅ Shipped.**
  `docs/fork_playbook.md` (259 lines): when to fork, what it gets
  you (and what it doesn't), a substantial "why most organisations
  shouldn't fork" framing (AGPL §13 obligations propagate, security
  posture inheritance, support burden, super-linear divergence cost,
  trust non-transitivity, three lighter-weight alternatives that
  satisfy most "we need our own trust root" requirements without a
  fork), the five-step procedure (fork source, generate keypairs,
  embed pubkeys, fork release workflow, replace install URL),
  verification checklist, and honest threat-model notes about what a
  fork does NOT solve (CI drift, vendored-dep trust, key custody
  are still §0.3.1-3 problems in any fork).

**Priority ordering for execution:**

1. Cheapest, highest immediate signal: 0.3.5 (SLSA provenance) +
   0.3.6 (SHA-pin actions). Days of work, immediately auditable,
   no design decisions. Do both before any Tier 1 item.
2. Most credibility per dollar: 0.3.4 (transparency log) + 0.3.8
   (`hull verify-self`). These close the "I have the binary" →
   "I can prove what built it without trusting gethull" loop.
3. Hardest but most important long-term: 0.3.3 (signing off
   GitHub Actions secrets) + 0.3.1 (pin CI environment). These
   are organisational/procedural changes more than code, and they
   determine whether the trust story survives serious scrutiny.

---

## 1. PostgreSQL backend (HlDbBackend)

**Priority:** Medium-High. First non-SQLite backend; validates the DB-vtable
abstraction that already powers `HL_ENABLE_DB=0` compute-only builds.

**Approach:**

- `HlDbBackend` implementation using libpq.
- Connection string via `--db postgres://…` or per-handler config.
- Statement caching via `PQprepare`.
- Async queries via libpq's async protocol (not worker threads. Avoids the
  extra hop and gives us pipelining).
- Hull internals (`_hull_*` tables) **stay on embedded SQLite** so apps can
  be ported incrementally; only application tables move to Postgres.

**Tasks:**

- [ ] Vendor or dynamic-link libpq (choose: more deps but real prod use vs.
      simpler distribution)
- [ ] `src/hull/cap/db_postgres.c`. Backend vtable impl
- [ ] Connection pooling (single connection vs. pool. Start with pool)
- [ ] Parameter binding (Hull's `?` placeholder → Postgres `$1, $2…` rewrite)
- [ ] Type mapping (HlValue ↔ Postgres OIDs)
- [ ] Migration runner compatibility (Postgres dialect for `_hull_migrations`)

**Out of scope:** transactions across SQLite + Postgres (no XA / two-phase
commit). Apps that mix both must accept eventual consistency.

---

## 1.3 Web stdlib namespace reorganization (target v0.2.0)

**Priority:** High. Foundational — `hull/web/*` is the namespace the
§1.4 future helpers (`flash`, `pagination`, `table`) are born under;
this section moves the *existing* strictly-web modules to match. Done
once, before the surface grows further.

**Motivation.** Today the stdlib registry mixes strictly-web modules
(`hull/cookie`, `hull/form`, `hull/htmx`, `hull/sse`, `hull/ws-*`) with
runtime-agnostic ones (`hull/db`, `hull/log`, `hull/crypto`, `hull/i18n`,
…) at the same flat namespace level. As the web surface grows
(§1.4 flash/pagination/table, §1.5.d styled-confirm/CSP-report, etc.)
the flat namespace becomes a soup. Moving the strictly-web modules
under `hull/web/*` mirrors the established `hull/middleware/*`
precedent and lets `hull modules available` group output meaningfully.

**Locked design decisions (2026-05-31):**

- **Namespace name: `hull/web/*`.** Broad enough to cover HTTP-protocol
  concerns (cookie, session, CSRF, form), HTML rendering (htmx, future
  flash/pagination/table), and real-time delivery (ws, sse). Matches
  the standard ecosystem term ("web framework", "web middleware").
- **Hard break in v0.2.0, no back-compat aliases.** Old names fail at
  module-load with a fix-it hint: `module 'hull/cookie@1' was renamed
  to 'hull/web/cookie@1' in v0.2.0`. Pre-1.0 conventions allow
  breaking changes in minors; soft-landing aliases would carry
  indefinitely and pollute `hull modules available` output.
- **v0.2.0 is the version bump.** Pre-1.0 minor. Tag carries
  `BREAKING:` lead and a migration table in the release notes.
- **`hull/jwt@1` stays flat.** Cross-cutting: API auth, CLI tokens,
  service-to-service. Not strictly web.
- **`hull/http-server@1` stays flat.** Foundational — the layer
  `hull/web/*` is built on top of. Convention: `hull/web/*` are
  *consumers* of http-server, not http-server itself.
- **`hull/template@1` stays flat.** The engine itself is content-type
  agnostic. Theoretically reusable for emails, configs, codegen.
- **`hull/http-client@1`, `hull/email@1`, `hull/smtp@1` stay flat.**
  Cross-cutting (CLI fetches APIs, batch jobs send mail, etc.).

**Move table:**

| Today | v0.2.0 |
|---|---|
| `hull/cookie@1` | `hull/web/cookie@1` |
| `hull/form@1` | `hull/web/form@1` |
| `hull/htmx@1` | `hull/web/htmx@1` |
| `hull/sse@1` | `hull/web/sse@1` |
| `hull/ws-server@1` | `hull/web/ws-server@1` |
| `hull/ws-client@1` | `hull/web/ws-client@1` |
| `hull/middleware/auth@1` | `hull/web/middleware/auth@1` |
| `hull/middleware/cors@1` | `hull/web/middleware/cors@1` |
| `hull/middleware/csp@1` | `hull/web/middleware/csp@1` |
| `hull/middleware/csrf@1` | `hull/web/middleware/csrf@1` |
| `hull/middleware/etag@1` | `hull/web/middleware/etag@1` |
| `hull/middleware/health@1` | `hull/web/middleware/health@1` |
| `hull/middleware/idempotency@1` | `hull/web/middleware/idempotency@1` |
| `hull/middleware/inbox@1` | `hull/web/middleware/inbox@1` |
| `hull/middleware/logger@1` | `hull/web/middleware/logger@1` |
| `hull/middleware/outbox@1` | `hull/web/middleware/outbox@1` |
| `hull/middleware/ratelimit@1` | `hull/web/middleware/ratelimit@1` |
| `hull/middleware/rbac@1` | `hull/web/middleware/rbac@1` |
| `hull/middleware/session@1` | `hull/web/middleware/session@1` |
| `hull/middleware/transaction@1` | `hull/web/middleware/transaction@1` |

(Unchanged: `hull/jwt`, `hull/http-server`, `hull/http-client`,
`hull/template`, `hull/email`, `hull/smtp`, `hull/app`, `hull/db`,
`hull/log`, `hull/json`, `hull/crypto`, `hull/time`, `hull/env`,
`hull/fs`, `hull/i18n`, `hull/validate`, `hull/csv`, `hull/search`,
`hull/image`, `hull/compute`, `hull/gpu`, `hull/timers`, `hull/worker`,
`hull/tui`, `hull/inspect`.)

**Coupling with §1.4 and §1.5.c.** The flash/pagination/table helpers
are already designed for `hull/web/*`. To avoid a two-step migration
for users (adopt `hull/web/flash@1` in v0.1.10, then re-migrate
existing modules in v0.2.0), §1.4 and §1.5.c are likely best
bundled into v0.2.0 itself — one coherent "web stdlib reorganization
+ companion expansion" release. v0.1.9 (multipart) ships under the
current names; v0.2.0 ships the rename and the new helpers together.

**Tasks (v0.2.0):**

- [ ] §1.3-1. Module registry: rename the 20 affected entries in
      `src/hull/module_registry.c`. Resort the table by canonical name
      (`hull/middleware/*` rows disappear, `hull/web/middleware/*`
      rows take their place at the new sort position; `hull/cookie`
      rows likewise). Update inline comments noting the rename.
- [ ] §1.3-2. Resolver fix-it hint. When an app declares one of the
      20 old names, emit `module 'hull/cookie@1' was renamed to
      'hull/web/cookie@1' in v0.2.0; update app.manifest.modules` and
      fail fast. Test fixture for each renamed module.
- [ ] §1.3-3. Stdlib cross-references. Every stdlib module that
      `require`s / `import`s a renamed module updates to the new path.
      Run `grep -rn 'require("hull.cookie")' stdlib/lua/` (and the
      JS / middleware equivalents) to find them all.
- [ ] §1.3-4. Examples sweep. `examples/auth`, `examples/todo`,
      `examples/hypermedia_todo`, `examples/jwt_api`, and any other
      example that uses a renamed module updates its
      `app.manifest.modules` + require/import sites + tests.
- [ ] §1.3-5. Scaffold sweep. `stdlib/cli/lua/hull/init.lua` (every
      profile including `--profile htmx`) emits the new names.
- [ ] §1.3-6. Docs sweep. CLAUDE.md, README.md, docs/htmx.md,
      docs/agent_guide.md, docs/security.md, every
      `stdlib/context/*.md` topic doc that references a renamed
      module. Single PR; sed-able for most cases but tables need
      hand-review.
- [ ] §1.3-7. Completion sweep. Update `hull modules available --tui`
      grouping (group `hull/web/*` and `hull/web/middleware/*` together
      in the picker) and the bash/zsh/fish completion tables that list
      modules.
- [ ] §1.3-8. `hull modules available` output format. Group rows by
      first segment (`hull/`, `hull/web/`, `hull/web/middleware/`,
      etc.) with section headers. Improves discoverability of the
      reorganized surface.
- [ ] §1.3-9. Release notes for v0.2.0. Lead with `BREAKING:` and the
      full move table. Migration recipe: a one-liner sed for `app.lua`
      / `app.js` plus the manifest update.

**Out of scope:** automated codemod tool. Hand-edit + sed is enough
for the surface area; a one-shot subcommand would take longer to
write than the migration takes for any real app.

---

## 1.4 General web stdlib companions (runtime-agnostic)

**Priority:** Medium. A handful of small modules that any web app
needs regardless of frontend strategy (REST API, server-rendered,
HTMX, hybrid). Grouped here so they don't get pulled into
HTMX-specific batches and so non-HTMX apps can still find them. The
HTMX scaffold (§1.5) wires them in opportunistically.

**Locked design principles:**

- **General-purpose, not HTMX-coupled.** HTMX-specific emission paths
  exist as additional optional methods on each module, never as the
  module's reason to exist.
- **One stdlib module per concern.** Lua + JS parity, manifest
  declaration, unit tests. Cross-referenced from §1.5 where the
  HTMX scaffold should wire them in.
- **Namespace: `hull/web/*`.** Matches the existing `hull/middleware/*`
  precedent — a sub-namespace for "convenience helpers a web app
  needs on top of the foundational primitives." Foundational
  web modules that predate this convention (`hull/template@1`,
  `hull/cookie@1`, `hull/form@1`, `hull/jwt@1`, etc.) keep their
  flat names; the `web/` namespace is for new layered helpers
  going forward.

**Tasks (target v0.1.10):**

- [ ] §1.4-1. `hull/web/flash@1` (Lua + JS). One-shot user notifications.
      The classic POST/redirect/GET helper, applicable to any web
      app. `flash.set(req, text, kind?)` stashes in session;
      `flash.consume(req)` drains for the next render. HTMX bonus
      path: `flash.trigger(res, text, kind?)` emits
      `HX-Trigger: {"flash":{...}}` for fragment-swap paths that
      bypass the redirect. Template partial `partials/_flash.html`
      for default rendering. Unit tests for both emission paths.
      Wire into `examples/hypermedia_todo` (POST handlers) and
      optionally `examples/auth` (login success).
- [ ] §1.4-2. `hull/web/pagination@1` (Lua + JS). Server-side helper
      for `?page=N&per_page=M` paginated lists. Useful for REST
      JSON endpoints, server-rendered pages, and HTMX fragment
      swaps alike. `pagination.from_query(req, opts)` returns
      `{page, per_page, offset, limit}`; `pagination.render(total,
      opts)` returns `{total, pages, page, prev, next, links[]}`
      suitable for templates or JSON serialization. Partial
      `partials/_pagination.html` for HTML rendering. Scoped to
      offset-based pagination; cursor-based deferred. Demo:
      paginated todo list in `examples/hypermedia_todo`.

**Future candidates (no target):**

- `hull/web/table@1` — server-rendered sortable/filterable list
  helper. Generic core: parse `?sort=col&dir=asc&filter[k]=v` from
  the query; validate sort/filter columns against an allowlist
  (prevents enumeration + SQL injection); build `ORDER BY` / `WHERE`
  fragments; render a plain HTML `<table>` partial with
  `<th><a href="?sort=...">Header</a></th>` headers that work
  without JS. HTMX opt-in: `{ htmx = { target = "#table-body" } }`
  swaps the anchor `href` for `hx-get` + `hx-target` so header clicks
  do fragment swaps. Same generic-with-HTMX-opt-in shape as
  `hull/web/flash@1`. Pairs with `hull/web/pagination@1` for the
  canonical CRUD list view.
- `hull/web/seo@1` *(cloud-deployed apps only)* — Open Graph /
  Twitter Card / JSON-LD helpers. Template-friendly: `seo.tags({
  title, description, image, type, card = "summary_large_image" })`
  returns the full `<meta>` set for the page `<head>`.
  `seo.json_ld(structured_data)` renders the
  `<script type="application/ld+json">` block. Pairs with the
  canonical-URL middleware (below). Out of scope for Hull's
  local-first majority (internal tools, CLI utilities, auth-walled
  admin apps don't need any of this); relevant only for the
  public-facing cloud-deployed slice.
- `hull/web/openapi@1` — Spec generator for the JSON half of
  HTMX-hybrid apps (HTMX routes returning HTML coexist with
  `/api/*` routes returning JSON). Walks `app.get/post/...`
  registrations, infers params from `:path/:vars`, accepts
  `app.get("/api/x", { schema = {...} }, handler)` overrides
  for explicit request/response shapes. Emits OpenAPI 3.1 JSON +
  serves Swagger UI at `/docs`.
- Request ID propagation helpers.
- Canonical-URL / trailing-slash redirect middleware.
- Content negotiation helper.
- `robots.txt` / `sitemap.xml` generator.

Add here when surfaced by a real app build-out.

---

## 1.5 Hypermedia web application profile (HTMX + Pico)

**Priority:** High. Fills the gap between full-page SSR apps and
React-style SPAs for business workflow software. Target use case: internal
tools such as IT asset trackers, admin consoles, inventory systems, approval
workflows, CRM-like dashboards, and other CRUD-heavy applications where the
server should remain the source of truth.

**Thesis.** Hull already has the right backend primitives for secure SSR:
templates, forms, sessions, CSRF, RBAC, SQLite, migrations, static files,
search, CSV, email, audit logging, and single-binary deployment. HTMX adds
partial page updates while preserving HTML as the application protocol.
Pico.css provides a classless baseline that rewards semantic templates and
avoids a frontend build pipeline.

**Split into three sub-items.** §1.5.a is the HTMX core (target v0.1.8).
§1.5.b is multipart uploads + attachment storage (target v0.1.9). §1.5.c
collects the broader enterprise-internal-app gaps that the profile makes
visible but are independent of HTMX itself (OIDC, app audit log, admin UI
conventions, import/export workflows).

**The desired application profile is:**

```
templates/
  base.html
  pages/
  fragments/
static/
  vendor/htmx.min.js
  vendor/pico.classless.min.css
  app.css
```

No CDN by default. Assets are vendored into `static/`, embedded by
`hull build`, served from `/static/*`, and covered by a self-hosted CSP.

**Out of scope (for the entire §1.5).** Adopting React/Vue/Svelte,
client-side hydration, npm as a required app build step, or making HTMX a
runtime dependency of Hull itself. HTMX and Pico remain vendored static
assets at the application layer; Hull provides the server-side conventions
and helpers.

### 1.5.a HTMX core (target v0.1.8)

**Locked design decisions (2026-05-30):**

- **CSS framework:** Pico v2 classless. SHA-pinned vendored copy under
  `vendor/pico/`. Scaffold copies into `static/vendor/pico.classless.min.css`.
- **CSP profile:** `style-src 'self' 'nonce-{rand}'` + `style-src-attr
  'unsafe-inline'` + `script-src 'self' 'nonce-{rand}'` + `default-src
  'self'` + `img-src 'self' data:` + `form-action 'self'` + `frame-ancestors
  'none'` + `base-uri 'self'`. Per-request 128-bit nonce. `style-src-attr
  'unsafe-inline'` exists only because Pico uses inline `style="…"`
  attributes for some components (`<details>`, `<dialog>`, form controls).
  HTML `<style>` blocks and `<script>` tags still require nonce.
- **Runtime parity:** Both Lua and JS. Helper module, middleware, and
  example app all exist in both runtimes.
- **Example app:** New minimal `examples/hypermedia_todo` (Lua + JS). Not
  reusing existing `examples/todo` to keep the HTMX patterns visible and
  uncluttered.
- **Scaffold trigger:** `hull init --profile htmx`. Extends existing
  `init.lua`; no new top-level command.

**Tasks (v0.1.8):**

- [ ] §1.5.a-1. `hull/htmx@1` helper module (Lua + JS). API: `is(req)`,
      `boosted(req)`, `redirect(req, res, path)`, `retarget(res, selector)`,
      `reswap(res, mode)`, `trigger(res, event, payload)`, plus the
      obvious additions `location(res, opts)`, `push_url(res, url)`,
      `refresh(res)`. Module registry entry. Unit tests.
- [ ] §1.5.a-2. `hull/middleware/csp@1` (Lua + JS). `csp.htmx()` factory
      returns middleware that generates a per-request nonce, exposes via
      `req.ctx.csp_nonce`, sets the full CSP header. `csp.strict()` variant
      (no `style-src-attr 'unsafe-inline'`) for non-Pico apps. Tests.
- [ ] §1.5.a-3. Test helper extension. Surface `HX-Request` and friends
      via Lua/JS `t.request(...)` headers option. The cap/test.c C layer
      already supports custom headers; just needs the binding.
- [ ] §1.5.a-4. Vendor HTMX 2.x + Pico v2 classless. `make fetch-htmx`
      and `make fetch-pico` targets like `fetch-ca-bundle`, with SHA-256
      verification.
- [ ] §1.5.a-5. **HTMX scaffolding as a profile, composable with every
      existing layout.** Today the scaffold surface is two orthogonal
      axes that don't yet talk to each other: `--type {flat,rest,cli,
      tui}` picks **layout**, and (planned) `--profile htmx` picks the
      **content shape** (HTMX vs JSON-API handlers, manifest cluster,
      vendored assets, layout-template skeleton). Ship the profile axis
      so it composes with every layout instead of forking a new
      `--type` per content shape:
        - `--profile htmx`: HTMX manifest (`hull/web/htmx@1`,
          `hull/template@1`, `hull/web/middleware/csrf@1`,
          `hull/web/middleware/session@1`, `hull/web/middleware/csp@1`,
          `hull/web/flash@1`, `hull/web/pagination@1`,
          `hull/web/cookie@1`, `hull/web/form@1`); vendored
          `static/vendor/htmx.min.js` + Pico (SHA-pinned via the
          existing `make fetch-htmx` / `make fetch-pico` targets);
          `templates/layout.html` with `<body hx-boost="true">`, CSRF
          meta, flash slot, `htmx:configRequest` → `X-CSRF-Token`
          wiring; sample resource (or `app.lua` index handler in flat
          layouts) that dispatches `if htmx.is(req) then res:html(
          partial) else res:html(full_page) end`; matching tests that
          assert both fragment and full-page paths.
        - `--profile json`: today's `--type rest` content shape,
          extracted so it composes with `--type flat` too. Becomes the
          default for `--type rest` for back-compat.
        - `--profile empty`: bare manifest + bare `hello world` handler;
          for users who want to compose their own stack.
      Composition matrix once shipped:
        - `hull new --type flat --profile htmx <name>` (replaces what
          this item was originally specced as — `hull init --profile
          htmx`; `hull init` retains the same form for init-in-place)
        - `hull new --type rest --profile htmx <name>` (the modular
          HTMX app pattern — what the Trimble HU asset-inventory
          project needs, and what surfaced this design)
        - `hull new --type rest --profile json <name>` (today's
          `--type rest` default)
        - `--type cli` and `--type tui` ignore `--profile` (no web
          surface)
      Implementation: extract the manifest cluster + vendored assets +
      layout template + handler-pattern snippets into a shared
      `stdlib/cli/lua/hull/profiles_htmx.lua` consumed by
      `templates_flat.lua` and `templates_rest.lua`; add `--profile`
      parsing to `stdlib/cli/lua/hull/new.lua` and to the existing
      `hull init` flow; doc update in `docs/agent_guide.md`.
      **Surfaced 2026-06 by the Trimble HU asset-inventory project**
      (`asset_inventory_assessment.md` §e.2) — that app is a 50+ route
      HTMX application and bootstrapping it cleanly today requires
      either accepting the JSON-scaffold mismatch or hand-laying the
      whole structure. The orthogonal-axis design avoids forking
      `--type hypermedia` (and `--type htmx-cli`, `--type htmx-flat`,
      …) just to combine layout × content shape.
- [ ] §1.5.a-6. `examples/hypermedia_todo` (Lua + JS). Demonstrates the
      pattern: full-page render on plain GET, fragment render on
      `HX-Request`, optimistic row replacement, validation errors as a
      fragment, flash messages (documented OOB pattern; proper module
      helper lands in §1.4-1), CSRF on htmx requests. Tests for both
      htmx and plain-form paths.
- [ ] §1.5.a-7. `docs/htmx.md` pattern guide. Architectural pattern,
      response-header helpers, CSP nonce integration, CSRF on htmx
      requests, validation errors as fragments, flash messages
      (documented `hx-swap-oob` recipe; promoted to a stdlib module
      in §1.4-1), empty states, testing patterns.

### 1.5.b Streaming multipart + attachment storage (target v0.1.9)

**Locked design decisions (2026-05-31):**

- **Keel multipart parser is in scope.** Bumps Hull's Keel pin when the
  parser lands. Per-part disposition + size + MIME parsed; body streams
  via an iterator (no full-body buffering).
- **Runtime parity:** Lua + JS for `hull/attachment` and the multipart
  bindings.
- **Photo upload demo lands in `examples/hypermedia_todo`** rather than a
  new example. v0.1.8 ships the example without uploads; v0.1.9 adds them.
- **Multipart API shape: iterator.** Most Hull-idiomatic, matches how
  `db.query` streams rows.
  - Lua: `for part in req:multipart() do ... end` where each `part` has
    `name`, `filename`, `content_type`, `headers`, and `part:chunks(n)`
    iterator for streaming bytes.
  - JS: `for await (const part of req.multipart()) { ... }` where each
    `part` has `name`, `filename`, `contentType`, `headers`, and an
    async `part.chunks(n)` iterator.
  - Per-part cap, total-body cap, and max-parts count enforced by the
    parser; exceeded caps abort with 413.
- **MIME validation: header + sniff (defense-in-depth).** Reject by
  client `Content-Type` against the manifest allowlist on the fast
  path; then content-sniff the first 4 KiB on accepted parts and
  record the sniffed MIME in metadata. If sniffed MIME disagrees
  with the header on a stored part, the metadata row's `mime` is the
  sniffed value (truth-by-bytes); `declared_mime` retains the header
  for audit. Sniffer covers PNG/JPEG/GIF/WebP/PDF/PNG/SVG/plain-text
  via magic bytes; CSV / JSON fall through to header.
- **Storage layout + manifest contract: piggyback on `fs.write` +
  module init.** Manifest declares `fs = { write = {"data/attachments"} }`;
  app calls `attachment.init({dir, max_size, max_total, mime_allowlist,
  sniff = true})` explicitly. Storage layout:
  `<dir>/<sha256[0:2]>/<sha256>` (content-addressed; dedup is free).
  Metadata in SQLite table `_hull_attachments` (id, sha256,
  original_name, mime, declared_mime, size, uploaded_by, uploaded_at,
  refcount).
- **GC strategy: refcount-based, opportunistic + scheduled.**
  Application code calls `attachment.delete(id)` which decrements the
  refcount; when refcount hits zero, the row is marked `pending_gc`.
  An `attachment.cleanup()` helper (called from `app.daily(...)` in
  the scaffold) sweeps `pending_gc` rows older than 24h and removes
  the on-disk content. Background scan to detect orphaned files
  (on disk but not in DB) is a separate `attachment.scrub()` for
  ops use, NOT scheduled by default.

**Tasks (v0.1.9):**

- [x] §1.5.b-1. Streaming multipart parser in Keel (v2.0.0 / v2.1.0 /
      v2.1.1 / v2.1.2 / v2.2.0). Per-part content-disposition, filename,
      content-type, size. Iterator-shaped API via
      `kl_body_reader_multipart` + `kl_multipart_next`. Per-part cap,
      total-body cap, max-parts count, max-headers-size, max-input-
      buffer enforced. Streaming-handler dispatch (v2.1.0) + mid-stream
      early-exit success/error paths (v2.1.1/v2.1.2) + streaming-async
      pre-body dispatch (v2.2.0) so every cap surfaces as a structured
      4xx in both single-read and multi-read scenarios. Fuzz + ASan
      coverage in upstream CI.
- [x] §1.5.b-2. Hull-side iterator bindings for Lua + JS.
      `req:multipart()` (Lua iterator) and `req.multipart()` (JS async
      iterator). Each part exposes `part:chunks(n)` /  `part.chunks(n)`
      for streaming reads, plus `part:read()` / `part.read()` for whole-
      part buffering. Binary-safe (Lua byte strings, JS `ArrayBuffer`).
      Incremental SHA-256 hasher (`crypto.create_sha256()` Lua /
      `crypto.createSha256()` JS) for streaming digest. Coverage:
      94 e2e cases in `tests/e2e_multipart.sh` across both runtimes —
      tiny field, 10 fields, small bin, 2 MB bin, 5 MB bin, mixed,
      empty file, 10x keep-alive, max-part-size, skip/auto-drain,
      max_parts cap, max_headers_size cap, max_total_size cap (single-
      read), sync-completion keep-alive force-off, hasher edge cases.
      See `docs/multipart.md`.
- [x] §1.5.b-3. Content-MIME sniffer in `src/hull/cap/mime.c`. Magic-
      byte table for PNG/JPEG/GIF/WebP/PDF/SVG/HTML (HTML added beyond
      the original scope — same XSS surface as SVG, useful when users
      upload reports). UTF-8 plain-text fallback (rejects NUL bytes,
      overlong encodings, UTF-16 surrogates, code points > U+10FFFF).
      JSON / CSV correctly fall through to text/plain (valid UTF-8 text
      by definition) — callers needing finer discrimination use the
      declared Content-Type header. Header `hl_cap_mime_sniff(buf,
      len)` returns canonical MIME or NULL; statically allocated, do
      not free. 45 unit tests: 31 embedded-byte cases covering magic
      detection, truncation, case-insensitive shape matching, UTF-8
      edge cases (overlong, surrogate, NUL), plus 7 fixture-file smoke
      tests against real PNG/JPEG/GIF/WebP/PDF/SVG/HTML files in
      `tests/fixtures/mime/`.
- [x] §1.5.b-3.5. `hull/blob@1` module (Lua + JS). Pure content-
      addressed disk storage. Bytes in → SHA-256-keyed ID; bytes out by
      ID. Streaming put with **on-the-fly SHA-256** (hashed in lockstep
      with the temp-file write — never buffered just to hash). Atomic
      writes via temp + `rename(2)`. Sharded layout (`<dir>/<hash[0:2]>/
      <hash>`); 1-level default, 2-level opt-in. Self-verifying
      (filename IS the SHA). **Zero SQLite dependency** — compiles and
      runs cleanly under `HL_ENABLE_DB=0`, important because the
      compute AOT cache and Lua bytecode cache are exactly where
      compute-only builds need it most. API: `blob.init({dir,
      shard_depth, tmp_max_age})`, `blob.put(bytes) -> id, size`,
      `blob.put_verified(bytes, expected_id)`, `blob.writer()` →
      streaming writer with `:write()` / `:finalize()` / `:abort()`,
      `blob.get(id, { track_access = false }?)`, `blob.reader(id)`,
      `blob.exists/size/atime/delete`, `blob.iter` for ops scans,
      `blob.cleanup({max_total_size, max_age, strategy = "lru" |
      "fifo", dry_run})` for opt-in eviction. Cap-layer in
      `src/hull/cap/blob.c` reuses incremental SHA from §1.5.b-2 and
      routes all I/O through `hl_cap_fs_*`. **Six known consumers**
      (drives the design): (1) `hull/web/attachment@1` (§1.5.b-4
      below), (2) `hull tools install` migration (JSON sidecar map),
      (3) compute AOT cache migration (JSON sidecar map, system-wide
      `~/.hull/cache/compute/`), (4) Lua bytecode cache (sidecar-less
      — key IS the SHA), (5) LLM artifact cache (v0.1.11+), (6)
      template-AST cache (v0.1.10+). Full design + migration map in
      [docs/blob.md](blob.md). Tests: C-layer (put/get/streaming
      hash correctness/atomic rename/EXDEV fallback/concurrent put/iter/
      cleanup), Lua + JS binding integration, e2e against live `hull
      dev`. Migrations 1-2 land in v0.1.10 (after blob ships +
      battle-tested by attachment in v0.1.9); migrations 3-6 follow.
- [ ] §1.5.b-4. `hull/web/attachment@1` module (Lua + JS), built on
      `hull/blob@1`. Lives under `hull/web/*` per the v0.2.0
      reorganization — its primary input is a multipart `Part`, its
      primary output is the auth-gated `attachment.serve(req, res, id,
      { auth_check = fn })` route helper, and its `_hull_attachments`
      table mirrors the shape of `_hull_sessions` /
      `_hull_idempotency_keys` already living under
      `hull/web/middleware/*`. Thin layer: `_hull_attachments`
      metadata (id, blob_id, original_name, mime, declared_mime, size,
      uploaded_by, uploaded_at, refcount) + MIME validation via
      `hl_cap_mime_sniff` on the first chunk + auth-gated `serve()`
      helper, with the actual disk I/O delegated to `blob.writer()` /
      `blob.reader()` / `blob.delete()`. attachment owns the refcount;
      blob doesn't know one exists. `attachment.delete(id)` decrements;
      at refcount=0 sets `pending_gc=true`; `attachment.cleanup()`
      (called from `app.daily`) sweeps `pending_gc` rows older than
      24h and calls `blob.delete()`. API: `attachment.init(opts)`,
      `attachment.store(part, opts) -> id`,
      `attachment.read(id) -> stream`, `attachment.read_to_file(id,
      dst)`, `attachment.metadata(id)`, `attachment.delete(id)`,
      `attachment.cleanup()`, `attachment.scrub()` (ops-only),
      `attachment.serve(req, res, id, { auth_check = fn })`. Tests for
      happy path, MIME-mismatch, cap-exceeded, refcount, dedup, GC.
      SQLite-bound here (consistent with other `hull/web/*` modules
      that store state); blob underneath stays SQLite-free.
- [ ] §1.5.b-5. Photo upload demo in `examples/hypermedia_todo` (Lua +
      JS). File-input upload (drag-and-drop deferred to §1.5.e), per-todo
      attachment listing, delete with confirm. Tests covering upload +
      list + delete cycle.
- [ ] §1.5.b-6. Docs: extend `docs/htmx.md` with the upload pattern
      (form encoding, progress events via HTMX, validation feedback for
      rejected MIMEs). New `docs/attachments.md` covering API surface,
      MIME-validation (header + sniff), storage layout, refcount GC,
      `attachment.scrub()` for ops, manifest declaration.

### 1.5.b-X. hull/blob@1 migrations (target v0.1.10)

Six known consumers identified during the §1.5.b-3.5 design. All
backed by `hull/blob@1`. Four are runtime-infrastructure caches
that live OUTSIDE `app.manifest.fs` (cache is Hull's decision to
accelerate the app, not part of the app's I/O surface — see
[docs/blob.md §Runtime-infrastructure caches](blob.md) for the full
manifest-line rationale). One is a build-tool migration; one is a
new external user-facing feature.

**Disclosure that DOES exist** even though these aren't in
`app.manifest`:

- `hull doctor` reports cache locations and their sizes (similar to
  how it reports the CA bundle and tools status today).
- `hull inspect` surfaces "this binary uses caches at:
  `~/.hull/cache/...`" — informational, not a permission.
- Opt-out env vars: `HULL_NO_CACHE=1` disables all runtime caches;
  `HULL_NO_LUA_BYTECODE_CACHE=1`, `HULL_NO_AOT_CACHE=1`,
  `HULL_NO_TEMPLATE_CACHE=1` for granular control. Always-honored;
  disabling forces re-derive on every load.
- New `hull cache list|prune|clear` subcommand surfaces what's
  actually stored, evicts old entries, or wipes everything.

Tasks (target v0.1.10):

- [x] §1.5.b-X-0. Cap/store split. **Landed (prerequisite).** The
      low-level CAS implementation moved from `src/hull/cap/blob.c`
      to a new `src/hull/blob_store.c` (~600 LOC) exposing
      `hl_blob_store_*`. The cap layer (`cap/blob.c`) shrunk to a
      thin wrapper that adds manifest fs.write validation at init
      and forwards everything else to the store. `HlBlob` is now a
      typedef alias for `HlBlobStore` so existing callers compile
      unchanged.
      A new `hl_blob_store_put_keyed` variant lets runtime caches
      use the same atomic-rename + sharded layout in keyed-value
      mode (filename is caller-supplied, not content-derived) —
      the bytecode and AOT caches are key-value, not CAS, so this
      bridges the abstraction gap. Per-store discipline: each
      store is exclusively CAS *or* keyed; apps' blob stores stay
      CAS, runtime caches use keyed.
      The runtime caches share the layout but partition under
      `$HOME/.hull/blobs/runtime/<kind>/`, while app blobs live
      wherever the app's manifest declares them (typically inside
      the app dir). Single tree to walk for the future
      `hull cache list` command.
      Verified: 42/42 unit + 23/23 blob unit + 36/36 e2e_blob +
      12/12 e2e_aot_cache + 118/118 e2e build + 18/18 e2e sandbox
      + 280/280 e2e examples — all behave identically to before.

- [x] §1.5.b-X-1. Lua bytecode cache (runtime-infrastructure,
      sidecar-less). **Landed.** Cache key = `sha256(LUA_VERSION ||
      arch || endian || source)`. `hl_lua_load_cached()` is a
      drop-in replacement for `luaL_loadbuffer` wired into
      `mod_fs.c::hl_lua_register_stdlib` for both stdlib and app
      module loading. Stores raw `lua_dump`'d bytecode (strip=0 to
      preserve `ar.source` for the `_hull_*` namespace gate).
      Sandbox auto-allows the cache root on both seatbelt (macOS)
      and pledge/unveil (Linux/Cosmo). Honors `HULL_NO_CACHE=1` and
      `HULL_NO_LUA_BYTECODE_CACHE=1`. Falls back transparently to
      `luaL_loadbuffer` on any cache I/O failure. 5 unit tests in
      `test_lua` (`lua_bytecode_cache.*`).
      **Routed through `hl_blob_store_*` as of the cap/store
      split:** the on-disk layout is now
      `$HOME/.hull/blobs/runtime/lua-bytecode/blobs/<XX>/<sha256>`
      (sharded), shared infrastructure with `hull/blob@1` and the
      AOT cache.
- [x] §1.5.b-X-2. Compute AOT cache migration. **Landed.**
      Memoizes `wamrc` output at
      `$HOME/.hull/blobs/runtime/compute-aot/blobs/<XX>/<sha256>`
      (sharded; same blob_store layout as the bytecode cache and
      `hull/blob@1`). Key folds in `sha256(wasm_bytes) || arch ||
      memory64_flag || wamrc_version`, so each unique tuple
      compiles at most once per machine. Sidecar-less — the
      filename IS the key. Cross-app dedup falls out naturally
      (two apps using the same `score.wasm` for the same arch
      share one cache entry).
      No JSON sidecar — the design we sketched is replaced by a
      sidecar-less CAS layout (filename = key). Cleaner: no index
      file to corrupt, atomic via tmp+rename, concurrent build
      processes are benign because identical inputs produce
      identical bytes.
      New CLI module `stdlib/cli/lua/hull/aot_cache.lua` provides
      the cache surface (`key`, `lookup`, `store`,
      `wamrc_version`). Wired into the AOT loop in
      `stdlib/cli/lua/hull/build.lua`; build summary now reports
      hits + writes (`8 AOT module(s) compiled (8 from cache)`).
      New `tool.*` bindings landed alongside:
        - `tool.hull_cache_dir([kind])` — resolve cache subdir
        - `tool.hull_cache_disabled([kind])` — env-opt-out probe
        - `tool.rename(src, dst)` — POSIX-atomic rename (for CAS)
        - `tool.remove_file(path)` — gated unlink (tmp cleanup)
      Sandbox auto-allows the cache root in tool mode too (`hull
      build` runs under a tighter sandbox than apps).
      Honors `HULL_NO_AOT_CACHE=1`, `HULL_NO_CACHE=1`, and the new
      `hull build --no-cache` flag.
      Verification: examples/compute (8 modules) on M-series:
      cold build ~1.0s, warm build ~0.06s — 16× speedup. Warm-
      cache binary boots and serves requests with `aot=1` log
      confirming AOT path active. 12/12 cases in new
      `tests/e2e_aot_cache.sh` (cold/warm/HULL_NO_AOT_CACHE/
      HULL_NO_CACHE/--no-cache/cross-invocation reuse/binary
      still runs).
- [x] §1.5.b-X-3. `hull tools install` migration. **Landed.**
      Downloaded tool binaries land at
      `$HOME/.hull/blobs/tools/blobs/<XX>/<sha>` via
      `hl_blob_store_put_durable(s, body, len, expected_sha, ...)`
      — CAS mode, since we already know the SHA from the signed
      manifest. The blob_store call replaces the previous
      `hl_release_io_sha256_hex` + `mbedtls_ct_memcmp` + atomic
      write block: blob_store does the SHA verification AND the
      put_verified short-circuit (re-install of identical bytes
      is a stat-only fast path, no rewrite).
      The user-visible `$HOME/.hull/tools/<name>` becomes a
      symlink to the blob, atomically (re)placed via
      symlink-tmp + rename. Preserves the existing PATH-style
      lookup (`hl_tools_lookup_path` already uses
      `access(X_OK)` which follows symlinks transparently —
      cap/wasm.c's wamrc resolver and `build.lua`'s
      `tool.find_tool` need no changes).
      Dropped the JSON sidecar from the original sketch — not
      needed for a single-active-version-per-tool model. If a
      version-history feature is ever added, the index can land
      then; today's design is simpler and `hull cache list`
      will still see all tool blobs by walking
      `~/.hull/blobs/tools/blobs/`.
      Tool blobs partition into `blobs/tools/` (sibling of
      `blobs/runtime/`) so they're NOT swept by future
      `hull cache prune` runs — tools are durable signed
      downloads, not re-derivable caches.
      Verified: 23/23 test_tools_install (added
      `lookup_follows_symlink_to_blob`), 42/42 unit suites,
      64/64 e2e_tools (the one e2e failure on this host was a
      pre-existing test-isolation bug exposed by `build/wamrc`
      existing next to the hull binary), 12/12 e2e_aot_cache,
      36/36 e2e_blob, 12/12 e2e_update. Live install simulated
      against real `wamrc` bytes: first install writes blob +
      creates symlink + exec works through symlink; second
      install hits short-circuit (no rewrite); `hull doctor`
      reports the tool as managed.
- [x] §1.5.b-X-4. Template AST cache. **Landed.**
      `_template._compile` now routes through
      `hl_lua_template_compile_cached`. Cache key =
      `sha256(LUA_VERSION || arch || endian || generated_code)`.
      Cached value = `lua_dump()` of the inner render function
      (post-pcall) — on a hit we skip BOTH the parse pass and the
      pcall to unwrap the outer chunk.
      The in-process function cache (Lua-side, keyed by template
      name) stays in front for the warm path; the blob cache is
      the cold-start / reload-survival layer.
      Key insight: hashing the *generated* code (post-resolve of
      extends + includes) gives automatic invalidation when any
      referenced template changes. No dependency tracking
      needed — a different generated code is a different key.
      Store: `$HOME/.hull/blobs/runtime/templates/blobs/<XX>/<sha>`
      via the same `hl_blob_store_put_keyed` primitive that backs
      the bytecode + AOT caches.
      Honors `HULL_NO_CACHE=1` and `HULL_NO_TEMPLATE_CACHE=1`.
      Falls back to a fresh compile on any cache I/O failure.
      Files: `include/hull/runtime/lua_template_cache.h`,
      `src/hull/runtime/lua/template_cache.c`,
      `src/hull/runtime/lua/mod_template.c` (one-line swap from
      `luaL_loadbuffer + pcall` to `hl_lua_template_compile_cached`),
      4 new utests in `test_lua`
      (`lua_template_cache.miss_then_hit_populates_disk`,
      `opt_out_via_env_skips_disk`,
      `generated_code_change_invalidates`,
      `parse_error_returns_no_cache_write`).
      Verified: 42/42 unit, 40/40 e2e_templates, 36/36 e2e_blob,
      12/12 e2e_aot_cache. End-to-end smoke against a real
      template: first boot creates blob, second boot hits it,
      output is byte-identical.
- [x] §1.5.b-X-5. `hull cache list|prune|clear` subcommand
      **+ per-app cache isolation via `HULL_CACHE_DIR`. Landed.**
      Registry-driven (`include/hull/cache_registry.h`) — single
      source of truth for cache kinds, used by `cache list`, the
      planned `doctor` cache section (X-6), the planned `inspect`
      disclosure (X-7), and any future consumer.
      Commands:
        - `cache list [--json]` — one line per registered kind with
          path, entry count, total size, runtime/system flag.
        - `cache prune [--kind=K] [--max-size=N] [--max-age=N]
          [--strategy=lru|fifo] [--dry-run]` — runs
          `hl_blob_store_cleanup` over runtime caches (system stores
          like `tools` skipped by default; explicit `--kind=tools`
          still works).
        - `cache clear [--kind=K] --yes` — iter + delete every entry
          (not policy-based; works even on sub-second-old files).
      `--yes` required to prevent accidental wipe.
      **Per-app isolation: `HULL_CACHE_DIR=/abs/path`** overrides
      the default `$HOME/.hull/blobs/runtime/` for the entire
      runtime cache pool. For multi-tenant boxes, systemd / k8s /
      Docker deployments where each app must have its own cache
      and not see / be seen by another deployment. Must be
      absolute (relative paths rejected). Tools storage
      (`$HOME/.hull/blobs/tools/`) is intentionally NOT redirected
      — those are signed durable downloads with a stable system
      home. `HULL_NO_CACHE=1` etc. opt-outs still apply.
      Designed so optional layer C (automatic per-app isolation
      derived from app identity) can be added later without
      restructuring: it'd just compose under the same
      `hl_hull_cache_dir()` resolver — the override stays as the
      explicit / deployment-controlled mode and layer C becomes
      the convenience default for paranoid deployments.
      Files: `include/hull/cache_registry.h`,
      `src/hull/cache_registry.c`,
      `include/hull/commands/cache.h`,
      `src/hull/commands/cache.c`,
      sandbox auto-allow + cache_dir.c teach `HULL_CACHE_DIR`,
      dispatch.c + help.c register `cache`,
      `tests/e2e_cache.sh` (24 cases incl. isolation),
      `docs/blob.md` (per-app isolation section),
      `CLAUDE.md` (HULL_CACHE_DIR section).
- [x] §1.5.b-X-6. `hull doctor` cache reporting. **Landed.**
      `hull doctor` now has a Caches section between Module
      subsystems and the build-ready summary. One row per
      registered cache kind with status mark (✓ populated /
      ○ cold / ✗ unresolvable), entry count, total size, and
      on-disk path. Annotates `tools` as a system store.
      Reports active `HULL_CACHE_DIR` override when set.
      Footer points users at `hull cache list|prune|clear`.
      JSON variant adds a `"caches": [...]` array (mirroring
      `hull cache list --json`) plus a top-level
      `"hull_cache_dir"` field.
      Single source of truth via `hl_cache_registry()` —
      doctor and `hull cache` always agree.
      Files: `src/hull/commands/doctor.c` (+90 LOC for the
      Caches section in both human + JSON renderers, plus
      `cache_stats` + `doctor_format_size` helpers shared
      between them).
      Verified: 32/32 e2e_cache (+8 new doctor assertions),
      42/42 unit, 36/36 e2e_blob, 12/12 e2e_aot_cache, 40/40
      e2e_templates. (`hull doctor --tui` will surface the
      new fields when its panel renderer is extended — the JSON
      already exposes the data; Lua-side TUI work is separate.)
- [x] §1.5.b-X-7. `hull inspect` cache disclosure. **Landed.**
      New "Runtime caches (auto-allowed, not in manifest)"
      section in `hull inspect` between Capabilities and
      Migrations. One row per registered cache kind with
      `[runtime]` or `[system]` tag, on-disk path, and the
      one-line description from the registry. Surfaces active
      `HULL_CACHE_DIR` override with a per-app-isolation note.
      Footer points at `hull cache list|prune|clear` and the
      `HULL_NO_CACHE=1` opt-out.
      Two new tool bindings (no Lua-side `os` needed):
        - `tool.cache_kinds()` → list of
          `{name, description, is_runtime, path}` tables
          (registry-driven; new kinds auto-disclose).
        - `tool.cache_override()` → current `HULL_CACHE_DIR`
          string or nil.
      Files: `src/hull/runtime/lua/mod_tool.c` (+two bindings),
      `stdlib/cli/lua/hull/inspect.lua` (+disclosure section),
      `tests/e2e_cache.sh` (+7 inspect assertions).
      Verified: 39/39 e2e_cache, 42/42 unit, 36/36 e2e_blob,
      118/118 e2e_build, 40/40 e2e_templates.
- [x] §1.5.b-X-9. QuickJS bytecode cache. **Landed (JS-side
      parity with X-1).** `hl_js_compile_module_cached` wraps
      `JS_Eval(JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY)`
      with on-disk memoization. On hit, `JS_ReadObject` with
      `JS_READ_OBJ_BYTECODE` skips the parse pass entirely.
      Cache key = sha256(QJS_TAG || arch || endian ||
      module_name || source). `module_name` is in the key
      because QuickJS bakes it into the bytecode for traceback.
      Store: `$HOME/.hull/blobs/runtime/js-bytecode/` via the
      same `hl_blob_store_put_keyed` primitive backing every
      other runtime cache.
      Wired into all three JS module-load sites (platform_vfs,
      app_vfs, filesystem dev mode) in `runtime/js/runtime.c`
      with a one-line swap from `JS_Eval(...)` to the cached
      helper.
      QJS_TAG (currently `"qjs-2024-01-13"`) tracks the
      vendored snapshot — must bump together with any
      `vendor/quickjs/` change to avoid loading stale bytecode
      into an incompatible runtime.
      Auto-registered in `hl_cache_registry()` as `"js-bytecode"`
      so it picks up `hull cache list|prune|clear`, `hull doctor`
      Caches section, and `hull inspect` runtime-caches
      disclosure with zero per-surface code.
      Honors `HULL_NO_CACHE=1` and `HULL_NO_JS_BYTECODE_CACHE=1`.
      Falls back to fresh `JS_Eval` on any cache I/O failure
      (corrupt file → unlink + recompile + repersist).
      Files: `include/hull/runtime/js_bytecode_cache.h`,
      `src/hull/runtime/js/bytecode_cache.c`,
      `src/hull/runtime/js/runtime.c` (3-site swap),
      `src/hull/cache_registry.c` (+1 row),
      `bench/bytecode_cache/bench_js_bytecode_cache.c`,
      5 new utests in `test_js`
      (`js_bytecode_cache.*`).
      Verified: 42/42 unit suites, 280/280 e2e_examples
      (every JS example boots and serves through the cached
      path), 39/39 e2e_cache.
      Microbench (M-series, 70 synthetic 3.8 KB modules):
        - COLD:   974 µs/load (parse + serialize + write)
        - WARM:   202 µs/load (read + JS_ReadObject)
        - BYPASS: 262 µs/load (parse, no persist)
        - Warm speedup: 1.30× — matches the Lua cache exactly.

- [ ] §1.5.b-X-8. LLM artifact cache (latent, v0.1.11+). Deferred
      until a real LLM consumer in Hull exists. Key shape:
      `sha256(prompt || context || model_id || temperature)`. LRU
      via `blob.cleanup`. Honors `HULL_NO_CACHE=1`.

### 1.5.c HTMX-specific stdlib companions. Tier 1 (target v0.1.10)

**Motivation.** v0.1.8 shipped the HTMX core (helper module, CSP, CSRF,
session, scaffold, example, docs). v0.1.9 adds multipart for file
uploads. Both close obvious gaps, but a developer building a real
internal tool on top of `hull init --profile htmx` still has to
hand-roll several patterns that every HTMX app needs in week one. This
section groups the HTMX-specific ones; the generic ones live in §1.4.

**Locked design principles:**

- **Stdlib modules, not example boilerplate.** Each pattern lands as
  `hull/<name>@1` with Lua + JS parity, manifest declaration, unit
  tests. Examples then demonstrate, not invent.
- **`examples/hypermedia_todo` is the showcase.** All patterns visible
  in the canonical example without bloating it past ~400 lines per
  runtime.
- **No new vendored JS unless the cost is justified.** The HTMX
  `response-targets` extension is the only candidate; everything else
  is server-side helper + template + CSS.

**Cross-references to §1.4** (generic web helpers that the HTMX
scaffold should also wire in during the same v0.1.10 release):

- §1.4-1 `hull/web/flash@1` — replace the silent POST/redirect/GET
  success in `examples/hypermedia_todo`; document the
  `HX-Trigger: flash` path in `docs/htmx.md`.
- §1.4-2 `hull/web/pagination@1` — paginate the todo list demo;
  document the fragment-swap pattern for "load more" / page-link
  navigation in `docs/htmx.md`.

**Tasks (v0.1.10, HTMX-specific):**

- [ ] §1.5.c-1. Search + debounce snippet pattern. NOT a stdlib module.
      Just a documented `hx-get` + `hx-trigger="keyup changed delay:300ms"`
      + ratelimit middleware recipe in `docs/htmx.md` and a
      `/search` route in `examples/hypermedia_todo` that filters
      todos by title. Tests for empty + matching + non-matching
      queries.
- [ ] §1.5.c-2. Inline-edit pattern. Click row → swap to form → submit
      → swap back. Add canonical `GET /todos/:id/edit` (returns form
      fragment) and `PATCH /todos/:id` (returns row fragment) to
      `examples/hypermedia_todo`. **HTMX-only** for the example
      (uses `hx-patch`); doc note in `docs/htmx.md` about the
      Rails-style `POST /todos/:id?_method=PATCH` recipe for apps
      that want plain-form degradation. CSRF token freshness: doc
      note recommending `max_age = 4 * 3600` for HTMX apps + a
      `csrf.refresh(req, res)` helper that re-issues the token on
      any fragment response (so long-running edit cycles don't expire
      mid-flow).
- [ ] §1.5.c-3. Loading-indicator scaffold. Add a Pico-compatible
      `.htmx-indicator` spinner block to scaffold `static/app.css`
      and a one-line `<div id="spinner" class="htmx-indicator">…</div>`
      to `templates/base.html`. Document `hx-indicator="#spinner"`
      and per-element spinners in `docs/htmx.md`.
- [ ] §1.5.c-4. Form re-population on validation error. The canonical
      HTMX form pattern: submit → server validates → server returns the
      same form fragment with submitted values pre-filled and per-field
      error messages inline. Doc recipe in `docs/htmx.md` showing the
      template-driven approach (`{{ values.title }}` + `{% if
      errors.title %}<small role="alert">{{ errors.title }}</small>{% end
      %}`). Add a `partials/_form_field.html` to the scaffold that
      bundles label + input + error + value-preservation. The
      `examples/hypermedia_todo` create form uses it. Wire the existing
      `hull.validate` output shape into the partial so handlers stay
      one-liner.
- [ ] §1.5.c-5. Wire `hull/middleware/idempotency@1` into the scaffold
      for POST/PATCH routes by default. Today the module exists but
      apps must opt in. Scaffolded `app.{lua,js}` calls
      `idempotency.init()` + `app.use_post("POST", "/*",
      idempotency.middleware({ get_principal = ... }))` so HTMX
      double-clicks return the cached response instead of double-writing.
      Doc note in `docs/htmx.md` § Idempotency.

### 1.5.d HTMX stdlib companions. Tier 2 patterns (no target)

**Motivation.** These are real gaps that show up once the app has
real users, but are not blocking week-one development. Batch when
prioritized.

**Tasks:**

- [ ] §1.5.d-1. Vendor `htmx-ext-response-targets.min.js` (SHA-pinned
      via `make fetch-htmx`). Add `htmx.error_target(res, "#err")` helper.
      Scaffold opts in via `<body hx-ext="response-targets">`. Doc
      note in `docs/htmx.md` covering the success-vs-error swap-target
      split.
- [ ] §1.5.d-2. Out-of-band swap helper. `htmx.oob(fragment_html,
      selector?, swap?)` composes safely; `htmx.compose(primary,
      oob_blocks)` concatenates with the right markup. Avoids hand
      string-building of `hx-swap-oob="…"` attributes. Tests covering
      multi-OOB composition.
- [ ] §1.5.d-3. HTMX-friendly ratelimit response. Extend
      `hull/middleware/ratelimit@1` with a `htmx_response` option.
      Behavior when set + `HX-Request: true`: first try to push a
      flash entry via `hull/web/flash@1` if it's declared (`flash.push(req,
      "Rate limit exceeded", "error")`), fall back to
      `HX-Trigger: {"rate-limited":{"retry_after_s": N}}` for apps
      without flash. Status code remains 429 either way. Default
      behavior (JSON 429 for non-HTMX requests) unchanged.
- [ ] §1.5.d-4. Vendor `htmx-ext-sse.min.js` + HTMX-SSE bridge demo.
      SHA-pinned via `make fetch-htmx`. Add to `examples/hypermedia_todo`
      (live "items just changed" feed) rather than a new example. Both
      runtimes. Tests for the SSE-fragment-swap round trip.
- [ ] §1.5.d-5. Vendor `htmx-ext-ws.min.js` + HTMX-WS bridge demo.
      SHA-pinned via `make fetch-htmx`. Pair with `app.ws("/chat", ...)`
      in a separate `examples/hypermedia_chat` (Lua + JS) — the
      bidirectional pattern is distinct enough from todo to warrant its
      own example. Tests.
- [ ] §1.5.d-6. History / back-button doc section in `docs/htmx.md`.
      Cover `hx-push-url="true"`, the trade-off vs. fragment-only swap
      (Back may show stale DOM), HTMX's history snapshot mechanism, and
      the canonical "if you push URL, also wire up the full-page
      fallback" pattern. No new code; pure docs.
- [ ] §1.5.d-7. Polling pattern doc section in `docs/htmx.md`.
      `hx-trigger="every 5s"` as the lightweight alternative to SSE for
      dashboards. Caveats: pair with ETag + 304 for cheap polls, server
      load scaling with N clients. ~30 lines of docs.
- [ ] §1.5.d-8. Styled confirmation dialog. `hx-confirm` triggers the
      ugly browser `window.confirm()`. Add `htmx.confirm_dialog(opts)`
      helper + a Pico `<dialog>`-based partial `partials/_confirm.html`
      that wires to `htmx:confirm` event. Scaffold opt-in. Tests for the
      confirm-then-submit and cancel paths.
- [ ] §1.5.d-9. CSP report-uri endpoint helper. Optional
      `csp.report_handler({path = "/csp-report", log_fn = ...})` middleware
      that accepts the browser's CSP violation reports, parses the JSON,
      and emits one structured log line per violation. Useful during
      rollout to catch nonce regressions. ~50 lines.
- [ ] §1.5.d-10. Client-side toast renderer for `flash.trigger`. Today
      the trigger path fires `HX-Trigger: {"flash":...}` but the
      scaffold has no listener — the event is invisible without
      app-side JS. Ship a ~30-line inline `<script nonce>` snippet in
      the scaffold's `base.html` that listens for the `flash` event
      and appends an `<article>` to `#flash-zone` with a
      `prefers-reduced-motion`-aware CSS fade-out. Five lines of CSS
      for the slide-in animation. Without this, `flash.trigger` is a
      half-finished API.
- [ ] §1.5.d-11. Form auto-save / draft pattern. Long forms shouldn't
      lose data on refresh. Pattern: every form field has
      `hx-trigger="input changed delay:500ms"` + `hx-post="/drafts/X"`,
      server writes to a `_hull_drafts` table keyed by
      `(session_id, form_id)`. On GET of the form, server pre-fills
      from the latest draft. Doc recipe + tiny `hull/web/drafts@1`
      module (~80 lines) + demo on `examples/hypermedia_todo`'s new
      todo form.
- [ ] §1.5.d-12. Rich text editor recipe. `hull/web/rte@1` or just a
      doc pattern: drop-in `<trix-editor>` (Trix is small, CSP-safe,
      no build step) with `hx-post` on the wrapping form. Server-side
      HTML sanitization via a small allowlist helper. Two-paragraph
      doc + Pico-styled CSS.
- [ ] §1.5.d-13. Date/time picker recipe. Native `<input type="date">`
      / `type="datetime-local">` are now well-supported and accessible.
      Doc the timezone gotcha (browser sends local time, server should
      treat as user's TZ unless the form explicitly carries the
      offset). Recipe for the common "convert to UTC on the server"
      pattern.
- [ ] §1.5.d-14. Drag-and-drop sorting. Vendor Sortable.js (or
      document the recipe inline). Pattern: items have
      `data-id` attributes, Sortable callback fires
      `hx-post="/items/reorder"` with the new order array. Server
      updates `display_order` columns in a single `db.batch()`.
      `examples/hypermedia_todo` reorderable list.

### 1.5.e HTMX polish. Tier 3 (no target)

**Motivation.** Nice-to-have. Pick off opportunistically when touching
adjacent code.

**Tasks:**

- [ ] §1.5.e-1. View Transitions API integration. Document the
      `hx-swap="transition:true"` recipe + a `prefers-reduced-motion`
      fallback. Scaffold opt-in via base template.
- [ ] §1.5.e-2. Static asset fingerprinting for **all of `/static/*`**
      (not just vendored). Template helper `{{ static('app.css') }}` in
      Lua templates and `{{ static('vendor/htmx.min.js') }}` style across
      both runtimes, rendering to `/static/<path>?v=<sha8>` (or
      `/static/<sha8>/<path>` if we go content-hashed). At `hull build`
      time, walk `static/`, compute SHA-256 of each file's bytes, emit
      a name → 8-hex manifest baked into the binary. Fingerprinted
      requests get `Cache-Control: public, max-age=31536000, immutable`;
      unfingerprinted requests keep today's `max-age=86400` behavior.
      Dev mode: helper returns mtime-based suffix so hot reload works
      without rebuilding. Doc note about how to opt out per asset.
- [ ] §1.5.e-3. ARIA live region in base template for fragment-swap
      announcements. `<div aria-live="polite" id="sr-announce" class="visually-hidden"></div>`
      + `htmx.on("htmx:afterSwap", announce)` snippet documented.
- [ ] §1.5.e-4. Dark-mode toggle. Pico v2 supports `data-theme` natively;
      add a `<details role="list">` theme picker partial to the
      scaffold with `localStorage` persistence. ~30 lines of JS,
      nonce-friendly.
- [ ] §1.5.e-5. i18n wired into the scaffold. `hull/i18n@1` exists
      but the scaffold doesn't use it. Add an `i18n.init(...)` call,
      sample `locales/en.json` + `locales/de.json`, template
      `{{ "todo.title" | t }}` usage. Doc note in `docs/htmx.md`
      about RTL handling + `Accept-Language` detection.
- [ ] §1.5.e-6. Subresource Integrity (SRI) for vendored assets.
      `<script src="/static/vendor/htmx.min.js" integrity="sha384-...">`
      catches tampered vendored files. SHA-pin already exists in
      `make fetch-htmx`; just propagate the hash into the scaffold's
      `base.html` via a template helper or build-time substitution.
      Defense-in-depth even with `script-src 'self'` CSP.
- [ ] §1.5.e-7. Charts / data viz recipe. Pick one CSP-friendly
      no-build option (Chart.js works with nonce script tags; vega-lite
      for declarative spec-driven; or just `<svg>` for simple bars/
      sparklines). Doc the pattern, don't vendor (these libs are too
      big to ship by default). Pairs with `hull/web/table@1` for
      "table + matching chart" admin dashboards.

### 1.5.f Enterprise / internal-app gaps surfaced by §1.5 (deferred, no target)

These are real gaps any internal-tools developer will hit, but they are
independent of HTMX itself. Tracked here so they don't get lost; each
needs its own design pass + estimate before scheduling. Items marked
`[COMMERCIAL]` are intended for the enterprise tier
(see [Licensing tiers](#licensing-tiers-planning-convention) above).

- [ ] Asymmetric-signature *verification* exposed through `crypto.*`
      (RSA-PKCS1v15, RSA-PSS, ECDSA P-256, ECDSA P-384). Prerequisite
      for the OAuth/OIDC item below: every mainstream IdP signs
      ID tokens with RS256 or ES256, and Hull's `hull/jwt` is HS256-
      only today. mbedTLS is **already linked** in every build with
      `HL_ENABLE_HTTP_CLIENT=1` (for the HTTPS client) and exposes
      `mbedtls_pk_verify` + the RSA/ECDSA primitives — they're just
      not bound to Lua/JS. Scope:
        - New cap functions in `src/hull/cap/crypto.c`:
          `hl_cap_crypto_rsa_verify(pubkey_pem, alg, data, sig)`
          and `hl_cap_crypto_ecdsa_verify(pubkey_pem, curve, data,
          sig)`. Public-key inputs accept PEM (SubjectPublicKeyInfo)
          and JWK (so JWKS responses don't need PEM conversion).
        - Lua + JS bindings: `crypto.verify(alg, pubkey, data, sig)`
          with `alg ∈ {"rs256","rs384","rs512","ps256","es256","es384"}`.
        - `hull/jwt` extended: dispatch by the token's `alg` header,
          accept a key-resolver callback (`function(kid) → pubkey`)
          so callers can plug in JWKS caches.
        - `alg=none` rejected unconditionally; alg-confusion attacks
          (HS256 token presented against an RSA pubkey) rejected by
          requiring the caller to pre-commit to an algorithm family.
      No new vendored deps. Roughly the same shape as the existing
      Ed25519 path in `cap/crypto.c`. Unlocks: native OIDC (next
      item), webhook signature verification for inbound webhooks
      (GitHub, Stripe, etc.), and SAML if the [COMMERCIAL] tier
      reaches it. **Surfaced 2026-06 by the Trimble HU asset-
      inventory project** (`asset_inventory_assessment.md` §b
      item 3) — that app needs Entra ID OIDC and is blocked on this
      primitive.
- [ ] OAuth 2.0 / OIDC sign-in for consumer providers (community tier).
      `hull/web/middleware/oauth@1`. Built-in providers for Google,
      GitHub, GitLab, Microsoft consumer accounts. Standard
      authorization-code flow with PKCE; ID-token verification via
      JWKS. Account-linking helper for users that sign in with
      multiple providers. Settles "Sign in with Google" as the
      table-stakes consumer-app pattern; line vs. commercial below is
      "consumer IdPs" (free) vs. "enterprise IdPs + provisioning"
      (paid). **Depends on the asymmetric-verify item above.**
- [ ] Two-factor auth (TOTP). `hull/web/middleware/totp@1`. RFC 6238
      time-based one-time passwords (Google Authenticator / 1Password
      / Authy compatible). QR-code provisioning helper (renders
      `otpauth://` URL as SVG without an external lib). Recovery codes
      generation + verification. Plays with the existing session
      middleware: after password verify, set `req.ctx.session.pending_2fa
      = true`, gate sensitive routes on it.
- [ ] Account lockout / suspicious-login detection.
      `hull/web/middleware/auth_lockout@1`. Tracks failed-attempt count
      per `(account_id, ip_prefix)` in a small table, blocks with
      exponential backoff (1s → 5s → 30s → 5m → 30m → lockout).
      Optional integration with the existing audit log so security
      events show up alongside business events. Pairs with a "your
      account is temporarily locked" template fragment.
- [ ] Transactional email flow recipes. The `hull/email` module exists
      but the scaffold doesn't ship templates or routes for the
      universal-need-list: welcome, verify-email, password-reset
      request, password-reset complete, account-deletion confirm,
      magic-link sign-in. Add a `hull/web/auth-flows@1` (or extend
      `auth-middleware`) that wires the routes + bundles `_text.md` /
      `_html.html` template pairs in `templates/email/`. Compose with
      §5's email retry/backoff.
- [ ] **[COMMERCIAL]** Enterprise SSO middleware. SAML 2.0, LDAP, SCIM
      provisioning, IdP-initiated SSO, JIT provisioning, group/claim
      mapping, session-bridge helpers. The community-tier OAuth/OIDC
      (consumer providers, above) stays free; this is the
      enterprise-IdP integration layer (Okta, Azure AD, Auth0
      enterprise tier, Ping, ADFS, etc.).
- [ ] **[COMMERCIAL]** Advanced RBAC. ABAC (attribute-based access
      control), policy-as-code, role hierarchies, dynamic permission
      resolution, audit-grade decision logging. The community-tier
      role-based RBAC (`hull.middleware.rbac`) stays free.
- [ ] **[COMMERCIAL]** Compliance audit log. Retention policies,
      structured export (SIEM / S3 / Splunk), tamper-evident hash chain,
      redaction rules, regulator-ready retrieval. Distinct from the
      community-tier per-request audit logging in `hull.middleware.logger`,
      which records HTTP-level events; this one records *business-object*
      events (who changed which record, before/after values, source request)
      with the compliance-grade durability primitives.
- [ ] First-party app audit-log helper (community tier). Basic version
      of the above without retention / SIEM / hash-chain. Records who
      changed which business object, before/after values, source request,
      timestamp, optional reason.
- [ ] Reusable admin UI conventions for the patterns not already covered
      by §1.4 (flash, pagination) or §1.5.c-§1.5.e (inline edit, search
      debounce, loading indicator, styled confirm, form re-population):
      filter forms, optimistic row replacement (with rollback on server
      error), bulk-select + bulk-action toolbars, sortable table headers
      (`hx-get="?sort=..."`). Stdlib doc + helper module rather than
      example boilerplate.
- [ ] Optimistic concurrency control for the lost-update problem
      (two users edit the same record; last write silently wins).
      Standard fix: `If-Match: <etag>` on PATCH, or a `version`
      column with `WHERE version = ?`. Helper `concurrency.guard(req,
      res, current_version)` that returns 412 Precondition Failed when
      stale. Doc note in `docs/htmx.md` about pairing with inline edit.
- [ ] Import/export workflow helpers: CSV preview, per-row validation
      errors, dry-run mode, commit step, background processing hooks for
      large imports.

---

## 1.6 Native sidecar services (`hull.services`)

**Priority:** High. Enables large native accelerators such as bitnet.c /
llama.cpp-style inference engines without embedding them into Hull's trusted
core and without pretending they are as safe as WASM plugins. Native sidecars
are lower-trust, out-of-process services with explicit capabilities, narrow
RPC, supervised lifecycle, and OS sandboxing where available.

**Security stance:**

Preserve the execution tiering:

| Tier | Runtime | Trust posture |
|---|---|---|
| 0 | Hull core | Small trusted runtime |
| 1 | Lua/JS app logic | Hull capability model + kernel sandbox |
| 2 | WASM plugins | Portable in-process sandbox, no I/O |
| 3 | Native sidecars | Isolated native accelerators, out-of-process |
| 4 | Native in-process plugins | Forbidden by default; trusted/unsafe only |

Native sidecars must never become a general `exec` escape hatch. They are
declared services resolved by Hull, launched by Hull, sandboxed by Hull, and
communicated with through Hull-owned transports. Sidecar tool metadata may
declare what the tool can support, but the app manifest remains the final
authority; tool metadata must not expand app capabilities.

**Key architecture constraint:**

Current server startup applies phase-1 pledge before loading app code, and
that phase intentionally blocks `exec`/`proc`/`fork`. Do not weaken this. To
keep services in `app.manifest()` while still spawning before the sandbox is
sealed, Hull needs a manifest pre-extraction pass for privileged launch-time
declarations.

The `services` block must be statically extractable from a literal
`app.manifest({...})` table/object before executing the app:

- Accept: literal strings, numbers, booleans, arrays, and objects/tables.
- Reject for service declarations: function calls, imports/requires, env
  reads, string concatenation, conditionals, loops, computed keys, or runtime
  values.
- Normal app capability extraction can still run after app load; native
  sidecar launch decisions must come from the static subset.

**Manifest sketch:**

```lua
app.manifest({
    modules = { "hull/services@1" },
    services = {
        bitnet = {
            type = "native-sidecar",
            tool = "bitnet-worker@1",
            transport = "stdio-fd", -- stdio-fd | unix | tcp
            protocol = "json-rpc",
            restart = "on-crash",
            trust = "confined-native",
            models = {
                main = {
                    path = "data/models/bitnet/model.gguf",
                    access = "fd-read",
                },
            },
            fs_write = { "data/cache/bitnet" },
            net = { connect = {}, listen = {} },
            limits = {
                memory_mb = 8192,
                cpu_percent = 400,
                processes = 1,
                open_files = 64,
                wall_ms = 600000,
            },
        },
    },
})
```

Prefer `tool = "name@major"` over `exec = "./path"`. Hull resolves tools via
the signed `hull tools install` registry, platform-specific artifacts, hashes,
install paths, and optional sidecar metadata. Local executable paths should be
development-only escape hatches, e.g. `dev_exec = "./bin/bitnet-worker"`, with
loud diagnostics and no production-signing claim.

**Resource passing model:**

Sidecars should receive Hull-resolved resources, not ambient path authority.
For model files, prefer pre-opened read-only resources:

```json
{
  "models": {
    "main": {
      "resource": 10,
      "kind": "file-read",
      "size": 4213379012,
      "label": "main"
    }
  },
  "cache": {
    "resource": 11,
    "kind": "dir-rw"
  }
}
```

On Unix, resources are inherited FDs or passed via `SCM_RIGHTS`. On Windows,
they are inherited handles. The protocol should call them "resources", not
POSIX-only FDs. The sidecar should not discover arbitrary model/cache paths on
its own.

**Transport and protocol model:**

- Default transport: dedicated inherited RPC FD (`stdio-fd`), not child
  stdin/stdout. Keep stdin/stdout closed or pointed at `/dev/null`; stderr is
  logs only. This avoids log/protocol mixing.
- Phase 2 transport: Unix domain sockets for long-running local daemons,
  including socketpair and filesystem socket paths.
- TCP: design now, but keep disabled by default until listen/connect
  capabilities are explicit and audited. Default TCP binding must be
  localhost-only unless manifest says otherwise.
- Initial protocol: JSON-RPC 2.0 with Content-Length framing
  (LSP-style), not newline-delimited JSON. This supports pretty JSON, robust
  framing, future binary headers, and clean parser error handling.
- Streaming responses: JSON-RPC notifications keyed by request id, e.g.
  `$/stream` events for tokens/progress/done.
- Cancellation: LSP-style `$/cancelRequest` plus Hull-enforced local
  deadlines. On timeout, send cancel, wait a grace period, then terminate or
  restart by policy.
- Backpressure: bounded write queues, bounded in-flight requests, bounded
  stream buffering, and explicit failure when the app does not consume fast
  enough.

**Example app API:**

```lua
local services = require("hull.services")

local out = services.call("bitnet", "generate", {
    model = "main",
    prompt = "Summarize this asset history",
    max_tokens = 256,
}, { timeout_ms = 60000 })
```

Sidecar method baseline:

- `rpc.discover`
- `health`
- `load_model`
- `unload_model`
- `generate`
- `embed`
- `tokenize`
- `cancel`
- `stats`

**Keel vs Hull ownership:**

Keel should own reusable event-loop transport primitives only: FD watchers,
timers, write queues, framing parser/serializer, and optional JSON-RPC
client/server helpers if they stay dependency-light.

Hull owns process boundaries: manifest parsing, tool resolution, service
supervision, process launch, FD/handle/resource passing, sandbox/resource
limits, capability checks, audit logs, app bindings, and lifecycle policy.

Start with a thin `hull/rpc` layer above Keel. Promote transport-neutral pieces
to Keel later only if they prove useful outside Hull.

**Platform sandbox tiers:**

Expose the actual enforcement level at runtime and in `hull agent services` so
operators do not confuse "native sidecar" with WASM-grade containment.

| Level | Platforms | Backend |
|---|---|---|
| strong | Linux | `no_new_privs` + seccomp + Landlock + rlimits + FD discipline |
| strong | OpenBSD / Cosmopolitan where supported | pledge/unveil + rlimits + FD discipline |
| good | macOS | Seatbelt + Hardened Runtime + rlimits + inherited-handle discipline |
| partial | FreeBSD | Capsicum where available, otherwise documented degradation |
| partial | Windows | Job Object + restricted token / AppContainer later |

**Error model:**

Keep failure classes distinct:

- `rpc_error`. Valid JSON-RPC error returned by a method.
- `protocol_error`. Malformed JSON, bad frame, invalid id, unsupported method.
- `transport_error`. EOF, EPIPE, timeout, reset.
- `service_exit`. Process exited with status/signal.
- `sandbox_violation`. OS denied/killed the sidecar.
- `supervisor_error`. Launch failed, bad tool, denied path/resource/capability.
- `app_error`. Lua/JS misuse of the service API.

**Phased plan:**

- [ ] Phase 0: design doc + threat model + repo survey. Decide the exact
      static manifest pre-extraction subset and signing implications.
- [ ] Phase 1: minimal `stdio-fd` Content-Length JSON-RPC client/server,
      dedicated RPC FD, stderr log separation, `rpc.discover`, `health`, and
      one request/response method.
- [ ] Phase 2: Unix socket transport, including socketpair and filesystem
      socket paths; consider FD passing for resources.
- [ ] Phase 3: lifecycle supervision: launch, readiness, health checks,
      crash detection, restart policy, log capture, graceful shutdown, and
      `hull agent services`.
- [ ] Phase 4: sandbox/resource limits: close inherited FDs, scrub env, set
      cwd, rlimits, Linux seccomp/Landlock, OpenBSD pledge/unveil, macOS
      Seatbelt profile, Windows/FreeBSD degraded backends.
- [ ] Phase 5: TCP transport gated by explicit manifest capabilities.
      Localhost-only default; no production remote bind without a declared
      listen policy.
- [ ] Phase 6: bitnet.c proof-of-concept sidecar installed through
      `hull tools install`, using pre-opened model resources and no arbitrary
      filesystem discovery.
- [ ] Phase 7: signed sidecar packaging: tool metadata declares supported
      needs, app manifest grants concrete resources, `hull verify` includes
      sidecar tool identity/hash in the app's signed deployment surface.

**Hard parts / security traps:**

- Manifest timing: sidecars cannot be discovered by executing app code after
  phase-1 pledge; service declarations need static pre-extraction.
- Dynamic linker behavior: native tools may need shared libraries unless
  shipped static. Prefer static sidecar artifacts where practical.
- Inherited FDs/handles are ambient authority; close everything except
  explicit RPC/log/resource handles.
- Environment variables leak authority; start from an empty env and add only
  declared values.
- `cwd` leaks filesystem reachability; set to a controlled service workdir.
- TCP can accidentally become an undeclared local network service; keep it
  off until capability gates and tests are in place.
- Sidecars spawning subprocesses must be blocked by sandbox/resource policy.
- Logs must never share the RPC stream.
- Restart loops can DoS the host; cap restart rate and expose status.
- Native sidecars are lower trust than WASM. Documentation and `inspect` /
  `agent services` output should make that explicit.

**Out of scope:** arbitrary user-controlled process execution, in-process
native plugins, unrestricted `dlopen`, plugin package managers, remote sidecar
orchestration, and claiming sidecars are equivalent to WASM sandboxing.

---

## 1.7 Native sandbox runner and services (`hull sandbox`)

**Priority:** Medium-High. Follow-on to native sidecar services. Reuses the
same process-supervision, resource-passing, sandbox, and limit machinery to
run native executables under declared capability manifests, either as
foreground one-shot processes or supervised daemon processes.

This is a different product surface from app-owned sidecars:

- `hull sandbox run` runs a foreground / one-shot native process.
- `hull sandbox service` supervises a long-running native daemon.
- `hull.services` launches app-owned RPC sidecars on behalf of Lua/JS app
  logic.

The goal is not to make arbitrary native binaries "safe" in the same sense as
WASM or Hull app code. The goal is to provide an honest, capability-oriented
launcher for native programs that can operate inside a narrow declared surface.

**Proposed CLI:**

```bash
hull sandbox run ./blabla --manifest manifest.json
hull sandbox run ./blabla --manifest -
hull sandbox run glpsol --manifest - < glpk.solve.json
hull sandbox service start --manifest daemon.json
hull sandbox service status local-solver
hull sandbox service stop local-solver
hull sandbox service logs local-solver
hull sandbox inspect manifest.json
hull sandbox explain ./blabla --manifest manifest.json
```

Avoid overloading `hull run`; Hull app execution and native process sandboxing
should remain visibly distinct.

`--manifest -` reads the manifest JSON from stdin. This is important for
agent workflows and one-shot generated runs where writing a temporary manifest
file is unnecessary or undesirable. When stdin is used for the manifest, the
child's stdin must be explicitly configured separately (`"stdin": "inherit"`,
`"stdin": "null"`, `"stdin": {"file": "..."}`, or a future dedicated input
FD), so manifest input cannot be confused with child process input.

**Best-fit workloads:**

| Fit | Workload |
|---|---|
| Best | Native tools designed for Hull sandboxing |
| Good | Static or mostly self-contained CLI tools |
| Good | Solver/optimizer tools such as GLPK and HiGHS, run unmodified against declared input/output files |
| Good | Local daemons with narrow Unix-socket interfaces and explicit readiness checks |
| Mixed | Ordinary Unix programs with predictable file/network needs |
| Poor | GUI apps, browsers, package managers, shells, build systems |

**Process manifest sketch:**

```json
{
  "type": "native-process",
  "exec": "./blabla",
  "args": ["--model-fd", "$HULL_RESOURCE_model"],
  "env": {},
  "cwd": "work/blabla",
  "resources": {
    "model": {
      "path": "models/model.bin",
      "access": "fd-read"
    },
    "cache": {
      "path": "cache/blabla",
      "access": "dir-rw"
    }
  },
  "fs": {
    "read": [],
    "write": []
  },
  "net": {
    "connect": [],
    "listen": []
  },
  "stdio": {
    "stdin": "inherit",
    "stdout": "inherit",
    "stderr": "inherit"
  },
  "limits": {
    "memory_mb": 4096,
    "open_files": 32,
    "processes": 1,
    "wall_ms": 600000
  }
}
```

Prefer pre-opened resources (`fd-read`, `dir-rw`, inherited handles on
Windows) over broad path visibility. Path allowlists are still useful for
legacy tools, but the highest-integrity mode is "the child sees only handles
Hull intentionally gives it."

**Unmodified solver tools:**

This runner should make it practical to use established native command-line
tools such as GLPK (`glpsol`) and HiGHS without linking them into Hull and
without rewriting them as WASM modules. Many solvers already have a narrow
file/stdio shape:

```bash
hull sandbox run glpsol --manifest glpk.solve.json
hull sandbox run highs --manifest highs.solve.json
```

Example GLPK-style manifest:

```json
{
  "type": "native-process",
  "tool": "glpk@5",
  "args": [
    "--math", "model/problem.mod",
    "--data", "model/data.dat",
    "--output", "out/solution.txt"
  ],
  "cwd": ".",
  "fs": {
    "read": ["model/problem.mod", "model/data.dat"],
    "write": ["out"]
  },
  "net": { "connect": [], "listen": [] },
  "env": {},
  "limits": {
    "memory_mb": 2048,
    "open_files": 32,
    "processes": 1,
    "wall_ms": 300000
  }
}
```

For unmodified tools, path allowlists are necessary because the program does
not know how to consume Hull resource FDs. For Hull-aware tools, prefer
resource placeholders. The runner should support both modes and make the
security tradeoff visible in `hull sandbox inspect`.

**Daemon/service mode:**

Sandboxed daemons need distinct lifecycle semantics. Do not stretch
`hull sandbox run` to mean "background this and hope"; make service mode
explicit.

Example service manifest:

```json
{
  "type": "native-service",
  "name": "local-solver",
  "tool": "highs-server@1",
  "args": ["--socket", "$HULL_SOCKET"],
  "cwd": "work/local-solver",
  "transport": {
    "type": "unix",
    "path": "run/highs.sock"
  },
  "readiness": {
    "type": "unix-connect",
    "timeout_ms": 5000
  },
  "restart": {
    "policy": "on-crash",
    "max_restarts": 3,
    "window_ms": 60000
  },
  "fs": {
    "read": ["models"],
    "write": ["run", "cache"]
  },
  "net": { "connect": [], "listen": [] },
  "env": {},
  "limits": {
    "memory_mb": 2048,
    "processes": 1,
    "open_files": 64
  },
  "logs": {
    "mode": "capture",
    "max_bytes": 10485760
  }
}
```

Daemon-specific requirements:

- Readiness detection: process-started is not ready. Support Unix-connect,
  TCP-connect, HTTP health endpoint, JSON-RPC `health`, and process-only as a
  last resort.
- Stable identity: services have names, state directories, PID/state files,
  socket paths, and status JSON.
- Lifecycle commands: `start`, `stop`, `restart`, `status`, `logs`.
- Graceful shutdown: send configured signal/request, wait grace period, then
  terminate.
- Restart policy: disabled by default; `on-crash` must include restart-rate
  limits.
- Stale socket cleanup: only remove sockets owned by the service state.
- Double-fork/background escape: treat daemonization that detaches from the
  supervisor as a policy violation unless a future platform service-manager
  integration explicitly supports it.
- Network daemons: prefer Unix sockets. TCP listen requires explicit
  `net.listen`, defaults to `127.0.0.1`, and must appear in `inspect/status`.

**Unified execution model:**

1. Parse and validate the native process manifest.
2. If the manifest came from stdin, require an explicit `base_dir` or use the
   current working directory with a diagnostic in `inspect`.
3. Resolve all paths relative to the manifest location, explicit `base_dir`,
   or the stdin fallback base.
4. Open declared resources before sandboxing.
5. Build the child argv from literal args plus resource placeholders.
6. Start from an empty environment; add only declared env vars.
7. Set a controlled cwd.
8. Close every inherited FD/handle except stdio and declared resources.
9. Apply resource limits.
10. Apply the best available OS sandbox backend.
11. Exec/spawn the child.
12. For `native-process`, monitor exit status, signals, wall timeout, logs, and
    sandbox diagnostics until the foreground process exits.
13. For `native-service`, wait for readiness, persist service state, monitor
    lifecycle, enforce restart policy, and keep logs/status queryable.
14. Emit structured JSON when requested (`--json`) for agents and tests.

**Relationship to sidecars:**

This should be implemented after the sidecar supervisor proves the core
primitives:

- process launch without shell invocation
- environment scrubbing
- close-on-exec / inherited FD discipline
- pre-opened resource passing
- rlimits / Job Objects / platform limits
- Linux seccomp/Landlock, OpenBSD pledge/unveil, macOS Seatbelt, Windows and
  FreeBSD degraded backends
- structured error and audit model

The runner should reuse the same internal API where possible, but without RPC
or restart policy by default.

**Platform semantics:**

The runner must report actual enforcement strength, not a generic "sandboxed"
claim:

```json
{
  "sandbox": {
    "level": "strong",
    "backend": "linux-seccomp-landlock",
    "degraded": false
  }
}
```

If a platform cannot enforce a requested capability boundary, default behavior
should be fail-closed unless the operator passes an explicit development flag
such as `--allow-degraded-sandbox`.

**Tasks:**

- [ ] Define `manifest.json` schema for `type = "native-process"` and resource
      placeholders.
- [ ] Define `type = "native-service"` schema for daemon lifecycle:
      name, transport, readiness, restart, logs, state dir, graceful shutdown.
- [ ] Support `--manifest -` to read manifest JSON from stdin, with explicit
      child-stdin configuration and clear `base_dir` path-resolution rules.
- [ ] Add `hull sandbox inspect` to normalize and display the resolved
      capability surface without running the process.
- [ ] Add `hull sandbox explain` to show which OS backend rules would be
      applied on the current platform.
- [ ] Implement `hull sandbox run` using the sidecar process/sandbox
      supervisor primitives.
- [ ] Implement `hull sandbox service start|stop|restart|status|logs` with
      service state files, stale-socket handling, readiness checks, graceful
      shutdown, and restart-rate limits.
- [ ] Support inherited stdio plus optional capture mode (`--json`,
      `--capture-output`, max output size).
- [ ] Support pre-opened file/dir resources and placeholder expansion in argv.
- [ ] Support path-allowlisted legacy tools that cannot consume resource FDs,
      with `inspect` clearly marking the broader path-based authority.
- [ ] Add network capability gates: no connect/listen by default; localhost
      listen/connect only when declared; remote connect only when declared.
- [ ] Add solver examples for GLPK and HiGHS, including read-only model/data
      inputs, write-only solution directories, no environment, no network, and
      bounded memory/wall-time.
- [ ] Add daemon examples using Unix sockets first; TCP daemon examples only
      after `net.listen` policy is implemented and tested.
- [ ] Add clear error classes mirroring sidecars: manifest error, launch
      error, sandbox unsupported, sandbox violation, timeout, exit status,
      signal, readiness failure, restart exhaustion, escaped daemon,
      resource denied.
- [ ] Add tests with tiny static helper binaries that attempt allowed and
      denied file reads/writes, env reads, network access, subprocess spawn,
      and FD leakage.
- [ ] Document honest workload fit and platform degradation behavior.

**Security traps:**

- Arbitrary native programs often depend on dynamic libraries, locale files,
  `/proc`, temp dirs, config files, DNS files, and subprocesses. Tight
  manifests will break many programs; broad manifests weaken the guarantee.
- Shells and build systems are poor targets because they are designed to
  discover and execute more programs.
- GUI applications are out of scope for v1; window-system access is a broad
  ambient channel.
- Dynamic linker behavior can undermine "no file access" claims unless the
  child is static or the loader/library paths are explicitly accounted for.
- Passing path strings instead of resources invites confused-deputy bugs.
- Leaking an inherited directory FD can bypass path allowlists.
- Network controls must be OS-enforced where possible, not only checked by
  Hull before launch.
- Daemons that double-fork, write arbitrary PID files, or reopen logs outside
  declared paths can escape supervision assumptions.
- Stale Unix sockets and PID files can cause confused ownership unless state
  files include Hull-generated service identity.
- On platforms with partial sandbox support, the CLI must be explicit about
  degraded enforcement.

**Out of scope:** remote orchestration, containers, VM isolation, package
manager sandboxes, GUI app sandboxing, system-wide init/service-manager
integration, and claiming arbitrary native binaries are equivalent to Hull
apps or WASM plugins.

---

## 1.8 [COMMERCIAL] Hardware-token + KMS signing integrations

**Priority:** Medium. Enterprise-tier counterpart to the community-tier
software-key signing. The community tier already provides
`hull sign-release` and `hull sign-platform` with raw Ed25519 keys
generated by `hull keygen`; this section is the productized integration
with hardware tokens and cloud KMS for customers whose threat model or
compliance requirements forbid software-only key storage.

Distinct from [§0.3.3](#03-trust-chain-hardening-post-v015-gap-analysis)
which tracks gethull's OWN release-signing key custody (an internal
operations decision, not a customer feature). This section is the
customer-facing feature.

**Planned integrations:**

- [ ] YubiKey (OpenPGP applet for Ed25519, PIV for ECDSA P-256). Local
      PCSC/CCID access via the customer-host's smart-card daemon. No
      private key material ever leaves the token.
- [ ] AWS KMS asymmetric keys (Ed25519, ECDSA, RSA-PSS). Sign-blob
      operation via the KMS API; private key never leaves AWS.
- [ ] GCP KMS asymmetric keys. Same shape as AWS KMS.
- [ ] Azure Key Vault keys. Same shape.
- [ ] Multi-party signing ceremony: m-of-n schemes for major releases.
      Distributed sign-then-aggregate.

**API sketch:**

```
hull sign-release hull.sha256 --key yubikey:slot=auth
hull sign-release hull.sha256 --key aws-kms:arn=...
hull sign-platform <platform>.a --key gcp-kms:project=...
hull sign-release hull.sha256 --key m-of-n://policy.json   # multi-party
```

**Tasks:**

- [ ] Pluggable signer backend in the `hull sign-*` family. Abstract
      `HlSigner` vtable with a default software-key implementation
      (community tier) and hardware/KMS implementations (commercial
      tier, lives in `artalis-io/hull-enterprise`).
- [ ] YubiKey OpenPGP integration (via libgpgme or direct CCID).
- [ ] AWS / GCP / Azure KMS clients (REST, signed with `http.fetch`).
- [ ] Multi-party signing protocol + ceremony tooling.
- [ ] Compliance docs: which integrations satisfy FIPS 140-3, Common
      Criteria EAL4+, eIDAS qualified signature requirements.

**Out of scope:** Building a key-management product. The KMS integrations
are just signing callouts to existing KMS systems the customer already
runs.

---

## 2. `hull tools install`. Side-loaded optional tools  ✅ Shipped (v0.1.2)

**Design:** [`tools_install.md`](tools_install.md). What landed:

- `hull tools install <name>` / `list [--json]` / `uninstall` subcommands.
- Tools land in `$HOME/.hull/tools/` (mode 0755), isolated from PATH.
- Trust chain reuses the same Ed25519-signed `hull.sha256` manifest as
  `hull update`. No new keys, no new ceremonies.
- Version-coupled: pulls from the SAME release as the running hull
  binary (not "latest"), so e.g. wamrc stays at the WAMR commit hull
  was compiled against.
- First concrete tool: `wamrc` (WAMR AOT compiler), published for
  linux-x86_64 / linux-aarch64 / darwin-arm64. Cosmo unsupported
  (LLVM doesn't fit a fat APE binary). Cosmo users `make wamrc`.
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

## 3. Platform-sig completion. Make `HL_PLATFORM_PUBKEY_HEX` meaningful

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

**Priority:** High for v0.1.3. This was the loudest remaining gap on
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
  rule got removed. The real key is in the GH secrets, just not
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
| `release_io.{c,h}`. HTTPS GET, signed-manifest verify, SHA-256, atomic write | Same module verifies the embedded platform-sig blob (no new code paths) |
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
  running unsigned apps. Same opt-in behavior as today, so legacy
  v0.1.0–v0.1.2 apps continue working at runtime.

### Manifest format

Mirror `hull.sha256`'s shape. Line-based text, not JSON:

```
0000000000000000000000000000000000000000000000000000000000000001  linux-x86_64
0000000000000000000000000000000000000000000000000000000000000002  linux-aarch64
0000000000000000000000000000000000000000000000000000000000000003  darwin-arm64
0000000000000000000000000000000000000000000000000000000000000004  cosmo-x86_64
0000000000000000000000000000000000000000000000000000000000000005  cosmo-aarch64
```

Why: avoids JSON canonicalization headaches entirely (deterministic
key order, whitespace, escaping). Signed against the file bytes.
Reuses every helper from `release_io.{c,h}`.
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
| App with empty `package.sig.platform` at runtime WITHOUT `--verify-sig` | No change from today. Runs as-is. Default `hull <app>` doesn't verify signatures unless asked. |
| `--no-verify-platform` passed at any step | Skip the check, log once at info level. |

The `--no-verify-platform` flag exists on both `hull build` and the
runtime serve path. It's the documented escape valve for
dev-built hulls and for forensic-mode operation; expected to be
rare in production. Without it, self-built dev hulls can't build
production-ready apps. An acceptable strict-default tradeoff
matching the v0.1.2 audit-hardening posture.

### Six commits, ordered

The original draft of this plan put "restore the real pubkey" first
as a small spike commit. That sequencing was wrong: restoring the
pubkey activates `hl_verify_startup`'s platform-key pinning, which
hard-rejects any app whose `package.sig.platform.public_key_hex`
doesn't match the embedded key. Today's
`hull sign-platform` + `hull build --sign` developer flow (exercised
by `e2e_build.sh` Step 14) signs platforms with the developer's own
key. So the moment the real gethull pubkey is embedded, every
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
| 1 | `sig:` | `src/hull/platform_sig.{c,h}` (manifest builder, signer, verifier, `extract_for_arch` helper. Pure data; reuses `release_io` helpers (`find_checksum`, `verify_manifest_sig`, `sha256_hex`). Unit tests with synthetic hashes including mismatch + tamper cases. Standalone) no runtime behavior change. | 1d |
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

### Tasks (in dependency order. Mirrors the commit table)

- [ ] `src/hull/platform_sig.{c,h}`. Manifest builder + signer +
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
  documented intended behavior. Opt-in stricter verification.
  Rebuild against v0.1.3+ hull to make those apps pass.

### Effort

Realistic estimate: **5 engineering days** (split roughly 4
engineering + 1 release-engineering, matching the v0.1.2 shape).
The CI reorg in step 3 is the highest-risk piece; the rc1 → smoke →
clean tag dance from v0.1.2 applies here too.

---

## 3.1 Cosmo APE: tool-mode compiler invocation on Linux

**Priority:** Medium. Discovered while gating v0.1.3. The release
workflow's platform-sig E2E smoke test passes on all three native
arches (darwin-arm64, linux-x86_64, linux-aarch64) but had to be
skipped for the Cosmopolitan APE build because `hull build` can't
spawn a system compiler when run as a cosmo binary on Linux.

**Symptom:**

```
/usr/bin/cc: error while loading shared libraries: libc.so.6:
              failed to map segment from shared object
/usr/bin/gcc: error while loading shared libraries: ...
/usr/bin/clang: error while loading shared libraries: ...
hull: compiler 'cosmocc' not found in PATH
hull build: no C compiler available
```

Even with `--compiler cosmocc` and `/opt/cosmo/bin` on `$PATH`,
hull's tool-mode sandbox (pledge/unveil polyfill provided by
jart/cosmopolitan) restricts file-system access to a small allowlist
that doesn't include the system loader paths needed by `cc`/`gcc`/
`clang`, nor the cosmocc install location. The fork+exec succeeds
but the child's dynamic loader can't map its dependencies.

**Impact:**

End-users running `hull-cosmo` on Linux can't run `hull build`
(unless they bypass the sandbox, which we don't expose for tool
mode). The cosmo binary is fine for `hull dev`, `hull test`,
`hull <app>`, `hull update`, `hull tools install`. Those don't
spawn a system compiler. Only `hull build` is affected, and only
when `hull-cosmo` is the binary doing the building.

End-users have two workable paths today:

1. Install a native hull (`hull-linux-x86_64` / `hull-linux-aarch64`)
   for the platform doing the building, keep `hull-cosmo` for
   distributing to mixed-OS targets.
2. Build outside the sandbox manually (extract platform library +
   invoke the compiler by hand).

Neither is a great story for the "one binary for all OSes" cosmo
promise.

**Fix candidates** (one or more. Design once we pick up the task):

1. **Widen the tool-mode unveil set.** Add `/lib`, `/lib64`,
   `/usr/lib`, `/usr/lib64`, and the `cosmocc` install dir
   (`$HOME/.cosmocc/` and `/opt/cosmo/`) to the tool-mode allowlist.
   The risk: a wider sandbox during `hull build` weakens the
   capability story for build-time tooling. Possibly OK because the
   build step is supposed to invoke a compiler.
2. **Auto-detect `cosmocc` location.** `hl_compiler_select`'s system
   candidates today are `cc, gcc, clang`. Add `cosmocc` and try a
   few well-known install paths (`$HOME/.cosmocc/bin`,
   `/opt/cosmo/bin`) before falling back to `$PATH`. Doesn't fix the
   shared-library mmap issue for native compilers, but at least
   lets `cosmocc` work.
3. **Use embedded TinyCC on the cosmo binary too.** If TCC's
   embedded codegen can target the cosmo runtime, `hull build` could
   skip the system compiler entirely on cosmo. Requires investigating
   whether the embedded TCC builds cleanly under cosmocc and produces
   loadable APE/native output.
4. **Run the cosmo-side E2E smoke test inside a chroot** that
   doesn't apply the polyfill. Closes the CI gap without affecting
   end-users.

**Definition of done:**

- `hull build --sign` works end-to-end from a `hull-cosmo` binary
  on Linux, in the default sandbox, with the same `--verify-sig`
  pass as the native builds.
- Re-enable the cosmo case in `release.yml`'s "Platform-sig E2E
  smoke test" step.

---

## 3.2 Auto-extract embedded `libhull_platform.a` for tool-mode commands

**Priority:** Medium-High. The companion to §3.1. Both fall out of
the same architectural gap.

End-users installing a release binary via
`curl -fsSL https://gethull.dev/install.sh | sh` get a hull binary
with `libhull_platform.a` *embedded* (the `EMBED_PLATFORM=1` build
flow). `hull build` knows how to extract the embedded archive to a
tmpdir and feed it into the link step. But two other tool-mode
commands hit a missing-file error because they don't:

```
$ hull eject
hull eject: cannot find libhull_platform.a
hint: run `make platform` first

$ hull sign-platform --dir /some/dir/ key
hull sign-platform: no platform libraries found in /some/dir/
```

The "run `make platform`" hint is only actionable for someone with
the Hull source tree. End-users who installed via the release
binary have only the `hull` executable. There's no make,
no `vendor/`, no way to materialize the .a without rebuilding hull
from scratch.

**Impact:**

- `hull eject` is unusable on installed release binaries. Eject's
  whole purpose is "give me a self-contained scaffold I can build
  from without hull". Exactly the audience that has only the
  binary.
- `hull sign-platform` (the v0.1.2 per-app developer-signed
  platform layer) is unusable for the same reason. End-users who
  want to ship signed apps with `hull build --sign` need to either
  build hull from source or transplant a .a from somewhere.

**Fix:**

Both commands already have a clear extraction sink: the same
embedded blob the `hl_embedded_platform_*` accessors expose to
`hull build`. The fix is to call those accessors from
`commands/eject.c` and `commands/sign_platform.c` (or their Lua
stdlib equivalents), write the bytes to a tmpdir, and pass that
path through to the existing logic. `build.lua` already does this
pattern. It's just not factored into a shared helper that the
other tool commands can reuse.

Suggested factoring:

- `hl_platform_lib_extract(tmpdir, &out_path)` in
  `src/hull/build_assets.c` (where the embedded blob lives).
  writes `libhull_platform.a` (single-arch) or both cosmo arches to
  `tmpdir/` and returns the path. Returns -1 if no platform is
  embedded.
- `stdlib/cli/lua/hull/eject.lua` calls this before scanning for
  `libhull_platform.a`. Same for `stdlib/cli/lua/hull/sign_platform.lua`.

Same blob, same trust chain (the embedded bytes are what
sign-platform-manifest signed at release time), no new code paths
through the sandbox.

**Definition of done:**

- `hull eject` works on an installed release binary, with no source
  tree present.
- `hull sign-platform` works the same way. Produces a `platform.sig`
  that `hull build --sign` accepts.
- The "run `make platform` first" hint is replaced with the
  extraction logic for binaries with embedded platforms; the hint
  stays only for hulls built without `EMBED_PLATFORM=1` (where
  there's genuinely no .a to extract).

---

## 3.3 Platform-sign chain follow-ups (rough edges)

**Priority:** Low–Medium. The main chain works end-to-end and is
CI-gated as of v0.1.3, so none of these block anything. Group them
into one v0.1.4 cleanup batch alongside §3.1/§3.2.

**1. Sandboxed `--verify-sig` smoke coverage.**

The new platform-sig E2E smoke test in `release.yml` runs the
output binary with `--no-sandbox -p 19888 ./app.lua` to avoid
fighting the sandbox during CI. That means we've validated the
signature chain works under `--verify-sig` *without* the runtime
sandbox active. We haven't proven `--verify-sig` + full sandbox
play nicely together on a release-built binary.

Add a second variant of the smoke step that starts myapp with the
sandbox on (default) and confirms the same `/` returns
`{"ok":true}`. Catches any signature-chain code that accidentally
needs paths/syscalls the sandbox blocks.

**2. Gethull-key rotation story in `docs/security.md`.**

`HL_PLATFORM_PUBKEY_HEX` is embedded into every hull binary at
compile time. Today's `docs/security.md` describes the key but
not what happens when it rotates (compromise; scheduled rotation;
moving to a hardware-backed key). The implicit story is:

  1. Generate new keypair offline.
  2. Update `HL_PLATFORM_PUBKEY_HEX` in `include/hull/signature.h`,
     update `GETHULL_DEV_PLATFORM_KEY` in `site/verify.html`, ship a
     hull release. New release signs new manifests with the new key.
  3. Old hull binaries keep working. They verify against the OLD
     pubkey embedded in them, against the OLD signed manifests they
     carry. Apps built with old hulls keep verifying.
  4. New apps built with new hull verify against the new pubkey.
  5. There is NO cross-validity: a new hull can't verify an old
     app's gethull layer (different signing key). `--no-verify-platform`
     is the documented escape for that case.

This needs to be written down in `docs/security.md`, including the
"what if someone publishes hull binaries claiming to be us with a
key we control" scenario (release signature gates that. Same
HULL_RELEASE_KEY chain). One page; mostly documentation.

**3. `hull verify --gethull-key <file>` parity in Lua.**

`stdlib/js/hull/verify.js` accepts `--gethull-key <file>` to verify
against an explicit pubkey (because the JS tool runtime can't reach
the embedded `HL_PLATFORM_PUBKEY_HEX`). `stdlib/cli/lua/hull/verify.lua`
relies entirely on `tool.platform_pubkey()`. No override. Add the
same `--gethull-key` flag to the Lua verifier for symmetry:

  - Use for offline auditing of a hull-signed app from a machine
    with a different hull binary (or no hull at all, via the
    browser verifier. Which already accepts override).
  - Use for verifying a fork's apps against the fork's pubkey
    without rebuilding hull with `-DHL_PLATFORM_PUBKEY_HEX=…`.

~30 lines of Lua plus a doc note.

**4. Extend `tests/release_smoke.sh` to cover platform-sig.**

The post-publish manual smoke today exercises `hull tools install
wamrc` (release-sig path) but not `hull build --sign + --verify-sig`
(platform-sig path). The reason was §3.2: an installed hull binary
can't run `sign-platform` without an external .a. Once §3.2 lands
and `sign-platform` works on installed binaries, extend
`release_smoke.sh` with a platform-sig section:

  ```sh
  hull keygen dev
  hull keygen plat
  hull sign-platform --dir ~/.local/bin/ plat
  hull build --sign dev.key -o /tmp/myapp some-tiny-app/
  /tmp/myapp --verify-sig dev.pub --no-sandbox -p 19888 some-tiny-app/app.lua &
  sleep 2
  curl -fsS http://127.0.0.1:19888/ | grep ok
  ```

Same checks the CI smoke test runs, but post-publish against the
actually-published binary. Catches any "the artifact uploaded
isn't the artifact CI built" surprise.

**Definition of done (collective):**

- CI's platform-sig E2E smoke step has a sandboxed variant.
- `docs/security.md` has a "Key rotation" subsection under §2.
- `hull verify --gethull-key <file>` works in Lua, with a matching
  `--help` line and a doc bullet in `docs/security.md §6.B`.
- `tests/release_smoke.sh` exercises both the release-sig and
  platform-sig paths post-publish.

---

## 4. Background job queue (`hull.jobs`)

**Priority:** High (bumped from Low after the §1.5.c HTMX surface
landed). The existing transactional outbox + inbox patterns cover
reliable side-effect delivery for *outgoing* events, and `app.every` /
`app.daily` timers handle simple schedules. What's missing: durable
queueing of user-triggered work — image resizing, CSV import, PDF
rendering, sending a batch of emails after a paid plan upgrade,
scheduled report generation. Any non-trivial web app needs at least
one of these within the first month.

Adjacent to §5: the email transactional flows shipped there should
schedule retries via this queue rather than each carrying their own
backoff logic.

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
- [ ] `jobs.enqueue()`. Insert with optional delay
- [ ] `jobs.process()`. Atomic claim + execute + update status
- [ ] Retry with exponential backoff (reuse `outbox.backoffDelay` math)
- [ ] Dead-letter queue for permanently-failed jobs
- [ ] JS parity

---

## 5. Email retry/backoff + transactional flow recipes

**Priority:** High (bumped from Low). Phase 6 wrapped `email.js` /
`email.lua` providers in try/catch so errors now surface cleanly as
`{ok:false, error}`. The retry envelope is the easy half; the harder
half is that **the scaffold doesn't show how to actually USE email**
for the standard set of flows every web app needs.

**Retry envelope (the easy half).** Wrap `email.send` itself (not
each provider) in an `opts.retry = { max_attempts, base_delay_ms }`
envelope. Use the same exponential backoff math as
`outbox.backoffDelay`. Once §4 lands, schedule retries through
`hull.jobs` instead of inline `app.every` callbacks.

**Transactional flow recipes (the load-bearing half).** Ship
template pairs and scaffold routes for the universal flows:

- Welcome email on signup
- Email verification (token-link + tap to confirm)
- Password reset request → email with reset link
- Password reset complete → confirmation email
- Magic-link sign-in (passwordless)
- Account deletion confirmation
- Notification digest (daily/weekly opt-in)

Each ships as a Pico-styled `<table>`-based HTML template + plain-text
counterpart under `templates/email/`. Scaffolded `app.{lua,js}` wires
the routes. Email previews via a dev-only route `/__email/preview/<name>`
that renders the template with sample data (no actual send) — invaluable
when iterating.

Pairs with §1.5.f's `hull/web/middleware/auth-flows@1` (the route +
session-management half).

---

## 6. Test coverage gaps surfaced by audits

Three items the audits flagged as deserving unit tests (currently e2e-only):

- [ ] **`hl_migrate_*`**. `src/hull/migrate.c` has no unit-test suite. Edge cases (checksum mismatch, missing migrations table, concurrent attempts) deserve in-process tests.
- [ ] **Sandbox profile builder** (`sandbox.c::build_seatbelt_profile` / unveil-path builder). Only e2e-covered today, and only on the platforms CI runs. A unit test calling the profile-build helper and asserting on the generated SBPL/unveil list would catch regressions on platforms CI doesn't run.
- [ ] **`hl_snprintf_append` helper**. Phase 5 audit recommendation. Replaces the brittle `req_len += snprintf(...)` idiom that recurs in `agent/request.c`, `cap/smtp.c`, and template codegen. Land the helper + tests + sweep the call sites.

---

## 7. Observability (structured logs + metrics + traces)

Hull's local-first majority gets observability for free: `--audit`
emits JSON capability events, `hull.middleware.logger` emits logfmt
request lines, both go to stderr where `tail -f` / `grep` / `jq`
work fine. The local-deployment case (single binary, single host,
single process) is fully covered by what's already shipped.

This section is for the **cloud-deployed multi-service slice**:
apps running behind a load balancer where requests flow through
multiple Hull instances, where stderr is captured by a log
aggregator, where ops needs to answer "which of these 12 instances
served the request that took 8s, and was it the DB or an outbound
API?" That's not Hull's identity question; it's a complementary
deployment mode. The priorities below reflect that.

### 7.1 Structured-JSON request log alongside logfmt

**Priority:** Medium. Useful for ANY deployment that pipes logs to
an aggregator — that includes some local-first setups (Loki on a
homelab, journald → systemd-cat → jq), so the win isn't strictly
cloud-only.

Today `logger.middleware` emits one logfmt line per request. Add an
`opts.format = "json" | "logfmt"` (default logfmt for human dev,
JSON when set). Shape matches the de-facto contract Datadog / Loki
/ Splunk Cloud / journald accept: `{timestamp, level, msg, method,
path, status, duration_ms, request_id, session_id, user_id?, ...}`.
Small change, broadly useful.

### 7.2 Prometheus-format metrics exporter

**Priority:** Low for local apps; Medium for cloud-deployed apps.
Single-instance local apps see `hull doctor` / a quick `htop` and
move on. Multi-instance fleets need pull-scraped metrics.

`hull/web/middleware/metrics@1` exposes `/metrics` in Prometheus
exposition format. Built-in metrics: request-count, request-duration
histogram (per method + status), in-flight requests, DB-query count,
WASM/GPU call count + duration. Apps add custom counters via
`metrics.counter("name", { labels = {...} }):inc()`. Optional
`--metrics-port` to separate the scrape endpoint from the public
listener.

### 7.3 OpenTelemetry traces (W3C `traceparent` propagation)

**Priority:** Low. Distributed tracing solves "where did this request
spend its 8s across 5 services?" — which is a problem local-first
apps don't have (they ARE the whole system). Add when there's an
explicit user asking; until then the implementation cost (spans,
context propagation, exporter buffering, batching) isn't worth
carrying.

If/when built: `hull/web/middleware/otel@1`. Reads incoming
`traceparent` header, generates spans for the request lifecycle
(route match, middleware chain, handler, DB queries, outbound HTTP,
WASM/GPU dispatch). Exports via OTLP/HTTP to a collector (no
embedded vendor SDK dependency). Adds `traceparent` to outbound
`http.fetch` calls. Spans carry the `request_id` so traces correlate
with logs.

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
   the platform-sig wire format isn't active yet. See section 2.
5. ✅ `v0.1.0` tagged 2026-05-25, release workflow signed
   `hull.sha256` and published all six artefacts.

See [`release_signing.md`](release_signing.md) for the full flow and
the "Release Process" section of [`../CLAUDE.md`](../CLAUDE.md) for the
operational checklist.
