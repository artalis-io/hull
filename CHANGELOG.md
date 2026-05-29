# Changelog

All notable changes to Hull are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); Hull adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html) on the
public surface (`hull` CLI flags, embedded stdlib API, manifest schema,
release-artifact layout).

## [Unreleased]

### Added

- **Byte-reproducible builds, three layers, CI-gated.** Same source tree → byte-identical `build/hull` between rebuilds. Same source + same hull version → byte-identical app binary from `hull build`. Plus `make self-build` proves hull is self-hostable across all platforms. New CI job `reproducibility` enforces all three on every commit. Mechanism: deterministic ar archives via `ZERO_AR_DATE=1`; `-ffile-prefix-map=<srcdir>=.` in `sys_compile` to strip per-build tempdir paths from .o file content; same-path methodology for the `hull build` test (macOS `ld64` hashes output path into LC_UUID, so end-user same-target-name behavior is what's tested). Documented in `docs/MANIFESTO.md` "Reproducible builds" and `docs/POSITIONING.md` precision notes; full investigation arc preserved in `docs/roadmap_next.md §0.2` as lessons-learned.
- **`docs/POSITIONING.md` (new, 153 lines).** Standalone messaging-style guide encoding the canonical thesis, descriptor, target audiences, tone, vocabulary table, geographic positioning, typography rules, surface-update cadence, and authoring checklist. Lives separately from `MANIFESTO.md` (which is narrative WHY); POSITIONING is operational reference for anyone writing about Hull (humans or AI agents).
- **New canonical thesis surfaced across every Hull-mentioning surface:** *"Code became disposable. Trust is not."* Hero on gethull.dev, README opener, MANIFESTO callout, INVESTORS callout, GitHub repo About. Canonical descriptor unified to *"Hardened, capability-secure runtime infrastructure for AI-native systems."*
- **Self-hosted site assets.** gethull.dev no longer fetches from third-party CDNs. Tailwind, Lucide, Inter + JetBrains Mono fonts all vendored under `site/vendor/` and `site/fonts/`. Verify.html CSP tightened to `default-src 'none'` with `'self'` for everything except `connect-src https:` (for the dev-key fetch path).
- **OG card PNG variant.** `site/og-card.png` (1200×630, rendered from the SVG via `rsvg-convert`) added so LinkedIn and Facebook social previews render correctly (those platforms don't render SVG OG images).
- **OG metadata on `verify.html`.** Previously had only `<title>` + `<meta name="description">`; social shares of the verifier URL showed blank/scraped previews. Now has full `og:*` + `twitter:*` block with the canonical thesis.
- **`Engineered in Europe for sovereign, deploy-anywhere AI infrastructure.`** Imprint-style line added to site footer and README license area. Restrained provenance, no flag.
- **LICENSING.md vendored-dependency table.** Every static-linked third-party library listed with license (mostly MIT/BSD/Apache/ISC/PD). Explicit TinyCC note: LGPL-2.1+ is the only non-permissive dependency; compliance with §6 is satisfied by Hull shipping full vendored source under AGPL.

### Changed

- **README, MANIFESTO, AGENTS, INVESTORS positioning aligned** to the new canonical thesis. README's lead tagline + Why section rewritten. MANIFESTO title vocabulary updated (`Agent-Native, Local-First Applications` → `Hardened, Capability-Secure Runtime for AI-Native Systems`). AGENTS.md opening line uses canonical descriptor. INVESTORS adds thesis callout at top and refreshes Status section (was stuck at "approaching v0.1.0" — now correctly states v0.1.4 shipped 2026-05-28 with detailed shipped-features list). All `agent-native` → `AI-native`, `secure runtime` → `hardened, capability-secure runtime`. "Zero system dependencies" wording corrected: embedded TinyCC removes the system-compiler requirement, but the system linker is still used.
- **`docs/release_signing.md` status line** updated from `Design draft — pre-v0.1.0` to `Shipped (as of v0.1.3). The embedded gethull release key is live; every release manifest carries an Ed25519 signature verified by hull verify-release and by hull update before atomic install.`
- **Em-dash sweep across all prose files.** 1100+ em-dashes replaced with period + capitalize for between-clause breaks, parentheses for parentheticals, colons for elaborations. Same pattern as commit 6b6f394 precedent. Applies to README, AGENTS, CLAUDE, MANIFESTO, INVESTORS, PERSONAS, POSITIONING, all `docs/*.md`, and site HTML. Two UI-placeholder em-dashes in `site/verify.html` status badges became middle dots (`·`).
- **Deploy-site workflow CF-invalidation bug fixed.** Conditional was `if: ${{ env.CF_DISTRIBUTION_ID != '' }}` but `env:` was set in the same step (applied after `if:`), so the invalidation step never ran. Promoted to job-level env; now fires correctly when the secret is configured.
- **Documentation archive cleanup.** Five superseded docs moved to `docs/archive/`: three Phase-6 audits, `ASSESSMENT.md` ("approaching v0.1.0" snapshot), `api_review.md` (pre-v0.1.0 public-surface review). Archive index (`docs/archive/README.md`) updated with new entries and reorganized sections.

### Fixed

- **macOS sandbox claim precision.** Hero/site tech-stack chip clarified to `pledge + unveil (Linux/OpenBSD/cosmo) · Seatbelt (macOS)` so the platform split is visible without restructuring copy. Aligns the marketing claim with the truth disclosed deeper in the "Hull is not" section.

## [0.1.4] — 2026-05-28

§3.1, §3.2, §3.3 from `docs/roadmap_next.md`. Closes the v0.1.4 batch from the platform-sign-chain follow-ups roadmap.

### Added

- **§3.1 — Cosmo APE `hull build` works on Linux.**
  - Tool-mode sandbox unveils widened with `/opt` + `$HOME/.cosmocc` + `$HOME/cosmocc` so the cosmocc toolchain's install locations aren't sealed off (`src/hull/sandbox.c`).
  - `hl_compiler_select()` auto-detects cosmocc when running as a cosmo APE: checks `$HOME/.cosmocc/bin/cosmocc`, `$HOME/cosmocc/bin/cosmocc`, `/opt/cosmo/bin/cosmocc`, then `$PATH`. No need to pass `--compiler cosmocc` explicitly (`src/hull/compiler.c`).
  - Makefile `WAMR_INVOKE_SRC` selection reordered: target-compiler checks (cosmocc / `*-unknown-cosmo-cc`) precede `UNAME_S=Darwin` so cross-building a cosmo APE from a macOS host picks the correct asm invoker (was previously failing with "missing elf symbol table").
  - Known limitation: cosmo platform-sig E2E smoke test in `release.yml` stays SKIPPED. The jart pledge polyfill on Linux rejects child `mmap(PROT_EXEC)` independently of unveil policy, so spawning `/bin/sh` or `/usr/bin/cc` from inside the polyfill always fails on Linux runners. Native build jobs (darwin-arm64, linux-x86_64, linux-aarch64) gate the gethull signature chain end-to-end.

- **§3.2 — `hull eject` and `hull sign-platform` work on installed binaries.**
  - Both commands auto-extract the embedded `libhull_platform.a` to a tempdir when no `build/` tree is present. End-users who only have the binary can now eject or sign-platform without first cloning the repo (`stdlib/cli/lua/hull/{eject,sign_platform,build}.lua`).
  - New Lua bindings: `tool.extract_platform()` (single-arch) and `tool.extract_platform_cosmo()` (multi-arch).

- **§3.3 — Platform-sign chain polish (four follow-ups).**
  - Variant A (`--no-sandbox`) + Variant B (with sandbox active) for the native platform-sig E2E smoke test in `release.yml`. Variant A is the primary gate; Variant B re-runs the same `myapp` with the full pledge/unveil sandbox active and confirms `--verify-sig` still works under it.
  - Key-rotation story added as a new section in `docs/security.md` §2. Covers scheduled rotation, post-compromise rotation, the non-cross-validity property (intentional), and the impersonation gate.
  - `hull verify --gethull-key <file>` parity in Lua (`stdlib/cli/lua/hull/verify.lua`). CROSS-CHECK semantics: the file's pubkey must match this hull's embedded `HL_PLATFORM_PUBKEY_HEX` AND the signature must verify against it. Documented asymmetry with `verify.js` (JS uses override semantics because it can't reach the embedded key from QuickJS).
  - `tests/release_smoke.sh` extended with platform-sig E2E section. Post-publish on the actually-uploaded artifact, runs sign-platform + build --sign + --verify-sig. Catches CDN-tampering / upload-integrity surprises the in-CI smoke can't see.

### Fixed

- **Cosmo platform-sig E2E re-disabled in `release.yml`.** Round-1 audit attempt re-enabled it under the §3.1 sandbox/compiler fixes; CI demonstrated the deeper polyfill mmap restriction (`/bin/sh: libc.so.6: failed to map segment from shared object`) is independent of unveil policy. Re-skipped with honest comment naming the actual mmap-vs-unveil distinction. Native build jobs continue to gate the gethull signature chain.

## [0.1.3] — 2026-05-27

> The first published v0.1.3 build was found to ship with a broken
> gethull layer (the `libhull_platform.a` embedded into the hull
> binary had different bytes than the .a `sign-platform-manifest`
> hashed, so `hull build --sign` against that binary failed the
> cross-check). The original tag + GitHub release were deleted and
> v0.1.3 was retagged with the release-process fix described under
> "Fixed — release process" below. End-users who installed the
> first v0.1.3 should `hull update` to pick up the corrected build.

### Added

- **Platform-sig chain end-to-end (v0.1.3).** The
  `HL_PLATFORM_PUBKEY_HEX` placeholder is replaced with the real
  gethull.dev platform Ed25519 public key
  (`2a5461235aa51bbbe1e9cbc462e6a63f37d099f5ad17646a8f3a67db2f3a4fad`),
  and the full release → build → verify chain is wired:
  - **Release CI:** `release.yml` reorg splits platform building
    into a matrix, then a single Linux job
    (`sign-platform-manifest`) downloads the per-arch `.a`
    artifacts, computes SHA-256s, signs the canonical text
    manifest (`<64-hex>  <arch>\n`, LC_ALL=C sorted by arch) with
    `HULL_PLATFORM_KEY`, and emits
    `build/embedded_platform_sig.h` for the downstream
    `build-native` / `build-cosmo` jobs to embed.
  - **`hull build`:** computes SHA-256 of the
    `libhull_platform.a` being embedded, cross-checks against the
    inherited signed manifest, hard-rejects on mismatch unless
    `--no-verify-platform` is set. Writes the inherited
    `{manifest, signature, arch_hashes}` into
    `package.sig.platform.gethull`.
  - **`--verify-sig` runtime:** verifies the gethull manifest
    signature against `HL_PLATFORM_PUBKEY_HEX`; hard-rejects on
    missing block unless `--no-verify-platform`. Apps built by
    dev hulls (placeholder pubkey) skip the check silently with
    a one-line warning — same bootstrap shape as `hull update`'s
    release-pubkey placeholder.
  - **`hull verify` (CLI):** matching `--no-verify-platform`
    flag, new `tool.platform_pubkey()` Lua binding exposing the
    embedded pubkey, gethull-layer block in the report.
  - **Browser verifier (`site/verify.html`):**
    `verifyGethullLayer()` checks `platform.gethull.signature`
    against the hardcoded gethull pubkey; renders the layer
    explicitly. Per-app platform layer is now self-consistency
    only (the canary scanner is gone — the signed per-arch
    SHA-256 does all the integrity work it was hypothesized for).
- **`--no-verify-platform`** flag on both `hull build` and the
  runtime serve path. Documented escape valve for dev-built hulls
  and forks signing with their own key.
- **`tool.platform_pubkey()`** Lua tool-mode binding — returns the
  embedded `HL_PLATFORM_PUBKEY_HEX` or `nil` when the all-zeros
  placeholder is active.
- **`hull sign-release` / `hull verify-release`** still cover the
  release-binary path; the new gethull layer is signed by a
  separate key (`HULL_PLATFORM_KEY`) so a release-key compromise
  cannot forge platform-sigs and vice-versa.

### Changed

- **`signature.c` §5 reduced to self-consistency.** The v0.1.2
  per-app platform layer no longer pins
  `platform.public_key_hex` against `HL_PLATFORM_PUBKEY_HEX` —
  that role moved to the new §5b gethull layer. Forks signing
  platform with their own developer key continue to work without
  conflicting with the upstream gethull pubkey. `hull verify`
  matches the same shape; the
  `Platform layer: WARNING — key mismatch` path becomes a soft
  comparison against `--platform-key` only.
- **Docs/security.md §2 and §6** rewritten to reflect the
  three-layer structure (gethull, per-app, app developer) and
  the v0.1.3 status flip from "wired but inactive" to "shipped
  and enforced." Section 3.A and 3.B replace the canary-based
  prevention story with the manifest-signature prevention story.
- **Honest scorecard on `gethull.dev`** moves the platform-sig
  bullet from "Not yet" to "Ships," and replaces the old gap
  text with an explicit out-of-scope note for post-install
  binary integrity (OS layer's job).

### Removed

- **Platform canary scanner** in the browser verifier. The
  canary was the placeholder integrity signal while the
  signed-manifest path was missing; v0.1.3's per-arch SHA-256s
  cover the same property without Makefile post-link gymnastics.

### Fixed — release process

- **Platform-sig embed drift in `release.yml`.** `make EMBED_PLATFORM=1`
  was rebuilding `libhull_platform.a` from source during the
  `build-native` / `build-cosmo` jobs, replacing the downloaded
  artifact (the one `sign-platform-manifest` had hashed) with a
  freshly-built .a that produces different bytes on a different
  GH Actions VM. Result: the hull binary embedded one set of bytes
  and a manifest signature pointing at a different set, breaking
  the gethull cross-check for every app built with v0.1.3.
  - Fixed by switching to `make -o build/libhull_platform.a` —
    GNU make's `--assume-old` flag tells make to treat the .a as
    pristine and never rebuild it.
- **Verify-before-embed step** in every build job: sha256sum the
  downloaded .a, compare against its line in the signed manifest,
  fail loudly on mismatch. Defense in depth if the rebuild guard
  ever regresses.
- **Platform-sig E2E smoke test inside CI** (`release.yml`): each
  `build-native` / `build-cosmo` job now does `hull keygen` +
  `hull sign-platform` + `hull build --sign` + run with
  `--verify-sig` on the freshly-baked hull binary. Both signature
  layers (per-app v0.1.2 + gethull v0.1.3) are exercised end-to-end
  before the artifact is uploaded. Tests `tests/release_smoke.sh`
  still cover the release-sig path (post-publish, manual); the new
  in-CI test covers the gap that let v0.1.3-rc ship broken.

### Fixed

CI matrix repairs — four jobs that had been red since v0.1.2 went
green during the v0.1.3 stabilization pass:

- **Cosmopolitan (APE)** — test fixtures used `system("rm -rf ...")`
  for cleanup, which Cosmo's toybox `rm` rejects (the `-r` flag fails
  in any combined form: `-r`, `-rf`, `-fr`, `-Rf`). The failed cleanup
  left tmpdirs intact and the next `mkdir` hit `EEXIST`, cascading-
  failing the rest of the suite. Replaced with in-process
  `nftw(FTW_DEPTH | FTW_PHYS)` recursive delete — POSIX, works on
  Linux/macOS/cosmo uniformly, no flag-parsing dependency. Gated the
  `_XOPEN_SOURCE` define on `__linux__` so the change doesn't hide
  Darwin extensions on macOS.
- **Static Analysis (scan-build + cppcheck)** — scan-build flagged
  `agent/compute.c`'s `const char *platform = "unknown"` as a
  dead-store on Linux x86_64 because subsequent `#ifdef` branches
  reassigned it. Restructured as one `#if/#elif/#else` chain so
  exactly one assignment runs per build. cppcheck flagged four false
  positives — `agent/overview.c` `knownArgument`, three
  `commands/tools.c` `knownConditionTrueFalse` on version-string
  prefix-stripping — added suppressions for the defensive code
  cppcheck couldn't model.
- **Code Coverage** — `test_lua`'s link line referenced `$(PLEDGE_OBJS)`,
  `$(BUILDDIR)/tool.o`, `$(BUILDDIR)/sandbox.o`, and several other
  objects without listing them as prerequisites. Serial builds picked
  them up via earlier test binaries' chains; the coverage and ASan
  parallel builds didn't. Added explicit prereqs so make schedules
  them unconditionally.
- **MSan + UBSan** — vendor `*_CFLAGS` carried
  `-fsanitize=memory,undefined`; UBSan flagged well-known
  "technically UB but works on every target" patterns in
  `tweetnacl` (left shift of negative values in field arithmetic)
  and `quickjs` (function-pointer casts in `cutils`, signed-overflow
  left shifts). Dropped `,undefined` from QJS/LUA/SQLITE/LOG/
  SH_ARENA/SH_JSON/TWEETNACL/STB CFLAGS so vendor TUs get MSan
  shadow tracking (the actual point of the job) but not UBSan.
  Added MSan instrumentation to `MBEDTLS_CFLAGS` so MSan tracks
  mbedtls writes to caller buffers (e.g. `mbedtls_sha256` →
  caller-provided `digest[32]`); without it MSan reported every
  subsequent read as use-of-uninitialized. Hull's own CFLAGS still
  carry `-fsanitize=memory,undefined`, so Hull-code UBSan errors
  still fail the build loudly.

## [0.1.2] — 2026-05-26

Two themes: (1) side-loaded Hull-native tools (`hull tools install`),
opening the door for optional executables like `wamrc` without bloating
the main binary; (2) a complete agent-onboarding surface so AI coding
agents can bootstrap against a fresh hull install without scraping
docs from elsewhere.

### Added

- **`hull tools install <name> [--all]` / `tools list [--json]` /
  `tools uninstall <name>`** — Side-load optional tools (currently
  `wamrc`, the WAMR AOT compiler) from GitHub releases into
  `$HOME/.hull/tools/`. Reuses the same Ed25519-signed `hull.sha256`
  manifest that protects `hull update` — no new keys, no new
  ceremonies. Version-coupled: pulls from the SAME release as the
  running hull binary so e.g. wamrc stays at the WAMR commit hull was
  compiled against. Cosmo unsupported for tools that need LLVM. Full
  design: [`docs/tools_install.md`](docs/tools_install.md).
- **CI `build-wamrc` matrix** (linux-x86_64, linux-aarch64,
  darwin-arm64) in the release workflow — produces signed
  `hull-wamrc-<platform>` assets listed in the same `hull.sha256`
  manifest as the hull binaries.
- **`tool.find_tool(name)` Lua binding** (build-tool VM) — generic
  4-step lookup (`~/.hull/tools/` → sibling-of-hull → `$PATH`).
  Replaces the inline `find_wamrc()` heuristic in `build.lua`.
- **`hull --help` / `hull help` / `hull -h`** — Top-level usage
  printer grouping every registered subcommand by purpose. Suppresses
  build-time-gated commands when their `HL_ENABLE_*` flag is off so
  help only advertises what this binary can do. Symmetric with
  `--version`/`-v`/`version`.
- **`hull agent tools`** — Generic JSON dump of the tool registry
  crossed with the install state on this host. Independent of
  `HL_ENABLE_HTTP_CLIENT` — CLI-flavor builds without the installer
  can still enumerate what's registered.
- **`hull agent compute` gains a `wamrc` block** — installed/path/
  managed/available_for_platform/install_hint, so agents seeing
  modules with no AOT artifacts can recommend
  `hull tools install wamrc` in one call.
- **`hull agent context --list`** — Enumerate every embedded topic
  doc plus which level markers (minimal/compact/full) each populates.
  Cold-start agents call this first to discover the topic registry.
- **`hull agent overview [app_dir]`** — Single-shot composite
  summary (runtime, routes, compute, gpu, migrations, declared
  modules, tests, build_ready). One call to orient an agent dropped
  into an unfamiliar tree.
- **Six new context docs** in `stdlib/context/`: `orientation.md`
  (the AI-agent cold-start primer), `quickstart-{web,cli,tui}.md`
  (opinionated bootstraps per app shape), `gpu.md` (WGSL shaders +
  GPU vs WASM AOT crossover guidance), `tools.md` (`hull tools
  install` from the app-dev perspective). All three levels.
- **Discoverability breadcrumbs** in `hull --help`, bare-`hull`
  usage, and `install.sh` postscript — every entry point now points
  at `hull agent context --task=orientation --level=minimal` so an
  agent can bootstrap by reading any one of them.
- **Shared `release_io.{c,h}` helpers** extracted from
  `commands/update.c` — HTTPS GET, JSON-string extraction, SHA-256
  hex, manifest-line lookup, atomic install. Used by both `hull
  update` and `hull tools install` so the trust chain is implemented
  once.
- **`tests/release_smoke.sh`** — Post-`gh release create` smoke
  script that exercises the live install path: download wamrc from
  the just-published release, verify SHA-256, run `wamrc --help`,
  uninstall. The only end-to-end test of the live install codepath
  (the rest is covered by unit + e2e suites).
- **Shell completions** (bash, zsh, fish) for the new surface:
  `tools list/install/uninstall`, `agent overview`, `agent context
  --list`, all 19 context tasks.

### Fixed

- **`hl_release_io_find_checksum` OOB-read defense** — Reordered the
  exact-match guard's OR chain so the bounds check runs before the
  byte dereference. Dormant in practice (Keel allocates body+1 and
  writes a trailing NUL), but the helper is now safe regardless of
  caller. Surfaced by the post-implementation audit.
- **`atomic_write` checks `fsync` + `close` return values** — Aborts
  + unlinks the `.new` sidecar on either failure. Prevents a
  power-loss window where rename could happen over a half-flushed
  file (ENOSPC, EIO on network mounts).
- **SHA-256 hex compare is now constant-time** in both `hull update`
  and `hull tools install` paths (via `mbedtls_ct_memcmp`). The
  compared values are public, so the practical timing leak is nil,
  but the codebase now uniformly uses constant-time comparison for
  hash equality.
- **JSON escaper for `hull tools list --json`** — Tool descriptions
  now flow through a proper RFC 8259 escaper. Today's static
  registry is clean (no embedded quotes/backslashes), but future
  tool descriptions can't accidentally produce malformed JSON.
- **Makefile xxd hyphen handling** — The stdlib registry generator's
  sed expression now translates hyphens to underscores in symbol
  names, matching xxd's own normalization. Without this, context
  docs with hyphens in their filenames (`quickstart-web.md`) would
  fail to link.

### Changed

- **`build.lua` wamrc lookup simplified** — Replaces the inline
  `find_wamrc()` (PATH → `./build/wamrc` → sibling-of-hull) with a
  call to `tool.find_tool("wamrc")` plus a single `./build/wamrc`
  dev fallback. Same effective lookup order, implemented once in C.
- **`hull doctor` wamrc row** — Reports `installed` /
  `managed via hull tools` / `not installed` states. Hint text now
  recommends `hull tools install wamrc` (signed download) over
  `make wamrc` (build from source) as the first option.
- **`hull agent context` bare-command error** — Now points users at
  `--list` instead of hard-coding a stale topic enumeration.
- **`hull update` refactored to use `release_io`** — Same external
  behavior; the HTTPS + SHA-256 + manifest plumbing it relied on is
  now shared with `hull tools install`. Net diff: 252 lines deleted
  from `update.c`, same lines added to `release_io.{c,h}` plus
  reused in `tools.c`.

## [0.1.1] — 2026-05-25

Patch release. Two reproducible v0.1.0 bugs that broke first-time
user experience on Linux:

### Fixed

- **`hull init` scaffolded app crashed at load.** The generated
  `app.lua` / `app.js` declared `modules = ["hull/time@1"]` and
  imported `hull.log` + `hull.time`, but never declared
  `hull/http-server@1`. The runtime gate refused `app.get` (which
  the http-server module installs on the `app` intrinsic) and
  emitted "module 'hull/log' was imported at top-of-file but is not
  declared". Templates now declare every module they import
  (`hull/http-server@1`, `hull/log@1`, `hull/time@1`); the JS
  template also gains its missing `import { app, log } from
  "hull:..."` lines.
- **Released native binaries shipped without the embedded platform
  library.** `hull doctor` on a freshly-installed v0.1.0
  `hull-linux-x86_64` / `hull-linux-aarch64` / `hull-darwin-arm64`
  reported `Platform library: embedded no` and `hull build: not
  ready`. Cause: the release-workflow native-build step ran plain
  `make` while the Cosmopolitan job correctly ran `make CC=cosmocc
  EMBED_PLATFORM=cosmo`. Now the native step runs `make platform`
  followed by `make EMBED_PLATFORM=1`, with a smoke-test check that
  fails the release if doctor doesn't report `embedded yes`. Binary
  size grows ~5.5 MB → ~12 MB per native platform; still well under
  the cosmo APE.

### Improved

- `HL_PLATFORM_PUBKEY_HEX` and `HL_RELEASE_PUBKEY_HEX` macros are
  now `#ifndef`-guarded in their headers so tests, self-hosted
  forks, and future rotation tooling can override them at compile
  time via `-DHL_*_PUBKEY_HEX=...`. Production builds inherit the
  embedded values unchanged.

### Reverted

- `HL_PLATFORM_PUBKEY_HEX` is back to the all-zeros placeholder.
  v0.1.0 shipped a real value, but the rest of the platform-sig
  chain (canary emission in `libhull_platform.a`, release-time
  signed manifest, `hull build` pass-through, runtime
  enforcement — tracked as item 2 in `docs/roadmap_next.md`)
  isn't in place yet. With a real pubkey embedded and no actual
  signed platform artefact, the pinning bypass in
  `hl_verify_startup` turns off and rejects every `hull build`
  output. The placeholder restores the documented "wired but
  inactive" state until the full chain ships. The platform key
  itself is still backed up offline and in the
  `HULL_PLATFORM_KEY` GitHub secret — no key regeneration needed
  when the chain is ready.

### Trust model

No changes. Existing v0.1.0 binaries continue to verify their own
updates against the same embedded release pubkey; v0.1.1 is a
drop-in replacement.

## [0.1.0] — 2026-05-25

First publicly tagged release. The goal of `v0.1.x` is to lock the
distribution and capability model so apps and tooling built against
`v0.1` can rely on a stable shape; performance, internal architecture,
and the WASM / GPU surface continue to evolve under that contract.

### Distribution

- **One binary per platform.** `hull` ships as a self-contained
  executable with no runtime dependencies. Bundled: Lua 5.4, QuickJS
  (ES2023), SQLite, mbedTLS, WAMR, TweetNaCl, the embedded Mozilla CA
  bundle, the platform standard library, and the build toolchain.
- **Four release artifacts per tag.**
  - `hull-linux-x86_64` — native ELF (~5 MB).
  - `hull-linux-aarch64` — native ELF for Graviton / NVIDIA DGX /
    Ampere / Raspberry Pi 4+ (~5 MB).
  - `hull-darwin-arm64` — native Mach-O for Apple Silicon (~5 MB).
  - `hull-cosmo` — Cosmopolitan APE (fat x86_64 + aarch64, ~30 MB)
    that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from
    a single file.
- **Signed releases.** Every release manifest (`hull.sha256`) is
  Ed25519-signed (`hull.sha256.sig`). The release public key is
  embedded in every Hull binary as `HL_RELEASE_PUBKEY_HEX`; `hull
  update` verifies the signature against the embedded key before
  atomically replacing itself via `rename(2)`. See
  [docs/release_signing.md](docs/release_signing.md).
- **One-line installer.** `curl -fsSL https://gethull.dev/install.sh | sh`
  detects OS+arch, downloads the matching native binary (or `hull-cosmo`
  on unsupported combinations), verifies SHA-256 against `hull.sha256`,
  and installs to `~/.local/bin/hull`.
- **Self-update.** `hull update` fetches the latest release via
  `api.github.com`, verifies both the signature and the SHA-256, and
  atomically replaces the running binary.
- **Shell completions.** Bash, zsh, fish completions ship in
  `completions/`.

### Runtimes

- **Lua 5.4** sandbox: `io` and `os` removed, custom `require` resolves
  only from the embedded stdlib registry, 64 MB memory cap, configurable
  per-request instruction limit (default 100 M instructions).
- **QuickJS** sandbox (ES2023): `eval` removed, `std`/`os` not loaded,
  64 MB memory cap, 1 MB stack cap, configurable per-request instruction
  limit.
- **One runtime per app** — entry point extension (`app.lua` or
  `app.js`) selects.
- **Polymorphic vtable.** Both runtimes go through the same
  `HlRuntimeVtable`; the capability layer (`hl_cap_*`) is shared C and
  is the only path to system resources.

### Capabilities

All system access is mediated by the C capability layer:

- **Database** — SQLite, parameterized queries, transactions, batch,
  user-defined functions (Lua/JS or WASM), async pool.
- **Filesystem** — manifest-allowlisted read/write paths, traversal
  rejection, symlink-escape rejection, mmap.
- **HTTP client / server** — separately gateable
  (`HL_ENABLE_HTTP_CLIENT`, `HL_ENABLE_HTTP_SERVER`). Server: routes,
  middleware, SSE, WebSockets, body limits, ETag, static files. Client:
  host-allowlisted `http.fetch`, SMTP, follow-redirects, mTLS via
  embedded mbedTLS.
- **Crypto** — SHA-256/512, HMAC, PBKDF2, Ed25519, NaCl box/secretbox,
  random. Constant-time comparison helpers.
- **Time / env / WebSockets / static files / image codecs**.
- **Compute (WASM)** — WAMR runtime, AOT compilation, gas metering,
  persistent instances, shared read-only data segments, streaming I/O.
  ~256 KB binary cost, gateable.
- **Compute (GPU)** — wgpu-native (Vulkan / Metal / DX12), WGSL
  compute shaders, pipelines, persistent buffers, GPU↔CPU and GPU↔GPU
  copies, textures. Opt-in (`HL_ENABLE_GPU=1`).
- **Tool mode (Lua-only)** — `hull build`, `hull deploy`, etc. run in
  a sandboxed Lua VM via `hl_tool_spawn` with a compiler allowlist.
  No `system()` / `popen()`.

### Capability sandbox

- **Linux / Cosmopolitan** — `pledge(2)` syscall filter + `unveil(2)`
  filesystem cap. Violation → SIGKILL.
- **macOS** — Seatbelt SBPL profile built dynamically from the
  manifest. Violation → EPERM.
- **OpenBSD** — native pledge/unveil.

### Module declaration

- `manifest.modules` is the canonical declaration mechanism. Apps list
  every first-party stdlib module they use, with API version
  (`"hull/crypto@1"` etc.).
- Two independent gates: **module resolver** (build-time / declaration
  check) and **per-call capability cap** (manifest `fs`/`env`/`hosts`
  allowlist enforced at the C boundary).
- `hull/app` is the only intrinsic — `app.manifest`, `app.get`,
  `app.post`, `app.use`, `app.router`, `app.ws`, `app.sse`, `app.main`.
  Other modules (timers, db, fs, http, …) must be declared.

### Build modes

- `make` — default build, ~5 MB on darwin/arm64.
- `make HL_ENABLE_DB=0` — compute-only build, ~3.9 MB on darwin/arm64
  (no SQLite, no DB-dependent stdlib modules).
- `make HL_ENABLE_HTTP_SERVER=0` — CLI-only build (no inbound HTTP).
  Apps may use `app.main(fn)` plus `http.fetch` outbound.
- `make HL_ENABLE_HTTP=0` — pure compute/CLI, no Keel, no mbedTLS,
  smallest possible build.
- `make CC=cosmocc EMBED_PLATFORM=cosmo` — fat APE binary with multi-arch
  embedded platform library.
- `make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu` — adds GPU compute.

### Application surface

- **Modular layouts** for REST / CLI / TUI apps: `hull new --type rest`
  / `--type cli` / `--type tui` scaffold a `routes/` + `models/` +
  `middleware/` + `lib/` skeleton instead of a single `app.lua`.
- **Background timers** — `app.every(ms, fn)` and `app.daily(hhmm,
  fn)` run on the event-loop thread, support full async runtime,
  return `false` to self-cancel.
- **Migrations** — versioned SQL migrations in `migrations/*.sql`,
  embedded in built binaries, auto-applied on startup, tracked by
  checksum.

### Standard library

Embedded `hull/*` modules (Lua + JS, identical semantics where the
languages allow):

- **Middleware** — cors, ratelimit, csrf, auth (session + JWT), session,
  logger, transaction, idempotency, outbox, inbox, rbac, health, etag.
- **Helpers** — cookie, jwt, template (with inheritance + filters),
  validate, form, i18n, csv, search (SQLite FTS5), image, db.udf.
- **CSRF wire format** is fixed and identical across Lua and JS
  (`tsHex.hmac_hex` over `session_id ":" tsHex`); a cross-runtime
  reference token is asserted by unit tests in both runtimes.

### Tooling

- **`hull agent`** — JSON introspection commands for AI coding agents:
  `routes`, `db schema`, `db query`, `request`, `status`, `errors`,
  `test`, `context`, `migrate`, `deploy`, `manifest`, `endpoint`,
  `middleware`, `capabilities`, `modules`, `validate`, `vfs`, `compute`,
  `gpu`, `perf`, `logs`, `eval`, `template`, `schema_diff`, `sql`.
- **`hull mcp`** — MCP server exposing the same introspection surface
  to MCP-aware clients (Claude Code, Cursor, OpenCode).
- **`hull doctor`** — environment-readiness report (platform embedded,
  compilers available).
- **`hull check`** — manifest + module-graph validation before tests.
- **`hull dev`** — watch-and-reload server with structured error
  sidecars (`--agent` writes `.hull/dev.json` + `.hull/last_error.json`).
- **`hull init`** — in-place project initialization (git-init-style;
  doesn't overwrite existing files).

### Distribution site

- `https://gethull.dev/` — landing page + `install.sh`. Hosted on S3
  behind CloudFront, deployed automatically from `site/**` changes via
  `.github/workflows/deploy-site.yml`.

### Licensing

Dual-licensed:
- **AGPL-3.0** — free for AGPL-compatible use.
- **Commercial** — closed-source embedding and proprietary
  distribution. Contact `licensing@artalis.io`.
- See [LICENSING.md](LICENSING.md) for which license applies to your
  use case.

### Known limitations in 0.1.0

- macOS x86_64 (Intel) is not a native target — Intel Mac users get
  `hull-cosmo` via the installer.
- Windows native binary not in the matrix yet; Windows users get
  `hull-cosmo` via WSL or directly (APE runs natively on Windows).
- HTTP/2 is supported via Keel, but production hardening of HTTP/2
  server is still maturing (see `vendor/keel/CLAUDE.md`).
- PostgreSQL backend for `hull/db` is on the post-0.1 roadmap; v0.1
  ships SQLite only.

[Unreleased]: https://github.com/artalis-io/hull/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/artalis-io/hull/releases/tag/v0.1.1
[0.1.0]: https://github.com/artalis-io/hull/releases/tag/v0.1.0
