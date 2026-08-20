<!--
SPDX-License-Identifier: AGPL-3.0-or-later
-->
# JS source-frontend Slice B - pinned, parser-scoped, module-only Test262 conformance

Status: design (approved with five tightenings, folded in below). Second of the two ratified
follow-on slices to the JavaScript source-analysis frontend (Slice A = the libFuzzer harness,
merged as 6c76ea25). Independently mergeable; design-first.

## 1. Purpose and scope

Add a standards-derived conformance gate: run Hull's JS parser (`hull:source:parser`) against a
pinned, committed subset of the official Test262 suite (https://github.com/tc39/test262) and
gate parser accept/reject against Test262's own metadata AND a QuickJS compile-only oracle.
Complements the Slice-A robustness fuzzer (no crash under arbitrary bytes) with correctness vs
the standard.

### 1.1 Initial goal: MODULE-flagged parser cases only

The FIRST integration is limited to Test262 cases explicitly marked `flags: [module]` under
`test/language/**`. Hull analyzes application JavaScript as MODULE source and the parser has no
script/module parse-goal API; honoring `onlyStrict` / `noStrict` / `raw` / script-goal would
either misclassify cases or silently invent a parser feature. Script-goal Test262 coverage is a
later story IF Hull ever needs script parsing.

Excluded (with REPORTED selection counts, section 9): execution/`runtime` and `resolution`
negatives, `test/built-ins/**`, `intl402`, `staging`, and any `flags` combination that is not a
clean module goal (a conflicting/incompatible goal flag is REJECTED, not treated as a module).
Only `flags` CONTAINING `module` participate.

### 1.2 negative.phase semantics

Test262 negative phases are `parse`, `resolution`, `runtime` -- there is no separate "early"
phase; early errors are represented under `negative.phase: parse`
(https://github.com/tc39/test262/blob/main/INTERPRETING.md#negative). Only `phase: parse`
participates in PARSER judgment; `resolution`/`runtime` negatives are excluded from parser
verdicts (a valid parse whose error is a link/runtime error is not the parser's concern).

## 2. Delivery: pinned, committed, GENERATED subset (offline)

A `scripts/fetch_test262.sh` clones Test262 at a PINNED commit, selects the module-flagged
language/parser subset, copies the cases, and GENERATES `manifest.json`. The committed subset is
what CI uses; CI is completely offline and NEVER invokes the fetch script. Layout:

```
tests/fixtures/test262/
  LICENSE           # Test262's BSD license, copied VERBATIM (provenance)
  UPSTREAM.md       # pinned commit SHA, fetch date (informational), selection-rules version, tool notes
  manifest.json     # GENERATED; upstream facts only (section 4); the C test's ONLY metadata source
  MANIFEST.sha256   # sha256 over manifest.json's canonical bytes (section 6)
  expectations.json # Hull POLICY, hand-reviewed (section 5); the C test's ONLY policy source
  cases/...         # the selected .js files, upstream copyright headers preserved
```

Provenance separation is deliberate: the GENERATOR writes only upstream facts (manifest); Hull
POLICY (what Hull declines / diverges on) lives in the separately hand-reviewed
`expectations.json`, so the fetch generator can never silently bless Hull behavior.

## 3. Fetch / selection tooling (reproducible)

`scripts/fetch_test262.sh` (run by a maintainer, never by CI):

- Fetches the EXACT pinned SHA into a temporary directory and verifies `HEAD` equals that SHA;
  it never consumes the moving default branch.
- Parses each candidate file's YAML frontmatter in Python (documented dependency: Python 3 +
  PyYAML, with an expected-version note); FAILS on malformed or unknown frontmatter rather than
  skipping it. Never auto-installs dependencies.
- Selects `test/language/**` cases whose `flags` contain `module` and whose goal is a clean
  module (rejecting conflicting goal flags), excluding `resolution`/`runtime`-only negatives per
  1.1; records exclusion counts by reason.
- Sorts every selection under a FIXED locale (`LC_ALL=C`) for deterministic order.
- Preserves each case's Test262 copyright header; copies the upstream `LICENSE` verbatim.
- Rejects symlinks and any path escape (a case path must be relative, canonical, inside
  `cases/`).
- Emits `manifest.json` (section 4) + `MANIFEST.sha256`; writes everything through a TEMPORARY
  output tree and replaces the destination only after complete success (no partial corpus).
- The fetch DATE is informational only (UPSTREAM.md); it is NEVER part of hashed deterministic
  metadata.

Refreshing the pin = re-run the script at a new SHA; the committed diff (manifest + cases +
hashes) is reviewed.

## 4. manifest.json - upstream facts ONLY

A `schema_version`, the pinned `upstream_sha`, a `selection_rules_version`, a `count`, and a
sorted `cases` array. Each case entry records ONLY upstream-derived facts:

```json
{
  "schema_version": 1,
  "upstream_sha": "<40-hex>",
  "selection_rules_version": 1,
  "count": <N>,
  "cases": [
    { "path": "language/module-code/example.js",
      "goal": "module",
      "flags": ["module"],
      "features": ["import-assertions"],
      "negative": { "phase": "parse", "type": "SyntaxError" }   // or null
    }
    // ... sorted by path (LC_ALL=C)
  ]
}
```

`path` is relative to `cases/`. NO Hull policy appears here. `source_hash` per case is carried
in the manifest too (a sha256 of the case bytes) so the C test can verify each committed case is
byte-identical to what was selected.

## 5. expectations.json - Hull POLICY (closed, exact-path keyed)

Hand-reviewed; the ONLY source of Hull policy. Maps EXACT case paths to a reviewed expectation
so that NO Hull-specific outcome is open-ended. Three categories:

```json
{
  "schema_version": 1,
  "expectations": {
    "language/expressions/dynamic-import/...": {
      "category": "unsupported-feature",
      "key": "dynamic-import",
      "reason": "Hull's source parser declines dynamic import()"
    },
    "language/module-code/...": {
      "category": "target-version-divergence",
      "reason": "vendored QuickJS does not implement <feature>"
    },
    "language/expressions/...": {
      "category": "static-semantic-omission",
      "reason": "Hull's parser intentionally does not enforce <early static rule>"
    }
  }
}
```

- `unsupported-feature` entries reference a key in a CLOSED unsupported-feature inventory (the
  set of constructs Hull's parser declines with `js.unsupported`). Every `unsupported` outcome
  must resolve deterministically to exactly ONE inventory key.
- `target-version-divergence` closes the Test262-vs-QuickJS disagreement bucket (section 6): an
  UNLISTED disagreement FAILS; a listed entry that STOPS diverging FAILS as a stale expectation.
- `static-semantic-omission` closes Hull's intentional early-error omissions the same way.

Closed both ways: an unexpected outcome without an entry FAILS, and a stale entry whose case no
longer exhibits the behavior FAILS. `expectations.json` has its own reviewed hash reported at
selection time.

## 6. Corpus integrity - fail closed

Before any conformance judgment, the C test validates the committed corpus and ABORTS the leg on
any breach:

- `schema_version` (manifest + expectations) is the expected value;
- `upstream_sha` matches the pin the test expects;
- `selection_rules_version` matches;
- the manifest `count` equals the number of `cases` entries;
- every `path` is relative, canonical, and resolves INSIDE `cases/`;
- every case file is a REGULAR file, not a symlink;
- each case is read in FULL (exact byte length) and its `source_hash` matches;
- no duplicate paths; the `cases` array is in sorted (`LC_ALL=C`) deterministic order;
- every committed `cases/**.js` appears in the manifest, and NO extra `.js` exists outside it
  (bijection between the manifest and the on-disk cases);
- `MANIFEST.sha256` matches `sha256(manifest.json canonical bytes)`;
- after the run, ALL enumerated cases were read AND analyzed (a count reconciliation).

Hash definition (avoid a self-referential hash): the manifest carries NO hash field of itself;
integrity is a SEPARATE `MANIFEST.sha256` over the manifest's canonical on-disk bytes. Case
integrity is the per-case `source_hash` inside the manifest (a hash of case bytes, not of the
manifest). The fetch date is excluded from all hashed metadata.

## 7. Three-way oracle + classification

Per case, three verdicts: the Test262 expectation (positive, or `negative.phase == parse`), the
QuickJS COMPILE-ONLY module verdict (parse + bytecode-gen, no execution, MODULE mode -- the same
oracle the existing conformance harness uses), and the Hull parser verdict (ACCEPT / REJECT
`js.syntax` / UNSUPPORTED `js.unsupported` / INDETERMINATE `js.limit.*`|`js.internal`).

| Condition | Bucket | Gate |
|---|---|---|
| Test262 & QuickJS disagree | target-version divergence | 0 UNLESS an exact-path `target-version-divergence` expectation (a stale one FAILS) |
| Test262 positive + QuickJS accepts + Hull `js.syntax`-rejects | false reject | 0 |
| Test262 parse-negative + QuickJS rejects + Hull clean-accepts | false accept | 0 UNLESS an exact-path `static-semantic-omission` expectation |
| QuickJS accepts + Hull clean `js.unsupported` | unsupported | reported; MUST map to one closed inventory key (else FAIL) |
| Hull `js.unsupported` + QuickJS REJECTS | unsupported-reject | 0 UNLESS an exact-path reviewed expectation (js.unsupported is valid only when the exact source is valid for Hull's QuickJS target) |
| Hull `js.unsupported` + `js.syntax` (same unit) | failure | 0 (unconditional) |
| Hull `js.internal` / `js.limit.*` | indeterminate | 0 |
| `resolution` / `runtime` negative | excluded from parser judgment | -- |

Every bucket that is not a hard pass resolves DETERMINISTICALLY to either a gate-0 failure or an
exact-path reviewed expectation. There is no open-ended "reported and ignored" bucket: a future
QuickJS or selection regression that would enlarge the divergence/unsupported set FAILS unless a
maintainer reviews and records it.

## 8. Test integration

Extend `tests/hull/frontend/test_js_conformance.c` with a `test262` leg that (a) runs the
section-6 corpus-integrity checks, (b) reads `manifest.json` + `expectations.json` via `sh_json`
(NO C-side YAML -- frontmatter is parsed once, offline, by the fetch script), (c) for each case
computes the three verdicts and buckets per section 7, and (d) gates. Reuses the existing
harness's QuickJS compile-only oracle and Hull-session parser-verdict engine.

## 9. The pre-commit size/file-count report (gate)

The vendored cases are NOT committed until a maintainer approves the selection. After the fetch
tooling lands, running it produces a report: pinned SHA; selected case count; total bytes;
positive vs parse-negative counts; exclusion counts by reason; a feature breakdown; anticipated
unsupported / divergence counts if classified; the license/provenance layout; and the manifest +
expectations hashes. Only after that report is approved are `cases/`, `manifest.json`,
`MANIFEST.sha256`, `expectations.json`, `LICENSE`, and `UPSTREAM.md` committed.

## 10. Non-scope

No script-goal parsing, no execution/resolution/runtime judgment, no `built-ins`/`intl402`/
`staging`, no parser behavior change, no new parse-goal API. The QuickJS oracle and Test262
target a specific ES version; divergences from a newer/older target are CLOSED via
`expectations.json`, not silently tolerated.
