# Hull. Next Features Roadmap

Status: **Active** | Last reviewed: 2026-07-12

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
- ✅ **v0.1.10 batch (§1.4 + §1.5.b-X + §1.5.c)**. Three sections of the v0.1.10 milestone fully shipped: (a) generic web stdlib — `hull/web/flash@1` POST/redirect/GET one-shot notifications with both session-stash and `HX-Trigger` paths, and `hull/web/pagination@1` offset-based paginated lists; (b) hull/blob@1 migrations + runtime-infrastructure caches — `hl_blob_store_*` low-level CAS extraction, four runtime caches (Lua/JS bytecode, Lua/JS template), compute-AOT cache via wamrc memoization, tools install via signed blob_store, `hull cache list|prune|clear|verify` CLI surface, `HULL_CACHE_DIR` per-app isolation, `docs/cache.md` standalone reference; (c) HTMX Tier 1 patterns — search+debounce, inline-edit with `csrf.refresh`, loading-indicator scaffold, form re-population on validation error with `partials/_form_field.html`, idempotency-by-default for POST/PATCH including HTML response replay. §1.5.b-X subsystem audit-closed after four passes ([§1.5.b-X](#15b-x-hullblob1-migrations-target-v0110)).
- ✅ **v0.2.0 batch (§1.3 + §1.5.a)**. Hypermedia profile + web stdlib namespace reorganization. `hull init --profile htmx` scaffolds a complete HTMX + Pico app (CSP nonce, CSRF, session, flash, pagination, search-with-debounce, inline edit, loading indicator, form re-population, idempotency). 20 strictly-web stdlib modules moved under `hull/web/*` with fix-it migration hints. See [`../CHANGELOG.md#020`](../CHANGELOG.md).
- ✅ **v0.3.0 batch (§1.5.b + §1.5.f, items 1–5 + first-party audit-log)**. Production-grade auth stack: `hull/web/auth-flows@1` (registration / verify / login / password-reset / magic-link / email-change / optional TOTP), `hull/web/middleware/totp@1` (RFC 6238 with dual-row enrollment + multi-key rotation + per-user + per-IP lockout), `hull/web/middleware/oauth@1` (OIDC Authorization Code + PKCE; Google + Microsoft Entra presets), `hull/web/middleware/audit-log@1` (append-only with per-device fingerprint + `cleanup_status` tri-state), `hull/web/auth-health@1` (probe + `hull agent auth-status`), `hull/web/pwned@1` (HIBP k-anonymity + 80KB embedded blocklist), `hull/qrcode@1`. Plus streaming multipart upload (`req:multipart()` / `req.multipart()`), `hull/attachment@1` + `hull/blob@1` + `hull/mime@1`, asymmetric crypto (RS256/384/512, PS256, ES256/384), SHA-NI runtime dispatch. **13 iterative security-audit rounds** converged in round 13 (zero findings, three independent reviewers). 132 commits since v0.2.0. See [`../CHANGELOG.md#030`](../CHANGELOG.md).
- ✅ **v0.4.0 batch (2026-06-17)**. HTMX widget tier complete (8 widgets: toast, confirm, form, search, inline-edit, sort, pagination, table) with browser-driven Playwright E2E coverage. Three production-affecting `hull build` + sandbox fixes (a standalone binary silently broke any app declaring `manifest.fs.write`; `static/vendor/*` assets weren't embedded). See [`../CHANGELOG.md#040`](../CHANGELOG.md).
- ✅ **v0.5.0 batch (2026-07-10, §5 + §6 native)**. Build-flavor epic (native half). All four HTTP flavors (full / server-only / client-only / pure-compute) link clean and are CI-covered; `hull build --flavor` + `--flavor=auto` (infers the minimal flavor from declared modules, validates the manifest against the target flavor at build time); signed per-flavor native platform libs + `hull platform install <flavor>` / `platform list`; build-time platform-lib re-verify closing the install->build TOCTOU. See [`../CHANGELOG.md#050`](../CHANGELOG.md).
- ✅ **v0.6.0 batch (2026-07-10, §6 cosmo)**. Signed published cosmo per-flavor platform libs (dual-arch), each built on its own fresh runner to avoid the second-cosmo-build loader corruption, + cosmo `hull platform install`. See [`../CHANGELOG.md#060`](../CHANGELOG.md).
- ✅ **libhull no-runtime embedding flavor (L-1..L-5, post-v0.6.0, on main)**. `make libhull` -> `libhull.a`, the runtime-free hardened core (no Lua/JS) that a native C/Rust/Zig host links to drive the two-phase sandbox + capability layer + WASM/GPU + signed-artifact machinery via the stable `<hull/embed.h>` ABI. L-1 archive + `sandbox_tool.c` split; L-2 versioned `hl_embed_*` ABI; L-3 sealed per-call `base_dir` (RO `sh_seal_arena`) + fail-closed seal + fork/SIGSEGV death test; L-4 release-signed archive (native + dual-arch cosmo) + scoped SBOM (`hull sbom --subject=libhull`); L-5 Rust + Zig reference embedders (`examples/embed_{c,rust,zig}`) with CI jobs. Follow-ups: c-audit single-shot-seal fix, SBOM `keel`/`mbedtls` gated on `HL_ENABLE_HTTP`, `hull sbom --flavor=<flavor>` + per-flavor release SBOMs, `tool_orchestration.o` purge-list fix, and the CI runner-pin ([§0.3.1](#03-trust-chain-hardening-post-v015-gap-analysis) runner-pin step). Design + use-cases + trust boundary: [`libhull_flavor.md`](libhull_flavor.md); [`roadmap.md`](roadmap.md) row 8.

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

- [ ] **0.3.1. Pin the CI build environment.** **Runner-pin done;
  immutable base still open.** Every `runs-on:` across `ci.yml`,
  `release.yml`, `deploy-site.yml`, and `dco.yml` now pins a versioned
  label (`ubuntu-24.04`, `ubuntu-24.04-arm`, `macos-15`) — no more
  mutable `ubuntu-latest` / `macos-latest`. This removes the big
  reproducibility killer: a `latest` label silently jumping to the next
  major OS (24.04 -> 26.04, macOS 15 -> 16) between a release and a
  rebuild. **Remaining:** versioned labels still receive GitHub's monthly
  patch updates, so the image is not byte-immutable across time. Closing
  that needs a digest-pinned Docker base image (medium) or a Nix flake
  (high) for the reproducibility-critical build/release jobs; until then
  "reproducible" holds within a major-version window, not indefinitely.
  **Effort:** ~~low for runner-pin~~ (done); medium for Docker base; high
  for Nix flake.

- [x] **0.3.2. Bootstrap trust path. ✅ Done (minisign).** The
  `curl ... | sh` install was TOFU on top of TLS + GitHub account
  integrity + `install.sh` content trust, with no out-of-band way to
  verify the installer before running it. Now `install.sh` is
  minisign-signed and the signature + public key ship on gethull.dev
  next to the installer (`install.sh.minisig`, `minisign.pub`; key ID
  `595ED89D8DCEBD6A`, committed at the repo root and cross-checkable on
  GitHub, a different origin than the S3 site). The landing page shows
  both paths: the `curl | sh` trust path and an offline "verify the
  installer first" path (`minisign -Vm install.sh -P <pubkey>`) that needs
  no pre-installed hull. `deploy-site.yml` verify-guards the published
  signature against the published installer, so a forgotten re-sign fails
  the deploy. minisign was chosen over Sigstore for the bootstrap
  (offline-verifiable, no extra online infra required). **Remaining:** the
  installer signing key is currently a maintainer software key; moving it
  to a YubiKey follows the same custody-hardening path as 0.3.3(b).

- [ ] **0.3.3. Move release signing off GitHub Actions secrets.**
  **(a) done; (b)/(c) need hardware + org process.** The release
  private key currently lives encrypted in GitHub's KMS as a workflow
  secret. Compromise of the gethull GitHub org gives an attacker the
  ability to sign arbitrary artifacts as gethull. No HSM, no hardware
  token, no offline ceremony, no multi-party signing. **Fix progression:**
  (a) ✅ document the current posture honestly, see
  [`release_signing.md` "Release-key custody"](release_signing.md);
  (b) move to a YubiKey OpenPGP / Ed25519 token operated by a person, with
  CI just preparing the manifest for human-attested signing; (c)
  multi-party signing for major releases. **Effort:** (a) done,
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

- [x] **0.3.7. Cosmo APE in the reproducibility matrix. ✅ Done.** The
  `Reproducible build` job previously matrixed Linux + macOS only; the
  shipped `hull-cosmo` release asset was untested. Now the
  `reproducibility-cosmo` job builds `hull-cosmo` on two independent
  runners (each doing exactly one `platform-cosmo` build, since a second
  in one job corrupts the loader) and a `reproducibility-cosmo-compare`
  job byte-compares them. **Result: byte-identical across runners**
  (13,921,722-byte APE), so `cosmocc` + `apelink` are deterministic and
  the "Cosmopolitan produces deterministic output" claim in
  [security.md](security.md) is now CI-gated, not just asserted.
  All three shipped artifact types (native, macOS, cosmo) are now
  reproducibility-covered.

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

- [ ] **0.3.10. Key rotation and revocation procedure.** **Partial.**
  Written rotation playbook landed in v0.1.4 — `docs/security.md`
  §"Key rotation" covers scheduled rotation + post-compromise
  procedure for both `HL_PLATFORM_PUBKEY_HEX` and `HL_RELEASE_PUBKEY_HEX`,
  including GitHub-secret update + CHANGELOG note flow. **Remainder:**
  in-binary successor mechanism (hull accepts a successor pubkey
  announcement signed by the current pubkey) NOT shipped — current
  model is recovery-by-rebuild, not chain-of-trust progression.
  **Effort (remainder):** high for the in-binary successor mechanism.

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

- [x] §1.4-1. `hull/web/flash@1` (Lua + JS). **Landed (commit 43e460b).**
      One-shot user notifications. Classic POST/redirect/GET
      helper, applicable to any web app. `flash.set(req, text, kind?)`
      stashes in session; `flash.consume(req)` drains for the next
      render. HTMX bonus path: `flash.trigger(res, text, kind?)`
      emits `HX-Trigger: {"flash":{...}}` for fragment-swap paths
      that bypass the redirect. Template partial `partials/_flash.html`
      for default rendering. Unit tests cover both emission paths
      (test_flash.{lua,js}). Wired into `examples/hypermedia_todo`
      POST handlers; HTMX trigger path verified by both Lua + JS
      e2e tests asserting on the `HX-Trigger` header shape.
- [x] §1.4-2. `hull/web/pagination@1` (Lua + JS). **Landed (commit 0385c5b).**
      Server-side helper for `?page=N&per_page=M` paginated lists.
      Useful for REST JSON endpoints, server-rendered pages, and
      HTMX fragment swaps alike. `pagination.from_query(req, opts)`
      returns `{page, per_page, offset, limit}`; `pagination.render(total,
      opts)` returns `{total, pages, page, prev, next, links[]}`
      suitable for templates or JSON serialization. Partial
      `partials/_pagination.html` for HTML rendering. Scoped to
      offset-based pagination; cursor-based deferred. Demo:
      paginated todo list in `examples/hypermedia_todo`. Also
      fixed a pre-existing template scoping bug in the example
      surfaced during integration.

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

- [x] §1.5.a-1. `hull/htmx@1` helper module. **Landed v0.2.0 (commit
      6327efe).** Registered as `hull/web/htmx` after the §1.3 reorg.
      API: `is(req)`, `boosted(req)`, `redirect`, `retarget`, `reswap`,
      `trigger`, `location`, `push_url`, `refresh`. Lua + JS parity,
      unit tests.
- [x] §1.5.a-2. `hull/middleware/csp@1`. **Landed v0.2.0 (commit
      38cd508).** Registered as `hull/web/middleware/csp` after the §1.3
      reorg. `csp.htmx()` + `csp.strict()` factories; per-request nonce
      via `req.ctx.csp_nonce`. Tests cover both factories.
- [x] §1.5.a-3. Test helper extension. **Landed.** `cap/test.c:38,71-86`
      accepts `header_names`/`header_values`; example usage in
      `examples/hypermedia_todo/tests/test_app.{lua,js}` with
      `{ headers = { ["hx-request"] = "true" } }`.
- [x] §1.5.a-4. Vendor HTMX 2.x + Pico v2 classless. **Landed v0.2.0
      (commit 67c1688).** HTMX v2.0.9 + Pico v2.1.1, SHA-pinned via
      `make fetch-htmx` / `make fetch-pico`.
- [x] §1.5.a-5. **HTMX scaffolding as a profile.** **Landed v0.2.0
      (commit 17ed2d1).** `hull init --profile htmx` (Lua + JS) ships
      manifest cluster + vendored assets + layout template + handler
      pattern. **Note:** the broader `hull new --type X --profile Y`
      orthogonal-axis composition described below remained the planned
      design intent (see "Surfaced 2026-06 by the Trimble HU project"
      note); verify whether the `--type rest --profile htmx` matrix is
      actually wired or still pending before considering this fully
      done. Original spec preserved verbatim:

      **HTMX scaffolding as a profile, composable with every
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
- [x] §1.5.a-6. `examples/hypermedia_todo`. **Landed v0.2.0 (commit
      d30123f).** Both Lua + JS variants. Demonstrates full-page +
      fragment paths, optimistic row replacement, validation-error
      fragment, flash messages (now via `hull/web/flash@1`), CSRF
      on HTMX requests. Tests for both paths.
- [x] §1.5.a-7. `docs/htmx.md` pattern guide. **Landed v0.2.0 (commit
      d2d61a4).** Architectural pattern,
      response-header helpers, CSP nonce integration, CSRF on htmx
      requests, validation errors as fragments, flash messages
      (documented `hx-swap-oob` recipe; promoted to a stdlib module
      in §1.4-1), empty states, testing patterns.

### 1.5.b Streaming multipart + attachment storage  ✅ Shipped (v0.3.0)

> **Shipped 2026-06-15.** Iterator-shaped `req:multipart()` / `req.multipart()`,
> per-part + total caps surface as structured 4xx, `hull/attachment@1` +
> `hull/blob@1` + `hull/mime@1` content-addressed storage with refcount
> GC, photo-upload demo in `examples/hypermedia_photos`. See
> [`../CHANGELOG.md#030`](../CHANGELOG.md). Locked design decisions below
> preserved as historical record.

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

### 1.5.b-X. hull/blob@1 migrations  ✅ Shipped (v0.1.10, audit-closed v0.3.0)

> **Subsystem status: CLOSED.** Three audit cycles converged on
> "no further work." `commands/cache.c` graded A, runtime cache
> files graded A-, every other module unchanged at A. Adding a
> new cache kind is now one registry row + one ~100-LOC file +
> one Makefile entry — list / prune / clear / verify / doctor /
> inspect / opt-out env var all automatic. The next person who
> touches this subsystem should be adding a new kind, not
> refactoring the existing six. See [docs/cache.md](cache.md)
> for the operator reference and [docs/blob.md](blob.md) for
> the CAS-primitive design. Open audit items at close-of-cycle:
> two cosmetic Lows (compile_persist_run's `JS_WriteObject` OOM
> attributed to `JS_EvalFunction`; `verify_one_kind` docstring
> wording mismatch) — both explicitly recommended NOT to fix
> by the auditors (no behaviour change, would-be-DRY-for-DRY
> territory). Audit history: pass-1 closed real layering
> issues; pass-2 closed real polish gaps (commits A-E); pass-3
> closed minor smells (commits F-G); pass-4 (post-G) found
> nothing worth fixing.

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

- [x] §1.5.b-X-10. JS template cache (parity with X-4). **Landed.**
      `hl_js_template_compile_cached` wraps `JS_Eval(MODULE |
      COMPILE_ONLY)` for the template engine's render-function
      compilation step. Initial design tried caching the
      post-eval render closure via `JS_WriteObject` — QuickJS
      rejects serialization of runtime closures, so the cache
      stores the pre-eval *chunk* and uses `JS_EvalFunction` on
      hit (consumes the chunk reference, no leak). Cache key =
      `sha256(QJS_TAG || arch || endian || template_name ||
      generated_code)`. Auto-registered in the cache registry as
      `"js-templates"` so it picks up list/prune/clear/verify/
      doctor/inspect for free. Honors `HULL_NO_CACHE=1` and
      `HULL_NO_JS_TEMPLATE_CACHE=1`. Files:
      `include/hull/runtime/js_template_cache.h`,
      `src/hull/runtime/js/template_cache.c`,
      `src/hull/runtime/js/mod_template.c` (one-line swap),
      `src/hull/cache_registry.c` (+1 row), 4 utests in
      `test_js` (`js_template_cache.*`). Verified: 81/81 utest_js,
      40/40 e2e_templates, 76/76 e2e_cache.

- [x] §1.5.b-X-11. Concurrency stress test. **Landed.** New
      `tests/e2e_cache_concurrent.sh` (9 cases) and
      `make e2e-cache-concurrent` target. Spawns N=8 parallel
      hull processes hammering the same cache root, plus a mixed
      4-Lua + 4-JS workload, plus targeted prune/clear races.
      Asserts no crashes, no zero-sized blobs, no leftover
      `tmp/.blob-*.tmp` files, and idempotent results on warm
      replay. Kept out of the default `make e2e` chain (~30s,
      spawns ~16 hull instances); CI invokes explicitly.

- [x] §1.5.b-X-12. `--json` on prune and clear. **Landed.**
      Both `hull cache prune` and `hull cache clear` now accept
      `--json` and emit `{"results":[{kind, removed, freed_bytes,
      skipped_no_store?}], "total_removed":N, "total_freed_bytes":N,
      "dry_run":bool}`. `clear --json` still requires `--yes` to
      perform the wipe (no behavioural change — just adds a
      structured output format alongside the human one). Verified
      via 7 new e2e cases in `e2e_cache.sh`.

- [x] §1.5.b-X-13. `hull cache verify [--repair] [--json]`.
      **Landed.** Walks every cache entry and flags corruption.
      For CAS-mode kinds (registry `is_cas=1` — `tools`) it
      recomputes `sha256(contents)` and compares to filename
      (the strongest possible integrity check). For keyed-mode
      runtime caches it does a structural check (filename shape,
      regular file, non-empty, readable — a zero-byte runtime
      cache entry is always corrupt since none of the runtime
      kinds write empty blobs). `--repair` unlinks corrupt
      entries — safe because the next compile/install
      repopulates from source. `--json` emits the same shape as
      prune. Exits 1 if anything is corrupt and `--repair`
      wasn't able to fix it. New `is_cas` field on `HlCacheKind`
      drives the per-kind dispatch — adding a new cache kind
      automatically picks the right verification mode. Files:
      `src/hull/commands/cache.c` (+`cmd_verify`),
      `include/hull/cache_registry.h` (+is_cas field),
      `src/hull/cache_registry.c` (annotated every row), 10 new
      cases in `e2e_cache.sh`.

- [x] §1.5.b-X-14. `docs/cache.md` standalone reference.
      **Landed.** New `docs/cache.md` (~10 pages) covers
      everything an operator needs: what each kind caches and
      why, layout under `~/.hull/blobs/`, every `hull cache *`
      verb, the doctor + inspect panels, the full env-var table
      with truthiness rules, per-app isolation via
      `HULL_CACHE_DIR` with systemd/Docker/Kubernetes recipes,
      concurrency guarantees, when the cache helps vs hurts,
      what's explicitly NOT cached, internals one-paragraph-each,
      and a migration note for pre-X-2 layouts. `CLAUDE.md` and
      `docs/blob.md` updated to cross-link as the canonical
      operator reference; `docs/blob.md` stays the CAS-primitive
      design reference.

- [x] §1.5.b-X-15. Cosmo cache e2e. **Landed.** New
      `tests/e2e_cache_cosmo.sh` wrapper + `make e2e-cache-cosmo`
      target. The wrapper validates the cosmo APE and delegates
      to the shared `e2e_cache.sh` with `HULL=$(BUILDDIR)/hull`,
      so every existing cache assertion (list / prune / clear /
      verify / doctor / inspect / opt-outs / --json shapes /
      large-cache warnings) runs against the cosmocc binary
      without duplicating logic. Target gates on `cosmocc` being
      on PATH so the default CI path is unaffected; explicit
      invocation triggers a full `platform-cosmo` rebuild +
      fat-APE link + the suite. Also caught + fixed: under
      single-arch `make CC=x86_64-unknown-cosmo-cc`, `AR=` must
      be explicitly passed as `x86_64-unknown-cosmo-ar` because
      the keel build picks up macOS's BSD `ar` by default,
      producing archives that GNU ld.bfd can't resolve symbols
      from (matches the comment in `vendor/keel/Makefile` about
      fat-cosmo using cosmo's `ar`).

### 1.5.c HTMX-specific stdlib companions. Tier 1  ✅ Shipped (v0.1.10)

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

- [x] §1.5.c-1. Search + debounce snippet pattern. **Landed (commit 447fa77).**
      NOT a stdlib module — documented `hx-get` +
      `hx-trigger="keyup changed delay:300ms"` + ratelimit recipe
      in `docs/htmx.md` plus a `/search` route in
      `examples/hypermedia_todo` filtering todos by title. Tests
      for empty + matching + non-matching queries.
- [x] §1.5.c-2. Inline-edit pattern. **Landed (commit ce37af7).**
      Click row → swap to form → submit → swap back. Canonical
      `GET /todos/:id/edit` (form fragment) and `PATCH /todos/:id`
      (row fragment) in `examples/hypermedia_todo`. HTMX-only for
      the example (uses `hx-patch`); doc note in `docs/htmx.md`
      covers the Rails-style `POST /todos/:id?_method=PATCH`
      recipe for plain-form degradation. CSRF freshness handled
      via `max_age = 4 * 3600` recommendation + `csrf.refresh(req, res)`
      helper for long-running edit cycles.
- [x] §1.5.c-3. Loading-indicator scaffold. **Landed (commit 9caa49f).**
      Pico-compatible `.htmx-indicator` spinner block in scaffold
      `static/app.css`; `<div id="spinner" class="htmx-indicator">`
      in `templates/base.html`. Both `hx-indicator="#spinner"` and
      per-element spinners documented in `docs/htmx.md`.
- [x] §1.5.c-4. Form re-population on validation error. **Landed (commit 90c554e).**
      Submit → server validates → server returns same form fragment
      with submitted values pre-filled and per-field error messages
      inline. `partials/_form_field.html` in the scaffold bundles
      label + input + error + value-preservation; uses `hull.validate`
      output shape so handlers stay one-liner. `examples/hypermedia_todo`
      create form wired up. Doc recipe in `docs/htmx.md`.
- [x] §1.5.c-5. Wire `hull/middleware/idempotency@1` into the scaffold
      for POST/PATCH routes by default. **Landed (commit 77a4033).**
      Scaffolded `app.{lua,js}` calls `idempotency.init()` plus
      `app.use_post("POST"/"PATCH", "/*", idempotency.middleware({
      get_principal = ... }))` so HTMX double-clicks return the
      cached response instead of double-writing. Bonus: HTML
      response cache path added so HTMX fragment responses replay
      correctly, not just JSON. Doc note in `docs/htmx.md` § Idempotency.

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

- [x] **Asymmetric-signature verification ✅ Shipped (v0.3.0).** RS256/384/512,
      PS256, ES256/384 via mbedTLS, fronted by an `HlAsymBackend` vtable in
      `cap/crypto_asym_mbedtls.c`. Lua + JS expose `crypto.verify_asym(alg,
      pem, message, sig)`; `crypto.x509_pubkey_pem(cert)` extracts the SPKI
      PEM from x509 DER/PEM for JWKS `x5c` consumption. `hull/jwt`
      dispatches by header `alg` with allowlist enforcement BEFORE key
      resolution (alg-confusion defeated). Used by `hull/web/middleware/
      oauth@1` for OIDC ID-token verify.
- [x] **OAuth 2.0 / OIDC sign-in ✅ Shipped (v0.3.0).** `hull/web/middleware/
      oauth@1`. Authorization Code + PKCE flow. Presets for Google and
      Microsoft Entra (with `tenant=common`/`organizations`/`consumers`
      auto-pattern issuer match). HMAC-signed state cookie binds
      (provider, state, nonce, PKCE verifier, return_to). `on_login` /
      `on_logout` callbacks; account-linking left to the app. 13 audit
      rounds (rounds 5–13).
- [x] **Two-factor auth (TOTP) ✅ Shipped (v0.3.0).** `hull/web/middleware/
      totp@1`. RFC 6238. Dual-row staging (`_hull_totp_pending` vs
      `_hull_totp`) so re-enroll doesn't lock out a user who lost new
      recovery codes mid-flow. QR via the new pure-Lua/JS `hull/qrcode@1`.
      Multi-key at-rest encryption (`encryption_keys = {[1]=OLD, [2]=NEW}`)
      with lazy + batch rekey. Per-user brute-force lockout; per-IP
      lockout opt-in via `trust_xff`. Auto-daily pending-row prune.
- [x] **Account lockout ✅ Shipped (v0.3.0)** — folded into `auth-flows`
      (login lockout: `_hull_auth_login_attempts`, default 5 failures
      / 15 min window with proper window-expiry reset) and `totp`
      (per-user + opt-in per-IP). The standalone `auth_lockout@1`
      module was deliberately *not* shipped — folding into the
      modules that need the gate matched the round-8 design decision.
- [x] **Transactional email flow recipes ✅ Shipped (v0.3.0).**
      `hull/web/auth-flows@1` wires welcome / verify / password-reset
      / magic-link / email-change. Apps provide the templates via
      `templates = { welcome = fn(ctx) → {subject, html?, text?}, ... }`
      callbacks. Per-recipient rate limit gates the attacker-chosen-
      recipient email-storm class (round-8 HIGH-1).
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
- [x] **First-party app audit-log helper ✅ Shipped (v0.3.0).**
      `hull/web/middleware/audit-log@1`. Append-only sign-in / auth-event
      log with per-device fingerprint (HMAC-salted hash of UA + IP-prefix,
      deployment-private salt). Records `login_success`, `login_failure`,
      `password_reset_completed`, `email_changed`, `email_change_revoked`,
      and custom kinds. `audit_log.list_devices(user_id)` for the
      /devices UI. Auto-daily cleanup; `cleanup_status() ->
      "scheduled" | "external" | "missing"` tri-state probe. Migration
      helper `recompute_fingerprints()` for salt rotation (paged,
      max(id)-bounded, in-process mutex). The retention-policy /
      structured-export / tamper-evident-hash-chain version is still
      `[COMMERCIAL]` (above).
- [ ] **HTMX widgets tier** — promoted to its own sub-section: see
      **§1.5.g** below (renamed from the earlier "admin-UI
      primitives tier" framing, which was too narrow — these widgets
      serve any data-list-heavy hypermedia app, not just internal
      tools). Absorbs the umbrella conventions originally listed
      here (filter forms, optimistic row replacement, bulk-select
      toolbars, sortable headers) plus the scattered confirm /
      toast / form items in §1.5.d-8 / §1.5.d-10 / §1.5.d-11, and
      adds the platform-side plumbing they all depend on (stdlib-
      shipped static assets + template partials + CSP preset).
- [ ] Optimistic concurrency control for the lost-update problem
      (two users edit the same record; last write silently wins).
      Standard fix: `If-Match: <etag>` on PATCH, or a `version`
      column with `WHERE version = ?`. Helper `concurrency.guard(req,
      res, current_version)` that returns 412 Precondition Failed when
      stale. Doc note in `docs/htmx.md` about pairing with inline edit.
- [ ] Import/export workflow helpers: CSV preview, per-row validation
      errors, dry-run mode, commit step, background processing hooks for
      large imports.

### 1.5.g HTMX widgets tier (no target, designed)

**Naming note.** Earlier drafts called this "admin-UI primitives"
(matching §1.5.f's original bullet and the Trimble assessment's
wording). The frame turned out to be too narrow — the same
primitives are useful for any data-list-heavy hypermedia app
(customer dashboards, B2B portals, marketplace listings), not
just internal tools. Renamed to "HTMX widgets" so the tier names
itself by *what it is* (server-rendered htmx UI primitives) rather
than by *who's using it*. Module paths under `hull/web/htmx/...`
were already correct.

**Motivation.** §1.5.f's "reusable admin UI" bullet and the scattered
items in §1.5.d (d-8 styled confirm, d-10 toast renderer, d-11 form
drafts) cover the same problem from different angles: every
data-list-heavy hypermedia app rebuilds the same five-to-eight htmx
interaction patterns by hand. The Trimble HU asset-inventory build
plan (asset_inventory_assessment.md §5.1 item 5) was the forcing
function — its MVP 1 register page alone needs search + sort +
pagination + inline-edit + confirm + toast + form-errors. Promoting
the bullet to a named tier makes the surface a coherent first-party
offering rather than example boilerplate.

**Design constraints (non-negotiable):**

| Constraint | Why |
|---|---|
| No client framework | HTMX + ~150 lines of plain JS *total* across all modules. Stays in the hypermedia-only profile §1.5 already committed to. |
| CSP-friendly | No inline scripts, no `eval`/`Function`. Apps add `'self'` to script-src/style-src; nothing else. |
| Theming via CSS variables | Ship minimal default + `--hull-htmx-*` vars. Apps with no design system get a usable look immediately; apps with one rebrand by overriding vars. |
| Server-rendered partials | Server owns HTML shape; client JS is event-glue only. Matches `flash.trigger` / `htmx.compose` conventions in §1.5.c–d. |
| Audit-stack discipline carries over | `{{ }}` auto-escape, parameterized SQL, magic-byte validation where bytes flow in. Subject to the same parallel-reviewer audit cadence as the auth stack. |

**Tasks:**

- [ ] §1.5.g-0. **Phase 0 — platform plumbing (prereq, ~2 days of C/runtime work).**
  - [ ] Stdlib-shipped static assets. Today `static/*` is app-only;
        platform stdlib needs a second VFS prefix so modules can ship
        CSS/JS files that get auto-served. Convention:
        `static/hull/<module>/*` served at `/static/hull/<module>/*`.
        Apps can override by writing the same path under their own
        `static/`. Same MIME / ETag / 304 / cache-control machinery as
        the existing app `static/` middleware.
  - [ ] Stdlib-shipped template partials. Same shape: `templates/hull/
        <module>/*` resolvable via the template engine; app-side
        overrides win. Today's single-prefix template loader needs to
        gain a fallback chain (platform VFS → app VFS, with app
        winning on collision).
  - [ ] CSP preset table. `csp = "htmx"` in `app.manifest()` expands
        at manifest-extract time to a known-good policy for htmx-
        driven SSR apps with stdlib JS served from `/static/`.
        Unknown preset names pass through as literal policies (zero
        risk of typos silently becoming `default-src 'none'`).
  - [x] Response helpers for the HX-Trigger header variants:
        `htmx.trigger(res, event, payload?, opts?)` where
        `opts.timing = "swap" | "settle"` maps to `HX-Trigger-After-
        Swap` / `HX-Trigger-After-Settle`. Replaces the prior
        `trigger_after_swap` / `trigger_after_settle` methods (single
        canonical API, no half-finished alternatives). Lua + JS.
        Shipped as part of Phase 0.

- [ ] §1.5.g-1. **`hull/web/htmx/toast@1`.** Flash messages via
      `HX-Trigger` header → styled toast that auto-dismisses. Client
      JS (~40 lines) listens once at boot, renders + dismisses.
      Replaces the half-finished `flash.trigger` listener originally
      filed as §1.5.d-10; that item collapses into this one.
- [ ] §1.5.g-2. **`hull/web/htmx/confirm@1`.** Styled `<dialog>`
      replacement for `hx-confirm`'s native popup; a11y-clean,
      themable, intercepts `htmx:confirm`. Replaces §1.5.d-8.
- [ ] §1.5.g-3. **`hull/web/htmx/form@1`.** Field-level error
      rendering (consumes a `validate.check` result), loading state on
      submit button, error-target wiring (pairs with the
      `htmx-ext-response-targets` work in §1.5.d-1). Does *not*
      include form-draft autosave; that stays a separate concern in
      §1.5.d-11.
- [ ] §1.5.g-4. **`hull/web/htmx/search@1`.** Debounced search input
      (`hx-trigger="input changed delay:300ms"`) with a results
      partial. Server helper accepts a query + paginator; client side
      is markup-only.
- [ ] §1.5.g-5. **`hull/web/htmx/inline-edit@1`.** Click-to-edit
      single field. `inline_edit.cell(name, value, edit_url)` renders
      the editable cell; sibling endpoint accepts the PATCH and
      returns the new cell HTML. Pairs with the optimistic-concurrency
      `concurrency.guard()` helper in the §1.5.f open list.
- [ ] §1.5.g-6. **`hull/web/htmx/sort@1`.** Sortable column-header
      partial; parses `?sort=col[:asc|desc]` and renders the
      direction arrow.
- [ ] §1.5.g-7. **`hull/web/htmx/pagination@1`.** Server-paginated
      list footer (next / prev swap a list body). Parses
      `?page=N&per_page=M`; cursor or offset based on opts.
- [ ] §1.5.g-8. **`hull/web/htmx/table@1`.** Composed widget:
      `table.render(rows, schema)` where the schema declares which
      columns are sortable / searchable / inline-editable. Wires
      search + sort + pagination + inline-edit into one call.
- [ ] §1.5.g-9. **Example + docs.**
      - `examples/htmx_widgets_register/` — tiny employee/asset CRUD
        that exercises every module. Both runtimes.
      - `docs/htmx_widgets.md` — usage guide; cross-reference from
        `docs/htmx.md`.
- [ ] §1.5.g-10. **Audit pass.** Parallel-reviewer audit using a new
      `/htmx-widgets-audit` skill modeled on `/auth-audit`. Three
      slices (toast/confirm/form, search/inline-edit, sort/pagination
      /table). Converges when all three slices return zero findings.

**No aggregator module.** Earlier drafts proposed
`hull/web/admin-ui@1` as a one-line opt-in that pulled in all eight
widgets transitively. Dropped: eight granular declarations isn't
unreasonable, and explicit declaration keeps the manifest legible
("which widgets are actually in scope?"). The aggregator's
hide-the-detail benefit was outweighed by the manifest-as-audit-
surface cost.

**Sequencing & estimate:**

| Phase | Items | Estimate |
|---|---|---|
| 0 — platform plumbing | §1.5.g-0 | ~2 days |
| 1 — primitives | §1.5.g-1 → §1.5.g-5 | 2–3 weeks |
| 2 — composed widgets | §1.5.g-6 → §1.5.g-8 | 1–2 weeks |
| 3 — example + docs + audit | §1.5.g-9 → §1.5.g-10 | ~1 week |
| **Total** | | **~4–5 weeks** |

Only Phase 0 touches C / runtime; phases 1–3 are pure stdlib +
examples + docs.

**Open decisions (defer until §1.5.g enters a release):**

1. **Date-picker / combobox / file-drop:** in scope or separate
   tier? Recommendation: defer. Ship the eight widgets first; add
   `date-picker` and `combobox` only if a concrete consumer (Trimble
   MVP 5 stocktake) needs them. File-drop already has server-side
   coverage via the multipart iterator + `hull/attachment@1`; a
   small `htmx/upload` widget could land in a follow-up tier.
2. **Default CSS shipping:** minimal opinionated default + CSS
   vars (recommended), or pure structural CSS with zero
   appearance? Recommended option means apps get a working look
   with zero theming work; pure-structural means every consumer
   starts from zero. Decide before §1.5.g-1.
3. **Bulk-action toolbars** (originally listed in the §1.5.f
   bullet): bundle into §1.5.g-8 (`table@1`) as part of the
   schema, or separate `bulk-action@1` module? Lean toward
   bundling into `table@1` — the bulk-action selection state
   couples to the table row state and splitting them invites
   duplication.

---

## 1.6 Native sidecar services (`hull.services`)

**Priority:** High. Enables large native accelerators such as bitnet.c /
llama.cpp-style inference engines without embedding them into Hull's trusted
core and without pretending they are as safe as WASM plugins. Native sidecars
are lower-trust, out-of-process services with explicit capabilities, narrow
RPC, supervised lifecycle, and OS sandboxing where available.

**Status: Phase 0 complete.** Full design committed in
[`docs/sidecar_design.md`](sidecar_design.md). The summary below
is kept for the roadmap-level overview; load-bearing decisions
(trust model, model lifecycle, RPC framing, concurrency, two-layer
split) live in the design doc and shouldn't be re-litigated here.

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

**Phased plan** (revised against the accepted design in
[`docs/sidecar_design.md`](sidecar_design.md)):

- [x] **Phase 0:** design doc + threat model + ABI lock + accepted
      decisions on trust model (vendor pubkey + Sigstore hybrid),
      model lifecycle (sidecar owns; Hull passes a directory FD),
      RPC protocol (JSON-RPC + opt-in binary stream frames), and
      concurrency (one process per service; sidecar manages inflight
      and multi-model internally). See
      [`docs/sidecar_design.md`](sidecar_design.md).
- [ ] **Phase 1:** Hull-core: static `services` pre-extraction +
      supervisor + stdio-fd JSON-RPC + `rpc.discover` + `health` +
      sandbox stubs. `dev_exec` local-dev escape hatch.
- [ ] **Phase 2:** binary stream fast-path; resource-FD passing
      (`HULL_RESOURCE_*` env + inherited FDs); `hull/services@1`
      stdlib with `services.stream` iterator.
- [ ] **Phase 3:** full lifecycle supervision (readiness, restart
      policy, log capture, graceful shutdown); `hull agent services`.
- [ ] **Phase 4:** Linux seccomp + Landlock backend; macOS Seatbelt
      backend; OpenBSD/Cosmo pledge/unveil backend; Windows Job
      Object backend.
- [ ] **Phase 5:** tool resolver extensions for `vendor_pubkey` +
      `attestation_repo`; `hull tools install` verifies both paths.
- [ ] **Phase 6:** new repo `artalis-io/hull-sidecar-sdk`
      (independent). Documented ABI; Apache/MIT.
- [ ] **Phase 7:** new repo `artalis-io/bitnet-sidecar`
      (independent). First end-to-end demo: a Hull app summarizing
      asset history via bitnet on a Trimble dev VM.
- [ ] **Phase 8:** Hull-core hardening based on Phase 7 findings;
      parallel-reviewer audit pass; Sigstore attestation flow
      documented.
- [ ] **Phase 9** (deferred): GPU sandbox design;
      `artalis-io/llama-cpp-sidecar`; optional `hull/llm@1`
      ergonomic wrapper.

**Total Phase 0 → bitnet working end-to-end: ~10 weeks.**

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

- [x] `src/hull/platform_sig.{c,h}`. **Landed v0.1.3 (commit 88848da).**
      Manifest builder + signer + verifier + `extract_for_arch` helper.
      Unit tests including mismatch + tamper cases. **[C1]**
- [x] `release.yml` reorg: `build-platform` matrix +
      `sign-platform-manifest` job + dependency on
      `build-native`/`build-cosmo` jobs + bootstrap check on
      `HULL_PLATFORM_KEY` presence. **Landed v0.1.3 (commit 9284aea).**
      Generates `embedded_platform_sig.h` + `embedded_platform_hashes.h`. **[C2]**
- [x] `--no-verify-platform` flag on `hull build` + runtime.
      `tool.platform_sig_get()` Lua binding + `build.lua`
      integration writing `package.sig.platform`. **Landed v0.1.3.**
      Wired through `src/hull/serve.c:401`, `mod_tool.c:972`,
      `stdlib/cli/lua/hull/build.lua:929,1001`. **[C3]**
- [x] `hull verify` + `--verify-sig` runtime enforcement.
      **Landed v0.1.3 (commit 39602e1).** Hard-reject paths with the
      documented error messages; E2E test mutates embedded `.a` bytes
      and expects non-zero verify. **[C4]**
- [x] Restore real `HL_PLATFORM_PUBKEY_HEX`. **Landed v0.1.3 (commit
      77d68f4).** Reverts the v0.1.1 placeholder; updates
      `test_signature.c` for the `platform_kind` param;
      `e2e_build.sh` Step 14 adds `--no-verify-platform` for the
      developer-signed-app path. **[C5]**
- [x] Audit pass (mirror v0.1.2). **Landed v0.1.4 (commit 6dfcd47).**
      Fixes embed drift; adds verify-before-embed + E2E smoke; OOB
      defense + constant-time SHA-256 compare + fsync/close on the
      atomic writes audited.
- [x] Update `docs/security.md §6` flip "platform layer inactive"
      → "shipped". **Landed v0.1.3 (commit ca7c8ef).** Document now
      reads "platform layer split into two sub-layers in v0.1.3".
- [x] Update `site/index.html` honest-scorecard. **Landed v0.1.3.**
      Scorecard at `site/index.html:1439` reads "Signed platform-sig
      chain (v0.1.3)".
- [x] Update `site/verify.html` fixture with a real v0.1.3 example.
      **Landed v0.1.3 (bundled in ca7c8ef).**
- [x] Post-release smoke: extend `tests/release_smoke.sh`.
      **Landed v0.1.3.** Lines 145-184 run `sign-platform` +
      `build --sign` + verify dance against published artifacts.

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

## 8. Deep mbedTLS allocator seal: SHIPPED (client) / NOT VIABLE (server)

**Status:** Client-side deep seal shipped as Keel v2.5.0 on 2026-06-19.
Server-side deep seal investigated and ruled out due to per-sign RSA
blinding writes (documented below).  Filing the full discovery here
so the asymmetry is recorded.

### Goal (achieved for client; ruled out for server)

Extend Keel v2.3.3's "policy half seal" beyond the top-level
`mbedtls_ssl_config` / `mbedtls_x509_crt` / `mbedtls_pk_context`
structs to also cover the deep heap allocations those structs own
(cert chain link list nodes, `cert.raw` DER bytes, `cert.pk` bignum
limbs, `pkey.pk_info` function pointer table, `pkey.pk_ctx` private
key material, full CA chain).

**Client side (HTTPS-out / CA chain): SHIPPED in v2.5.0.**  CA chain
+ all deep allocations sealed mprotect-RO post-create.  Closes the
silent-MITM attack against `hull update`, OAuth, JWKS, http.fetch.

**Server side (own pkey): NOT VIABLE.**  See "Why server can't deep-
seal" below.  Stays on v2.3.3 shallow seal (top-level struct only).

### Shipped design (client side)

1. `MBEDTLS_PLATFORM_MEMORY` enabled in `vendor/mbedtls/hull_config.h`
   to unlock `mbedtls_platform_set_calloc_free()`.
2. Per-context arena alloc/free adapter in
   `vendor/keel/src/tls_mbedtls.c`:
   - Global `g_current_arena` pointer set during `_ctx_create` body,
     cleared after.
   - `kl_mbed_arena_calloc` bumps from `g_current_arena` (zeroed),
     falls through to heap `calloc` when not in the swap window.
   - `kl_mbed_arena_free` walks a registry of live sealed arenas and
     no-ops for any pointer in any registered arena range; else heap
     `free`.  Registry covers post-swap-window frees of arena-resident
     pointers.
3. **Pre-warm walker** (`kl_mbed_prewarm_chain`) drives mbedTLS's two
   verify-time lazy caches BEFORE seal:
   - **RSA**: `ctx->RN` (Montgomery R² mod N) — populated lazily by
     `mbedtls_mpi_exp_mod`.  Pre-warmed via `mbedtls_rsa_public(rsa,
     zeros, output)` per pubkey.
   - **ECP**: `grp->T` (precomputed comb table for generator G) —
     populated lazily by `mbedtls_ecp_mul` on first mul-by-G.  Pre-
     warmed via `mbedtls_ecp_mul(grp, R, 1, G, drbg)` per group.
4. Per-context flag `deep_sealed` on `KlMbedtlsCtx` so destroy can
   branch: deep-sealed ctxs skip `mbedtls_*_free` (arena reclaims
   wholesale); shallow-sealed (server) ctxs keep the v2.3.3 stack-
   local copy + `mbedtls_*_free` pattern.

### Validation that landed v2.5.0

- Keel `test_tls` 20/20 and `test_tls_integration` 3/3 PASS.
- Hull `make e2e-ca-bundle` 8/8 PASS — real HTTPS handshake to
  example.com against the macOS system store (128 CAs, 105 RSA + 23
  EC) AND against the Mozilla bundle (145 CAs, 101 RSA + 44 EC)
  with mbedTLS deep allocations sealed RO.
- Hull `make test` 46/46 and `make e2e` 22/22.

### Why server can't deep-seal

`vendor/mbedtls/library/rsa.c::rsa_prepare_blinding` (lines
1296-1304) rewrites `ctx->Vi` and `ctx->Vf` on **every** RSA sign
operation as a documented timing-attack defense (Kocher 1996).  The
"cached path" (line 1296) squares both blinding values in place via
`mbedtls_mpi_mul_mpi` → can grow the limb arrays → triggers a write
through `ctx->Vi.p` / `ctx->Vf.p`.

`Vi` and `Vf` live INSIDE the rsa_context, which lives inside the
sealed arena once we deep-seal.  No pre-warm helps — blinding has to
be fresh per signature.  Skipping blinding would be a security
regression (the original CVE driver).

Server-side own_pkey therefore stays heap-resident (v2.3.3 shallow
seal: the top-level `mbedtls_pk_context` struct is sealed inside the
arena, but the bignum limbs it points at remain heap-allocated and
mutable, which is exactly what blinding needs).

A future two-arena scheme (seal `conf` + `cert` + `ca_cert` in arena
A, keep `pkey` in arena B that stays mutable) could close this, but
requires routing mbedTLS allocations by "which struct does this
belong to" — not possible through the current
`mbedtls_platform_set_calloc_free` API.  Would need an upstream
patch to add per-call allocator context.  Not pursuing.

### Original spike write-up (kept for context)

The initial implementation (before the pre-warm walker) crashed
during HTTPS verify with:

```
EXC_BAD_ACCESS (code=2, address=0x105090660)
hull`mbedtls_mpi_exp_mod_optionally_safe + 472
  -> str q0, [x25]
```

The `str q0, [x25]` is the `*prec_RR = RR;` assignment at
`vendor/mbedtls/library/bignum.c::mbedtls_mpi_exp_mod_optionally_safe:1664`.
Once identified, pre-warming RR per pubkey (and `grp->T` per EC
group) eliminated the fault and unblocked the seal.

### Memory cost

Measured high-water for the client arena post pre-warm:

| CA bundle              | n_certs | RSA | EC  | Arena used |
|------------------------|---------|-----|-----|------------|
| macOS system store     | 128     | 105 |  23 | 19 MB      |
| Mozilla bundle (curl)  | 145     | 101 |  44 | 37 MB      |

EC pre-warm dominates (~600 KB per pubkey — comb table + scratch).
RSA is cheap (~25 KB per pubkey — RR cache).  Virtual address space
is allocated more generously (`ca_len * 300 + 32 MB`) but only
written pages are physically backed, so the 19 / 37 MB high-water is
the real RAM cost.

For Hull's typical HTTPS-out workload (one client TLS context loading
the system or vendored CA bundle), this is acceptable.  Server-only
deployments pay nothing additional.

### What was previously tried (now superseded)

1. Enabled `MBEDTLS_PLATFORM_MEMORY` in `vendor/mbedtls/hull_config.h`
   to unlock `mbedtls_platform_set_calloc_free()`.
2. Added a per-context arena alloc/free adapter in
   `vendor/keel/src/tls_mbedtls.c`:
   - Global `g_current_arena` pointer set during `_ctx_create` body,
     cleared after.
   - `kl_mbed_arena_calloc` bumps from `g_current_arena` (zeroed),
     falls through to heap `calloc` when not in the swap window.
   - `kl_mbed_arena_free` walks a registry of live sealed arenas and
     no-ops for any pointer in any registered arena range; else heap
     `free`.  Registry covers post-swap-window frees of arena-resident
     pointers.
3. Refactored both `kl_tls_mbedtls_ctx_create` (server) and
   `client_ctx_create_from_mem` (client) to install the hook, open
   the swap window, run the existing parse + `ssl_config_defaults`
   calls unchanged, close the window, seal the arena.  Heuristic
   sizing: `sizeof(KlMbedtlsCtxPolicy) + cert_len*4 + key_len*4 +
   ca_len*5/6 + 32-64 KB floor`.
4. Refactored `kl_tls_mbedtls_ctx_destroy` to skip the four
   `mbedtls_*_free` calls for the sealed structs (the arena owns the
   storage; `sh_seal_arena_destroy` reclaims wholesale).  DRBG and
   entropy still freed normally (heap-resident, not in arena).

### What worked

- Keel `test_tls` 20/20 and `test_tls_integration` 3/3 PASSED (real
  server-side TLS handshakes through the sealed cert + pkey).
- Hull HTTPS-out to `example.com` with the small vendored Mozilla
  bundle (`vendor/cacert/cacert.pem`, 226 KB / 145 CAs) PASSED.
- Arena sizing heuristic was accurate — actual high-water for the
  full system CA store was 553 KB used out of ~2 MB allocated.
- All heap-write death tests (the v2.4.0-style fault-on-write
  invariants) would have worked unchanged.

### What broke

`make e2e-ca-bundle` second case (default CA resolution loading
`/etc/ssl/cert.pem`, 333 KB / 128 CAs) crashed the server with
`Bus error: 10` during the first outbound HTTPS request.

LLDB capture at the fault:

```
Process 32846 stopped
* thread #1, queue = 'com.apple.main-thread',
  stop reason = EXC_BAD_ACCESS (code=2, address=0x105090660)
    frame #0: 0x00000001001b30e8
      hull`mbedtls_mpi_exp_mod_optionally_safe + 472
hull`mbedtls_mpi_exp_mod_optionally_safe:
->  0x1001b30e8 <+472>: str    q0, [x25]   ; 16-byte SIMD store
```

`code=2` + the `str q0, [x25]` SIMD store pattern + the call site
deep inside RSA modular exponentiation map to mbedTLS writing into a
bignum-windowed-precompute table that lives inside the sealed RSA
public key context.  Verified by recompiling with the seal disabled
(arena alloc/free hook still installed, all allocations still landing
in arena, just no `sh_seal_arena_seal` call): HTTPS handshake worked,
`status=200` returned.

### Root cause

mbedTLS's RSA verify path performs lazy precomputation inside the
caller-provided `mbedtls_rsa_context`.  Even though
`mbedtls_ssl_config` is documented as const-after-`mbedtls_ssl_setup`,
the underlying `pk_ctx` for cert pubkeys (the chain CAs and the leaf
cert public key) gets MUTATED on first use — Montgomery table
windows, blinding values, or related precompute state get written
into the bignum limb storage that lives in our sealed arena.

This is not a v2.3.3 problem because v2.3.3 only sealed the
TOP-LEVEL structs (the `mbedtls_pk_context` itself); the bignum
limb arrays under `pk_ctx` stayed heap-resident and freely mutable.
Deep sealing crosses the boundary that mbedTLS treats as mutable.

### Why test 1 (small Mozilla bundle) passed and test 2 failed

Best hypothesis without further instrumentation: the chain CA that
was actually MATCHED for `example.com` verification differs between
the two stores.  Mozilla curated bundle may have matched an ECDSA
intermediate first (no RSA precomputation needed); the macOS system
store may have matched an RSA-rooted chain first.  Either way the
failure isn't deterministic on bundle content — it's deterministic
on "does verify use RSA on a key inside the sealed arena".

### Path forward (the multi-week project)

Three approaches, in increasing order of work:

1. **Upstream patch to mbedTLS adding `mbedtls_rsa_complete_full()`
   that pre-warms all lazy caches** (Montgomery window tables,
   blinding pre-state, anything `mbedtls_mpi_exp_mod*` writes on
   first use).  Walk the cert chain post-parse, call it on every
   RSA pubkey, then seal.  Same for any ECDSA equivalent.  Estimated
   2-3 weeks: reading mbedtls bignum code, identifying every lazy
   write, adding the precompute API, upstream PR cycle.  If upstream
   declines, vendor the patch.

2. **Two-arena scheme** — separate "const" arena (cert.raw, asn1
   buffers, ssl_config) from "mutable" arena (pk_ctx bignum limbs).
   Routes allocations to one or the other based on call-stack
   classification.  Needs hooking deeper than
   `mbedtls_platform_set_calloc_free` allows — requires
   patching the mbedtls source to thread a "category" through every
   internal `mbedtls_calloc` call.  Estimated 4+ weeks; maintenance
   burden grows with every mbedtls upgrade.

3. **Manual cert-chain deep-copy** — walk every cert post-parse,
   find every `mbedtls_rsa_context` / `mbedtls_ecp_keypair`, swap
   their bignum storage out to heap-resident copies, leave only the
   cert.raw + asn1 buffers in the arena.  Fragile across mbedTLS
   minor versions; bignum struct layout changes between releases.

Approach 1 is the only sustainable path.  Not picking it up until
there's specific external pressure to close the residual risk.

### Empirical findings worth preserving

- **MBEDTLS_PLATFORM_MEMORY works as documented.**  The runtime swap
  via `mbedtls_platform_set_calloc_free` is sound; the registry-based
  free hook correctly distinguishes arena from heap allocations.
- **Arena sizing heuristic for the client path:**
  `ca_len * 6 + 32 KB + sizeof(KlMbedtlsCtxPolicy)` was within 30 %
  of actual high-water for the macOS system CA store.  Page rounding
  to a multiple of 16 KB on Darwin makes the over-provision
  negligible.
- **No crash from the arena hook or the destroy refactor** —
  the bug is purely the seal-vs-runtime-write conflict.  If
  approach 1 above ever lands, only the seal line needs to be
  re-enabled; the rest of the scaffolding is correct.
- **What's safe to seal today (v2.3.3 baseline)** without touching
  mbedtls internals: `KlMbedtlsCtxPolicy` itself (the four top-level
  struct copies + `is_server` + `has_ca` flags) plus the policy
  arena's tiny page of metadata.  That's what shipped.
- **What's NOT safe** without upstream changes: anything mbedTLS
  reaches through `.MBEDTLS_PRIVATE(pk)`, `.pk_ctx`, or any bignum-
  bearing struct.

---

## 9. `-fsanitize=cfi-icall`: shipped 2026-06-20, HlGpuBackend follow-up shipped 2026-06-21

**Status:** Shipped as opt-in `HL_ENABLE_CFI=1` after a two-pass
investigation.  The first pass (2026-06-19) framed CFI as needing
a 3-5 week vtable refactor across six interfaces and rolled back.
The second pass (2026-06-20) re-examined the framing, found that
five of those six vtables were already CFI-compatible via
opaque-forward typed struct pointers, and only `HlDbBackend`
needed a small typed-handle change.  Total Hull-side diff on
ship day: one method-signature change in `db_backend.h` +
matching updates in `db_sqlite.c` + nine call sites.  Plus a
Makefile flag block.

The sixth vtable, `HlGpuBackend`, was deferred at ship time
pending Metal validation and retyped 2026-06-21 (commit
10d77cb) once that became possible.  Same recipe applied: all
16 vtable methods now take `HlGpuCtx *ctx` / `HlGpuDevice *dev`
typed handles; the wgpu backend casts to its own concrete type
at each method's first line.  Validated with 18/18 `test_gpu`
cases under real Metal on Apple M1 Max (compile, dispatch,
persistent buffer round-trip, two-stage pipeline, texture
write/read), plus 48/48 unit + 22/22 e2e under live CFI on
Lima Ubuntu 25.04 aarch64.

All six Hull polymorphic vtables now use typed-handle method
signatures.  CFI sees a matching type-id at every Hull
dispatch site.

### Goal

Close the §4c documented residual: "a type-confusion bug that
lands a fake vtable POINTER inside an unsealed object is not
blocked."  CFI catches type-mismatched indirect calls at the
call site, a different defense layer from §4b's sealed-arena
mprotect-RO on the dispatch TABLES themselves.  Same ROP/JOP
escalation chain, two stages.

### What ships

**`HL_ENABLE_CFI=1`** Makefile flag.  Auto-enables
`HL_ENABLE_LTO=1` (CFI requires LTO bitcode).  Probe-and-skip
pattern same as the existing hardening block: on toolchains
that reject `-fsanitize=cfi-icall` (Apple clang, gcc, cosmocc),
the build proceeds without CFI and `make hardening` reports
`probe failed` with the platform reason.

**Vendor TU exclusions.**  Two vendor patterns can't be CFI-
checked without forking the vendor:

- **QuickJS**: `JS_NewCFunctionMagic((JSCFunctionMagic *)f, ...)`
  registers callbacks of disparate signatures (0/1/2-arg,
  magic, magic+ctor) through one generic prototype.
- **WAMR**: `NativeSymbol` entries register host imports with
  a generic typed-erased dispatcher.

Both vendor TU groups (`QJS_CFLAGS`, `WAMR_CFLAGS`) get
`-fsplit-lto-unit` only, no `-fsanitize=cfi-icall`.  They
still co-link with the CFI-on TUs cleanly because clang refuses
mixed CFI / non-CFI LTO links without the split-unit flag.

**Death test.**  `tests/hull/test_cfi.c` casts a
`void(const char*)` function through `void *` to `int(int)`,
forks, and asserts the child died with `SIGILL` / `SIGTRAP` /
`SIGABRT` (the trap signal varies by architecture).  Self-
skips cleanly on non-CFI builds via a `-DHL_CFI_BUILD=1`
compile-time define the Makefile sets after the probe succeeds.

**Coverage.**  ~85% of indirect call sites in the final binary
get CFI: every Hull TU (cap/*, commands/*, runtime/*, worker_*,
plus everything in src/hull), every cap-module-dispatched
vtable now CFI-typed, Keel (via the v2.6.1
`KEEL_EXTRA_CFLAGS` passthrough), mbedtls, sqlite, lua,
tweetnacl, miniz, log.c, sh_arena, sh_json, sh_seal_arena.

### Why the first-pass framing was wrong

The 2026-06-19 spike report said "Hull's architecture is built
on six polymorphic vtable interfaces, each dispatching through
`void *ctx` + typed function pointers."  Only the first half
was right.  Survey on the second pass:

| Vtable | First-pass framing | Actual shape |
|---|---|---|
| `HlDbBackend` | `void *ctx` erasure | Yes, `void *ctx` (refactored) |
| `HlAsyncBackend` | `void *ctx` erasure | No: `HlAsyncBackendCtx *` opaque-forward |
| `HlNetBackend` | `void *ctx` erasure | No: `HlNetBackendCtx *` opaque-forward |
| `HlCompilerVtable` | `void *ctx` erasure | No: `HlCompiler *c` typed |
| `HlRuntimeVtable` | `void *ctx` erasure | No: `HlRuntime *rt` typed |
| `HlGpuBackend` | `void *ctx` erasure | Yes, `void *backend_ctx` (retyped 2026-06-21) |

The opaque-forward `typedef struct HlFooCtx HlFooCtx;` pattern
gives CFI the typed parameter it needs without the implementer
having to expose internal struct layout.  Five of the six
vtables already used this idiom; `HlDbBackend` and
`HlGpuBackend` had the exposed `void *ctx` shape and were
retyped (HlDbBackend on ship day, HlGpuBackend the day after
once Metal validation became available).

Similarly, Keel's `KlBodyReader` vtable, which the first pass
listed as a separate Keel-side blocker, already takes
`KlBodyReader *self` in every method.  The actual trip was in
Hull's `cap/body.c` wrapper, which called into Keel's body-
reader callbacks; Keel was just compiled without CFI flags so
its registered functions had no type-id metadata for the
caller's CFI check to validate against.  Fixed by threading
the Hull-side CFI flags into Keel's sub-make via the existing
`KEEL_EXTRA_CFLAGS` mechanism (v2.6.1).

### What's actually deferred

**User-supplied callback contexts** (`HlRowCallback`,
`HlAsyncTimerFn`, `HlAsyncWorkFn`, etc.) intentionally
take `void *cb_ctx` because the caller owns the type.  Not
fixable in Hull without forcing callers into a typed-base-struct
API.  Stays as documented gap.

### Validation that landed

| Platform | Mode | Result |
|---|---|---|
| Apple clang 17 (macOS arm64) | default | 48/48 unit, 22/22 e2e, 8/8 ca-bundle |
| Apple clang 17 (macOS arm64) | `HL_ENABLE_LTO=1` | builds clean, byte-reproducible |
| Apple clang 17 (macOS arm64) | `HL_ENABLE_CFI=1` | probe-skips cleanly, builds + tests 48/48 |
| Linux clang 21 (Lima aarch64) | `HL_ENABLE_CFI=1` | **48/48 unit, 22/22 e2e, 8/8 ca-bundle** under release-mode CFI trap |
| Linux clang 21 (Lima aarch64) | `test_cfi` death test | **CFI traps wrong-typed indirect call** as designed |
| Apple M1 Max (Metal) | `HL_ENABLE_GPU=1`, default mode | **18/18 `test_gpu` cases pass** after HlGpuBackend typed-handle retype (compile, dispatch, persistent buffer round-trip, two-stage pipeline, texture write/read) |
| Linux clang 21 (Lima aarch64) | `HL_ENABLE_CFI=1`, post-HlGpuBackend-retype | **48/48 unit, 22/22 e2e** under live CFI; gpu.h typed signatures compile + link clean under split-LTO (Lima has no GPU adapter so runtime GPU dispatch validation stays on macOS) |

### Implementation notes

- `-fsplit-lto-unit` consistency flag required across ALL LTO
  TUs whenever CFI is on for any subset (mixed CFI / non-CFI
  LTO links otherwise fail with "inconsistent LTO Unit
  splitting").  Vendor TUs that opt out of CFI itself still
  need this flag.
- LTO bitcode archives need a bitcode-aware `ar` (default GNU
  ar only indexes ELF symbols → `libkeel.a`'s bitcode objects
  look unreachable to the linker).  Makefile probes for
  `llvm-ar` / `llvm-ar-N` and overrides `AR :=` when found.
  Apple's `ar` is already bitcode-aware via Xcode's libtool so
  macOS didn't need the probe.
- Trap-on-violation in release; recover-with-diagnostic in
  debug (`DEBUG=1`).  Release mode is what defends; debug mode
  lets developers see which call site CFI flagged.
- `clang -has_feature(cfi_icall)` was unreliable in testing
  (returned false under valid CFI builds).  Use the Makefile-
  set `-DHL_CFI_BUILD=1` compile-time define instead.

### Lessons preserved

- **The first pass under-surveyed the codebase.**  The
  "six vtables, all `void *ctx`" framing came from grepping
  for vtable structs without reading their actual signatures.
  The real survey took 15 minutes and revealed the actual
  scope was ~30 minutes of refactor work, not 3-5 weeks.
- **Recover-mode CFI is the diagnostic tool.**  When release-
  mode trap kills the test before any output, switch to
  `DEBUG=1` (which auto-selects `-fsanitize-recover=cfi
  -fno-sanitize-trap=cfi`) and grep for `runtime error:` in
  the test output to see exactly which line + which signature
  CFI flagged.
- **Keel compiled-with-CFI worked first try.**  Keel's
  vtables (`KlBodyReader`, `KlTls`) already use typed
  self-pointers.  Just needed CFI flags threaded through
  the existing `KEEL_EXTRA_CFLAGS` Makefile passthrough
  (added in v2.6.1).  No Keel source change required.
- **Defer-with-clear-validation-gate worked.**  `HlGpuBackend`
  was held back from ship day because the validation needed a
  Metal-equipped macOS box that wasn't available right then.
  The lesson is the gate, not the wait: deferring on "needs
  hardware we don't have right now" produced a clean one-day
  follow-up (commit 10d77cb) once Metal was available, vs. the
  open-ended risk of shipping unvalidated.  All 18 `test_gpu`
  cases pass under real Metal dispatch + 48/48 unit + 22/22 e2e
  under live CFI on Linux.

### File diff

Hull-side commit:
- `Makefile`: new `HL_ENABLE_CFI=1` block (~80 lines), Keel
  sub-make CFI passthrough, hardening summary CFI line.
- `scripts/check_hardening.sh`: CFI verifier entry (looks for
  `__ubsan_handle_cfi_check_fail` in debug builds; otherwise
  reports as skip-with-build-time-verify pointer).
- `tests/hull/test_cfi.c`: NEW.  Two tests: the death test and
  a smoke test that logs CFI state for CI output.
- `include/hull/cap/db_backend.h`: vtable methods take
  `HlDbHandle *h` instead of `void *ctx`.  Inline wrappers
  updated to pass `h` not `h->ctx`.
- `src/hull/cap/db_sqlite.c`: every method's first line casts
  `h->ctx` to `HlDbSqliteCtx *` instead of casting the raw
  `ctx` parameter.
- `src/hull/app_context.c`, `src/hull/commands/migrate.c`,
  `src/hull/tool_orchestration.c`: direct vtable call sites
  pass `&handle` instead of `handle.ctx`.
- `tests/hull/cap/test_db_backend.c`,
  `tests/hull/runtime/js/test_js.c`,
  `tests/hull/runtime/lua/test_lua.c`: same.

Keel v2.6.2:
- `vendor/keel/src/connection.c`: `h2_preface` as `unsigned
  char[24]` (orthogonal clang-21 fix surfaced during Linux
  validation; not a CFI change).

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
