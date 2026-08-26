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
a bare `Phase <N>` - and failing on any hit that is not a reviewed survivor.

**Scope** = every tracked file MINUS the classes below. There is **no
directory-wide exclusion**: all first-party CODE / BUILD / CI-config / SCRIPT /
TEST / EXAMPLE comment prose is governed (S5 expanded the census into `tests/`,
`examples/`, `.github/`, and `scripts/` and recast the reducible narration S4's
narrower census had missed). Each excluded class is an exact path or a semantic
file-class, documented with **why it cannot contain the governed prose**:

| Excluded class | Kind | Why it cannot hold the governed prose |
|----------------|------|---------------------------------------|
| `vendor/**` | scope | third-party; not our comment prose to govern |
| `*.md` markdown | semantic | prose DOCUMENTS whose SUBJECT is the design incl. its phased history (`Phase 4.3 removed X` is content, not comment clutter). Still governed by `check-no-emdash`; the milestone rule is code-comment-specific. **Self-tested**: a shape in a `.c` bites, the same in a `.md` does not. |
| `*.wat *.wasm *.aot` | semantic | compute fixtures / binary artifacts - behavior data, not design-comment prose |
| the gate + its self-test | exact | contain the shapes as their definition / test data (circular) |

Additional exceptions, both narrow: `audit`-bearing lines are a **semantic**
exception (audit / security provenance always preserved), and the one dotted-shape
survivor - `sbom.c`'s public CLI help `Since Phase 4.3` - is an **exact-location**
allowlist entry. The bare architectural pipeline labels (`serve.c` `Phase
1:`..`Phase 11:`, `sandbox` `Phase 1/2`) and the jobs.js/lua public-doc changelog
are bare `Phase <N>` and match no shape.

The self-test proves the gate bites on each of the five shapes AND on planted
`tests/` narration (expanded scope), and does NOT bite an `audit` line, a bare
`Phase 7:` pipeline label, or milestone narration inside a `.md` document
(semantic exclusion).

## 3. Reducible narration recast (S4 census gaps + expanded scope)

S4's milestone census under-reached: `mk/*.mk` did not match nested
`mk/features/*.mk`, and `tests/` / `.github/` / `scripts/` were never covered. S5
recast the reducible narration in all of them to present tense: the
feature-composition build comments (`mk/features/*`, `tool_orchestration.c`), the
CI-architecture slice/checkpoint labels in `.github/workflows/*` and
`scripts/ci/*`, the durable-execution / frontend / fs test-section labels in
`tests/`, and the two `examples/` compute apps - keeping every doc reference,
`issue #114`, and `§`/Appendix cross-ref. The em-dash gate governs markdown; the
milestone gate does not, so design records keep documenting their phases.

## 4. Proof

- Living-scope em-dash census after the sweep: **0** unexpected in-scope hits
  (remaining em-dashes are all in vendor / docs-archive / .wat-.wasm / LICENSE).
- `make lint` exits 0 with both new gates reporting OK.
- Both self-tests pass (bite + clean).
- Unit tests **91/91 pass** and the full build succeeds.

### 4.1 Binary impact (honest accounting)

The produced `hull` binary is **NOT byte-identical** across S5. Measured on the
same host/toolchain, pre-S5 base (`#415` tip) vs S5 tip:

| Metric | Result |
|--------|--------|
| File SIZE | identical, 7601128 bytes (a coincidence, not evidence of equality) |
| Executable SHA-256 | **differs**: base `8f2b27c4750db4afe67d49b4ba4012b16246e4aa31a06359324173a741cf9c07` vs S5 `b979263d67931ff6bab8432599479b0320f4331541d78e53cd05e4f1c029a5e9` |
| `cmp -l` differing byte positions | ~3.08M (dominated by cascading offset shifts, below) |

Two intended change classes enter the binary; neither is "zero behavior change":

1. **322 compiled diagnostic-output strings** (Area A) - `hull doctor` / `deploy`
   / `cache verify` / compute / smtp / wasm / `verify` advisory text - changed
   em-dash -> hyphen. This is an **intended, behavior-preserving change to
   human-readable diagnostic wording**: the messages are advisory (not a parsed
   protocol / stable output contract), the meaning is unchanged, and the unit +
   e2e output tests pass (no golden asserted an em-dash; goldens that echo swept
   source stay in sync). It is classified as an intentional API-semantics-
   preserving wording change, backed by tests - not as no-op.
2. **Embedded stdlib source** - the Lua/JS stdlib is carried VERBATIM (comments
   included) in the platform VFS embedded in the binary, so the Area-C stdlib
   comment rephrases change those embedded bytes. Because em-dash (3 bytes) ->
   hyphen (1 byte) shortens each line, every subsequent byte in an embedded file
   shifts, which is why `cmp -l` reports millions of differing positions from a
   few hundred edits. The **executed stdlib behavior is unchanged** (Lua/JS
   ignore comments); only the embedded source text differs.

So: file size collides by coincidence, the executable hash legitimately differs,
and the differences are the two intended, test-backed classes above.
