# H1 / S5 - em-dash sweep + self-tested prose gates (record)

Status: **COMPLETE.** S5 rephrases em-dashes out of all living first-party prose
and adds two self-tested gates that keep the tree clean: no em-dashes in living
prose, and no new development-milestone narration in the S4-reviewed surface.
Stops for review before BuildContext.

## 1. Em-dash sweep

Every em-dash (U+2014) in living first-party prose is rephrased to the house-style
hyphen form: a spaced em-dash becomes a spaced hyphen ` - ` (the codebase's
dominant parenthetical separator), any other em-dash becomes `-`. This is a
convention-preserving rephrase,
not a blind byte swap, and it was applied in small area commits with pinned
before/after counts:

| Commit | Area | em-dashes |
|--------|------|-----------|
| A | behavior-bearing output STRINGS (reviewed separately) | 322 lines |
| B | `src/` + `include/` comments | 1700 -> 0 |
| C | `stdlib/` Lua/JS comments | 671 -> 0 |
| D | `Makefile` + `mk/` comments | 150 -> 0 |
| E | living `tests/` prose | 828 -> 0 |
| F | `examples/` + scripts + templates + tooling | ~515 -> 0 |
| G | active `docs/` + root markdown | ~1587 -> 0 |

**Behavior-bearing strings (Area A).** The 322 em-dashes inside code string
literals (agent deploy/overview JSON hints, `hull doctor` / `cache verify` /
compute output, smtp + wasm diagnostics, `hull verify` output, Lua/JS advisory
warnings) were triaged: all are human-readable advisory diagnostics - none is
protocol, golden-compared, or hash text - so all were rephrased and NO exact
allowlist entry was needed. Committed separately so the output changes are
reviewable in isolation. Unit tests (91/91) and the full build confirm no golden
broke.

**Excluded (not living first-party prose).** Vendored `vendor/**`, frozen
`docs/archive/**` bodies, `*.wat`/`*.wasm`/`*.aot` compute FIXTURES (behavior
data), and the frozen `LICENSE` (AGPL) are out of scope and retain their
em-dashes by design.

## 2. Two self-tested gates

Both are wired into `make lint`, `.PHONY`, and the CI Lint job, each with a
deterministic negative self-test that proves the gate BITES and then returns
CLEAN.

### 2.1 `check-no-emdash` (tests/check_no_emdash.sh)

Fails on any U+2014 in living first-party prose. Scope = all tracked files minus
the four documented non-living-prose classes above (each an exact path or a
semantic file-class - `vendor/`, `docs/archive/`, `*.wat|*.wasm|*.aot`, `LICENSE`
- never a broad keyword grep-out). There is no in-scope allowlist. The self-test
plants an em-dash in a tracked in-scope file, asserts the gate bites, removes it,
and asserts the gate returns clean.

### 2.2 `check-no-milestone-narration` (tests/check_no_milestone_narration.sh)

Keeps H1/S4 durable by matching development-milestone narration SHAPES - `Phase
<letter>`, `Phase N.N`, `Phase Nd-`, `Slice N`, `checkpoint N`; deliberately NEVER
a bare `Phase <N>` - and failing on any hit that is not an exact reviewed
survivor.

- **Scope** = the surface S4 reviewed: `src/ include/ stdlib/ Makefile mk/
  templates/`. `tests/`, `examples/`, and `docs/` are intentionally NOT covered:
  S4 did not review their design-phase labels, and docs legitimately narrate
  design history. This is a documented boundary, not a broad exclusion.
- **Survivors** (from [h1_s4_milestone_inventory.md](h1_s4_milestone_inventory.md))
  do not trip it: the architectural `Phase 1:`..`Phase 11:` (serve.c) and `Phase
  1/2` (sandbox) pipeline labels are bare `Phase <N>` and match no shape; audit
  provenance (`Phase 6 audit ...`) is bare and additionally semantically excluded
  (any line mentioning `audit`); the jobs.js/lua public-doc changelog is bare. The
  ONE dotted-shape survivor - sbom.c's public CLI help `Since Phase 4.3` - is the
  single exact-location allowlist entry.

The self-test proves the gate bites on each of the five shapes, and does NOT bite
an `audit` line or a bare `Phase 7:` pipeline label.

## 3. Also fixed here

S4's milestone census used `mk/*.mk`, which does not match the nested
`mk/features/*.mk`; those build-config comments (feature-composition `Phase
A/B/C/D`, keel `Phase 4.2b`, runtime `Phase 3b/3c`) and one missed `Phase C2` in
`src/hull/tool_orchestration.c` were recast to present tense so the new gate is
clean on its full scope.

## 4. Proof

- Living-scope em-dash census after the sweep: **0** unexpected in-scope hits
  (remaining em-dashes are all in vendor / docs-archive / .wat-.wasm / LICENSE).
- `make lint` exits 0 with both new gates reporting OK.
- Both self-tests pass (bite + clean).
- Full build byte-identical class + unit tests 91/91 pass (no behavior change from
  the Area-A string rephrases).
