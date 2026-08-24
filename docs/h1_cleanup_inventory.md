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
behavior was changed to produce this inventory.

---

## 1. Findings

### 1.1 Proven-dead code - minimal

The `-Werror=unused-function` / `-Werror=unused-variable` floor keeps the tree
tight, so there is very little genuinely-dead code.

| Item | Evidence | Confidence | Disposition |
|---|---|---|---|
| `include/hull/cap.h` | Umbrella header with **zero** inbound includes (every consumer includes the individual `cap/*.h`). | HIGH | **Delete** (S1). |
| `#if 0` / commented-out code blocks | None found. | - | none |
| Orphan `src/hull/**/*.c` | None found (every `.c` is in an OBJS list or a test link line). | - | none |
| Unreachable code / unused statics | None found (compile-time enforced). | - | none |
| Stale test fixtures / helpers | None found. | - | none |

Comments that *document a past cleanup* (e.g. `agent/template.c` "an earlier
snprintf snippet builder was dead code; removed per M-2") are NOT dead code; they
are addressed under comment archaeology (1.4), not deletion.

### 1.2 Redundancy - two real consolidations

**Hex encoding: 11 implementations.** A canonical, validated
`hl_cap_crypto_hex_encode()` (`include/hull/cap/crypto.h:355`) exists but is used
only by tests; production re-implements the byte->hex loop inline.

| Site | Form |
|---|---|
| `src/hull/runtime/lua/mod_crypto.c` | ~16 inline `%02x` loops |
| `src/hull/runtime/js/mod_crypto.c` | ~18 inline `%02x` loops |
| `src/hull/tool.c:65,82` | `fprintf` hex loops (direct-to-file; may stay) |
| `src/hull/signature.c:37` | `hex_encode` static (snprintf loop) |
| `src/hull/sbom.c:72` | `hex_encode_sha256` static (lookup table; SHA-256-specific) |
| `tests/hull/test_signature.c:22`, `tests/hull/cap/test_crypto.c:14,24,157` | test-only `hex_encode` helpers (3+) |

Consolidation target: route the runtime + `signature.c` + test helpers through
`hl_cap_crypto_hex_encode()`. `tool.c` (writes straight to a `FILE*`) and
`sbom.c` may keep local forms if the streaming shape matters - decide per site.

**Constant-time compare: 7 implementations** of the XOR-accumulate pattern.

| Site | Note |
|---|---|
| `src/hull/commands/feature.c:141` `ct_hex_eq` | **verbatim duplicate** of... |
| `src/hull/commands/flavor.c:84` `ct_hex_eq` | ...this one |
| `src/hull/release_io.c:257` `ct_hex_eq` inline | third copy |
| `src/hull/commands/verify_self.c:298` inline | fourth |
| `src/hull/runtime/js/mod_crypto.c:1145` `constantTimeEq` | public API, duplicates cap logic |
| `src/hull/runtime/lua/mod_crypto.c:815` `constant_time_eq` | public API, duplicates cap logic |
| `src/hull/cap/crypto.c:913` (inside `hl_cap_crypto_hmac_sha256_verify`) | the canonical volatile-accumulate |

Consolidation target: add a small public `hl_cap_crypto_ct_eq()` (or
`_ct_hex_eq()`) in the cap layer and collapse the command / release_io copies onto
it; the runtime `constantTimeEq` bindings then wrap the same helper. This is a
SECURITY-sensitive helper - treat as its own careful slice with tests, not a
mechanical sweep.

**Excluded (intentional dual-runtime parity, NOT redundancy):** the Lua/JS
HTML-escape tables (`template.lua:65` / `template.js:61`) and path
split/normalize (`path.lua` / `path.js`). These are hand-mirrored by design,
covered by parity tests, and governed by `stdlib_style.md`. base64url already
delegates to the cap layer (no duplication). Leave as-is.

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

Two sub-classes to distinguish at execution time:
- **Obsolete process narration** (`Phase N` / `Slice N` / `checkpoint N` as a
  status marker) -> remove or rephrase to describe the CODE, not the milestone.
- **`#NNNN` PR/issue provenance** (e.g. `#334: guard the mem64 fixture...`) -
  often genuinely useful as a pointer to rationale. **Decision needed** (1.6):
  keep meaningful provenance, or strip all issue refs as Keel did.

### 1.5 Em-dashes - large, mechanical

~897 first-party files contain U+2014 em-dashes in comment prose (confirmed
context, e.g. `main.c:2` `* main.c — Hull's ...`). By area:

| Area | Files |
|---|---|
| `src/hull` | 249 |
| `stdlib` | 205 |
| `tests` | 166 |
| `examples` | 142 |
| `include` | 124 |
| `mk` | 6 |
| `scripts` | 2 |
| `templates` | 3 |

Pure comment/prose; a byte-safe sweep of ` — ` -> ` - ` (or rephrase) is
mechanical. The project convention is already "no em-dashes in code prose"; this
is the one-time backlog cleanup that makes the forward gate (1.7) enforceable.

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

## 3. Proposed execution slices (ratify, then execute one at a time)

Each slice is independently reviewable + CI-verified; none mixes mechanical and
semantic change.

| Slice | Scope | Risk | Acceptance |
|---|---|---|---|
| **S1** | Delete `include/hull/cap.h` (dead header). | trivial | full build + `make test` green; `nm`/grep shows no lost symbol. |
| **S2** | Route hex encoders through `hl_cap_crypto_hex_encode` (runtime mod_crypto Lua+JS, `signature.c`, test helpers). Leave `tool.c`/`sbom.c` streaming forms unless clean. | low (pure refactor) | byte-identical hex output (existing crypto/signature tests); no behavior change. |
| **S3** | Add `hl_cap_crypto_ct_eq()` in the cap layer; collapse `feature.c`/`flavor.c`/`release_io.c`/`verify_self.c` copies + wrap the runtime `constantTimeEq`. | medium (security helper) | new unit tests for the helper (equal / unequal / length-mismatch, timing-shape); all signature/feature/flavor/verify e2e green. |
| **S4** | Comment-archaeology sweep: remove/rephrase `Phase N` / `Slice N` / `checkpoint N` narration per area (one PR per area: cap, runtime, commands, include, tests, stdlib). Semantic - human-reviewed, not sed. | low but broad | no code change (comments only); diff reviewed per area. |
| **S5** | Em-dash sweep per area (mechanical ` — ` -> ` - ` / rephrase), one PR per area. | low but broad | no code change; `LC_ALL=C grep -rl $'\xe2\x80\x94'` for the area returns 0. |

Ordering: S1 -> S2 -> S3 (code) then S4 -> S5 (prose). S4/S5 land the one-time
backlog that makes the forward gates enforceable.

## 4. Proposed permanent gates (self-tested, like `check-docs-integrity`)

To be wired into `make lint` + the CI Lint job ONLY after the corresponding
backlog slice lands (otherwise they fail on legacy debt):

- **`check-no-em-dash`** - fail if any living first-party file (the S5 scope, minus
  the freeze list) contains U+2014. Wire after S5.
- **`check-no-milestone-narration`** - fail on NEW `Phase N` / `Slice N` /
  `checkpoint N` status comments in first-party `src/include/tests`. Wire after
  S4. (Scope carefully: this targets process-narration vocabulary, not the words
  "phase"/"slice" in legitimate technical use - the gate must be self-tested with
  a negative fixture, and may need an allowlist for genuine domain uses.)

Each gate ships with a `-selftest` negative fixture proving it bites (the
`check_docs_integrity_selftest.sh` pattern).

## 5. Open decisions for review

1. **`#PR` provenance comments** - keep meaningful ones (pointer to rationale), or
   strip all issue references as Keel did? Affects S4's scope and whether the
   milestone gate also forbids bare `#NNNN`.
2. **`tool.c` / `sbom.c` hex** - consolidate onto the cap helper (needs a
   buffer-returning variant) or leave the streaming/lookup-table forms? (S2 scope.)
3. **Milestone-narration gate scope** - is a `Phase N` / `Slice N` / `checkpoint N`
   ban acceptable, given "phase" and "slice" also have legitimate technical
   meanings? (Gate design for §4.)

---

**Nothing in this document has been executed.** On ratification (with the §5
decisions), the slices run in order, each as its own reviewed + CI-green PR;
BuildContext follows the clean baseline.
