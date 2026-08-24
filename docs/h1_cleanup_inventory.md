# H1 - code-housekeeping inventory & freeze (design-only)

Status: **DESIGN-ONLY. INVENTORY + PROPOSED PLAN. NOTHING DELETED, MOVED, OR
REWRITTEN.** This document is the ratification target for a Keel-style code
cleanup (dead code, redundancy, misplaced ownership, comment archaeology,
em-dashes) that follows the H0 repository-governance work and precedes the
BuildContext checkpoint. Execution happens only after this plan is ratified, in
small independently-verified slices.

Scope: FIRST-PARTY living code only - `src/hull/**`, `include/hull/**`,
`tests/hull/**`, `stdlib/**`, plus `scripts/ mk/ Makefile templates/ examples/`
for the mechanical sweeps. EXCLUDED throughout: `vendor/**`, `build/**`,
`tests/.playwright/**`, `docs/archive/**` (frozen historical snapshots).

Method: read-only static survey (grep + focused code reading). No build or
behavior was changed to produce this inventory. Every count records the exact
command + exclusions used so it is reproducible (see 1.5 for the pattern).

**Revision note (review round 1).** Three initial overclaims were corrected in this
freeze: `include/hull/cap.h` is NOT proven dead (it is internally-unused PUBLIC
surface - 1.1); hex consolidation must NOT route generic code through the crypto/
capability object without a link-closure proof (1.2a); and the seven constant-time
compares are NOT one semantic family (1.2b). The null findings (1.3, and the
intentional Lua/JS parity duplication) stand.

---

## 1. Findings

### 1.1 Dead code - minimal; and one "internally-unused public surface"

The `-Werror=unused-function` / `-Werror=unused-variable` floor keeps the tree
tight, so there is very little genuinely-dead code.

| Item | Evidence | Confidence | Disposition |
|---|---|---|---|
| `#if 0` / commented-out code blocks | None found. | - | none |
| Orphan `src/hull/**/*.c` | None found (every `.c` is in an OBJS list or a test link line). | - | none |
| Unreachable code / unused statics | None found (compile-time enforced). | - | none |
| Stale test fixtures / helpers | None found. | - | none |

**`include/hull/cap.h` is NOT dead code - it is internally-unused PUBLIC surface.**
It is an umbrella header with **zero** *in-repository* includes (every Hull
consumer includes the individual `cap/*.h`). But `include/hull/**` is documented as
Hull's PUBLIC C-header surface, so zero in-repo use does not prove it is unused by
external EMBEDDERS; deleting it may be a source-compatibility break. Disposition
(S1, audit only - no deletion):
- classify it as "internally-unused public surface", not dead code;
- check the release / install / package history and the stability policy
  ([`stability.md`](stability.md)) for whether it is part of the committed public
  surface;
- then either RETAIN it, or DEPRECATE it explicitly (a deprecation note + a
  release cycle) before any removal - never a silent delete.

Comments that *document a past cleanup* (e.g. `agent/template.c` "an earlier
snprintf snippet builder was dead code; removed per M-2") are NOT dead code; they
are addressed under comment archaeology (1.4), not deletion.

### 1.2a Hex encoding - real duplication, but a link-closure question (not a mechanical merge)

Hex encoding is implemented many times, but there are already (at least) THREE
DELIBERATE homes at different link boundaries - which is the whole point:

| Home | Symbol | Link cost | Callers |
|---|---|---|---|
| cap/crypto | `hl_cap_crypto_hex_encode()` (`cap/crypto.h:355`) | pulls the crypto object (mbedTLS/TweetNaCl-adjacent, much more than hex) | tests only, today |
| runtime/cache | `hl_runtime_cache_hex_encode()` (`runtime/cache_common.h:58`) | dependency-narrow | 5 (bytecode/template caches Lua+JS, `commands/cache.c`) |
| inline copies | `%02x` loops / local `hex_encode` statics | none | `runtime/{lua,js}/mod_crypto.c` (~16 / ~18), `signature.c:37`, `sbom.c:72`, `tool.c:65,82` (fprintf), 3+ test helpers |

The existing `hl_runtime_cache_hex_encode` duplication is EVIDENCE that ownership
and link composition already matter here. Routing `signature.c` / `sbom.c` /
release tooling / build commands through `hl_cap_crypto_hex_encode()` would drag a
crypto/capability object into otherwise-narrow link configurations and could damage
Hull's modular build closure (the composable-base + feature-archive story).

So hex is **NOT** a mechanical "point everything at the cap helper" merge. Its
consolidation (S2b) must START with a **caller-by-caller dependency / link-closure
table** (what each caller already links, what pulling in a shared helper would
add), then choose the canonical home(s):
- a **dependency-neutral primitive** - a tiny internal shared helper/header with NO
  crypto-backend dependency (candidate home for the generic byte->hex loop), or
- **separate helpers** where build boundaries intentionally require separation.
`tool.c` (writes straight to a `FILE*`) and `sbom.c` (SHA-256-specific lookup
table) may keep local forms regardless. No consolidation is ratified until the
link-closure table is reviewed.

### 1.2b Constant-time compare - 7 sites, NOT one semantic family

The seven XOR-accumulate sites look alike but differ in contract - representation,
fixed-vs-variable length, whether length may leak, and whether the values are
secret. They must NOT be collapsed into one generic helper.

| Site | Compares | Secret? | Length | Timing-resistance actually needed? |
|---|---|---|---|---|
| `commands/feature.c:141` `ct_hex_eq` | fixed 64-char public checksums | no | fixed | not required (public value) |
| `commands/flavor.c:84` `ct_hex_eq` | **verbatim duplicate** of feature.c | no | fixed | not required |
| `release_io.c:257` `ct_hex_eq` | release manifest checksum | no | fixed | own input-validation / length assumptions |
| `commands/verify_self.c:298` inline | self-verify checksum | no | fixed | own assumptions |
| `runtime/js/mod_crypto.c:1145` `constantTimeEq` | attacker-controlled strings | maybe | **variable** (explicit unequal-length contract) | yes |
| `runtime/lua/mod_crypto.c:815` `constant_time_eq` | attacker-controlled strings | maybe | variable, unequal-length contract | yes |
| `cap/crypto.c:913` (in `hmac_sha256_verify`) | secret-derived HMAC bytes | yes | fixed | yes |
| `cap/pg_conn.c:511` (SCRAM) | fixed 32-byte protocol value | protocol | fixed 32 | yes |

Only the **verbatim `feature.c` / `flavor.c` pair** is immediately safe to
consolidate (identical contract: fixed-length public checksum) - and it belongs in
their shared release/download layer (both are `hull feature/flavor install`
verifiers over `hl_release_io_*`), NOT in the cap layer. Any BROADER consolidation
(S3) requires a **comparison-contract matrix** + tests proving the length- and
nullability-leak behavior of each merged pair before it is ratified.

### 1.2c Excluded - intentional dual-runtime parity (NOT redundancy)

The Lua/JS HTML-escape tables (`template.lua:65` / `template.js:61`) and path
split/normalize (`path.lua` / `path.js`) are hand-mirrored by design, covered by
parity tests, and governed by [`stdlib_style.md`](stdlib_style.md). base64url
already delegates to the cap layer. Leave as-is - this is the contract, not debt.

### 1.3 Misplaced ownership - clean (null finding)

No layering violations found. All direct I/O in `src/hull/runtime/**` is
legitimate: CLI stdin/stdout/stderr inside `app.main`, dev-mode module / template
/ shader loading fallbacks (guarded, only after a VFS/embedded miss), and
build-time `mod_tool.c` file ops during `hull build` (unveil-gated). `src/hull/utils/**`
carries no Hull-domain knowledge (`HlManifest` / `hl_cap_*`); no cap/ includes
runtime/, no runtime/ includes commands/. **No moves are warranted.** (The one
historical wrinkle - orchestration in `mod_tool.c` - was already relocated to
`tool_orchestration.c` per an earlier audit.)

### 1.4 Comment archaeology - large

~206 milestone / refactoring-narration hits across ~69 first-party
`src/include/tests` files (excludes vendor). Kind breakdown:

| Token | Hits |
|---|---|
| `Phase 1..6` | ~150 |
| `checkpoint N` | ~26 (mostly "checkpoint 3", the fs work) |
| `Slice 1..7` | ~32 |

These are historical process narration ("Phase 4.3 removed...", "Slice B wired...")
that made sense during the work but is now noise in the trusted core. Keel removed
this class and added a permanent gate.

Two sub-classes, distinguished at execution time (this REPLACES the earlier
open decision - the split is now the ratified policy):
- **Obsolete process narration** -> **GO.** `Phase N` / `Slice N` / `checkpoint N`
  as a status marker, and bare "Added in PR #123" archaeology. Remove or rephrase
  to describe the CODE, not the milestone.
- **Durable `#NNNN` provenance** -> **KEEP.** A PR/issue reference tied to an
  EXTERNAL bug, a vendor behavior, a security finding, or a CI incident is durable
  rationale (a pointer to why the code is shaped this way, e.g. a WAMR-patch
  reference or a CVE). Keep these; they are not archaeology.

The S4 sweep is therefore semantic (human-reviewed per area), never a blanket
`#NNNN` strip.

### 1.5 Em-dashes - large, mechanical (counts are scope-dependent; command recorded)

Em-dash counts move with the scope, so the freeze records the EXACT command as the
source of truth rather than a bare number. Canonical measurement (living
first-party; excludes vendored/build/generated, `tests/.playwright/**`, and any
`fixtures/` tree):

```sh
LC_ALL=C grep -rl --binary-files=text $'\xe2\x80\x94' \
  src include tests stdlib scripts mk Makefile templates examples \
  | grep -vE '(^|/)(vendor|build|node_modules)/|/\.playwright/|(^|/)fixtures/'
# files: 865 ; occurrences (same list piped to `grep -o … | wc -l`): 4262
```

An independent review measurement under a slightly different living-tree scope
(also excluding `tests/fixtures/**`) reported **867 files / 4046 occurrences** - the
same order of magnitude; the small delta is scope/method (which trees, how
occurrences are counted). **S5 pins the exact per-area count with its own recorded
command at execution time** (the freeze does not depend on a single global number).

Order-of-magnitude by area (from the canonical command, informational):
`src/hull` ~249 · `stdlib` ~205 · `tests` ~166 · `examples` ~142 · `include` ~124 ·
`mk` 6 · `templates` 3 · `scripts` 2.

Pure comment/prose (confirmed context, e.g. `main.c:2` `* main.c — Hull's ...`); a
byte-safe sweep of ` — ` -> ` - ` (or rephrase) is mechanical. The project
convention is already "no em-dashes in code prose"; this is the one-time backlog
cleanup that makes the forward gate (§4) enforceable.

---

## 2. Proposed exclusions (the freeze list)

Do NOT touch, now or by any gate:

- **Vendored trees** (`vendor/**`) and generated/build output (`build/**`,
  `tests/.playwright/**`).
- **Frozen historical docs** (`docs/archive/**`) - snapshots by definition.
- **Dual-runtime parity duplication** (Lua/JS escape tables, path normalize, and
  any other intentionally-mirrored stdlib algorithm) - parity is the contract.
- **Dev-mode / CLI / build-tool direct I/O** in `runtime/**` - legitimate, guarded.
- **Weak-symbol stubs** (`*_weakstub.c`, `*_stub.c`) - intentional composition seams.
- **Comments documenting a decision or invariant** (even if they mention a past
  cleanup) where the RATIONALE is still useful - rephrase, do not blank-delete.
- **`#PR` provenance** pending the 1.6 decision.

## 3. Proposed execution slices (revised per review; ratify, then execute one at a time)

Each slice is independently reviewable + CI-verified; none mixes mechanical and
semantic change. Design-heavy items (cap.h disposition, hex link-closure, the
comparison-contract matrix) produce a ratified sub-decision BEFORE any code moves.

| Slice | Scope | Risk | Acceptance |
|---|---|---|---|
| **S1** | **Dead-code EVIDENCE AUDIT only. No `cap.h` deletion.** Determine whether `include/hull/cap.h` is part of the committed public surface (release/install/package history + [`stability.md`](stability.md)); recommend RETAIN or DEPRECATE-then-remove. | none (audit) | a written disposition; zero code change. |
| **S2a** | Consolidate ONLY the verbatim `feature.c` / `flavor.c` `ct_hex_eq` (identical fixed-length public-checksum contract) into their shared release/download layer (`hl_release_io_*`). | low | byte-identical verify behavior; feature/flavor install e2e green; the two copies gone. |
| **S2b** | **Hex ownership + link-closure DESIGN.** Produce the caller-by-caller dependency table (1.2a), choose the canonical home(s) - dependency-neutral primitive vs separate helpers - then ratify. No consolidation code until ratified. | design | a reviewed link-closure table + ratified home decision; zero code change in this slice. |
| **S3** | **Comparison-contract inventory** (the 1.2b matrix): representation / fixed-vs-variable length / length-leak / secrecy per site. Consolidate ONLY genuinely-identical contracts, each with tests proving length + nullability behavior. | medium (security) | the contract matrix + per-merge tests; no over-merge of differing contracts. |
| **S4** | Comment-archaeology sweep by area (one PR per area: cap, runtime, commands, include, tests, stdlib): remove `Phase/Slice/checkpoint` narration and bare "Added in PR #N"; **keep durable `#NNNN` provenance** (1.4). Semantic - human-reviewed, not sed. | low but broad | comments only; diff reviewed per area. |
| **S5** | Em-dash sweep by area (mechanical ` — ` -> ` - ` / rephrase), one PR per area; then enable the §4 gates. | low but broad | comments/prose only; the canonical command (1.5) returns 0 for the area. |

Ordering: S1 (audit) and S2a (the one safe consolidation) can go first; S2b and S3
are DESIGN slices that gate their own consolidation; S4 -> S5 land the prose
backlog that makes the forward gates enforceable.

## 4. Proposed permanent gates (self-tested, like `check-docs-integrity`)

To be wired into `make lint` + the CI Lint job ONLY after the corresponding
backlog slice lands (otherwise they fail on legacy debt):

- **`check-no-em-dash`** - fail if any living first-party file (the S5 scope, minus
  the freeze list) contains U+2014. Wire after S5.
- **`check-no-milestone-narration`** - fail on NEW `Phase N` / `Slice N` /
  `checkpoint N` status comments, and bare "Added in PR #N" archaeology, in
  first-party `src/include/tests`. **Must NOT flag durable `#NNNN` provenance**
  (1.4) or the words "phase"/"slice" in legitimate technical use - so the gate
  needs a carefully-scoped pattern + an allowlist, and a `-selftest` fixture
  proving it bites on narration while passing durable provenance. Wire after S4.

Each gate ships with a `-selftest` negative fixture proving it bites (the
`check_docs_integrity_selftest.sh` pattern).

## 5. Decisions

Resolved in review round 1 (folded into the slices above):
- **`#PR` provenance** - KEEP durable references (external bug / vendor / security
  / CI incident); strip bare "Added in PR #N" archaeology (1.4, S4, gate §4).
- **cap.h** - NOT a deletion candidate; audit-then-retain-or-deprecate (1.1, S1).
- **Hex / constant-time** - no mechanical merge; design-gated by S2b / S3.

Remaining design questions, each resolved WITHIN its slice (not before):
1. **cap.h final disposition** (retain vs deprecate) - output of S1.
2. **Hex canonical home(s)** (dependency-neutral primitive vs separate helpers;
   whether `tool.c` / `sbom.c` participate) - output of S2b's link-closure table.
3. **Which constant-time contracts are genuinely identical** - output of S3's
   matrix (only the `feature.c`/`flavor.c` pair is pre-cleared, as S2a).
4. **Milestone-gate pattern + allowlist** - output of §4 gate design.

---

**Nothing in this document has been executed.** On ratification, the slices run in
order, each as its own reviewed + CI-green PR (the design slices S1/S2b/S3 produce a
ratified sub-decision before any code moves); BuildContext follows the clean
baseline.
