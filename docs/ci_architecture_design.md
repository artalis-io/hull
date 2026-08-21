# Change-Aware CI Architecture - design prompt

Status: **DRAFT - DESIGN HARD STOP. NOT YET RATIFIED OR IMPLEMENTED.**

## 0. Purpose

You are working in the Hull repository.

Rework GitHub Actions so tooling, frontend, and domain-compiler pull requests do
not pay the complete trusted-core CI cost unless they touch C, native runtime,
build-composition, ABI, or other core-sensitive surfaces.

The goal is not weaker CI. The goal is:

> Run the strongest tests relevant to the changed subsystem on pull requests,
> while preserving complete trusted-core verification when core-sensitive code
> changes and on every push to `main` and every scheduled maximal run.

The target flow is:

```text
                         pull request
                              |
                     fail-closed classifier
                              |
          +-------------------+-------------------+
          |                   |                   |
          v                   v                   v
       tooling              domain               core
          |                   |                   |
     focused tests       focused compiler      full matrix
     + fresh host          integrations
          +-------------------+-------------------+
                              |
                              v
                  applicability-aware CI gate
```

For `main`, schedule, or explicit force-full dispatch:

```text
full tooling + domain + core verification
```

Path classification is a pull-request optimization, not Hull's final
correctness oracle.

## 1. Required design cadence

CI is part of Hull's security, release, and trusted-runtime boundary. Do not
rewrite workflows immediately.

Before implementation:

1. Inspect all workflows, Make targets, test scripts, matrices, caches, and
   branch-check names.
2. Measure representative recent workflow runs where access permits.
3. Produce a job/path/coverage/runtime mapping.
4. Write verified findings and the proposed execution plans into this document.
5. Present the design for ratification.
6. Stop.

Implement only after approval, using the review-gated slices in section 24.

The first implementation slice is classifier plus classifier tests only. It
must not simultaneously rewrite the full workflow.

## 2. Current repository facts to preserve

Repository inspection must verify and update these observations:

- `.github/workflows/ci.yml` is a large multi-job workflow with native platform
  builds, many E2Es, sanitizers, fuzzing, native integrations, reproducibility,
  feature builds, browser tests, coverage, and lint.
- It currently has top-level documentation `paths-ignore` entries for both PRs
  and pushes to `main`.
- Lua and JavaScript tooling under `stdlib/cli/**` is embedded into the Hull
  executable through generated registries.
- A changed embedded parser/tool module is not present in an old cached Hull
  binary.
- Separate workflows exist for DCO, release, site deployment, and specialized
  Cosmopolitan/Windows probes.
- The current fuzz job combines many native protocol fuzzers with Lua and JS
  parser fuzzers.
- Release workflows and reproducibility checks must not consume selectively
  verified or untrusted cached build products.

Do not duplicate or accidentally remove current coverage without accounting for
it explicitly.

## 3. Important GitHub Actions model

Do not solve required CI using top-level `paths:` or `paths-ignore:` filters on
the required orchestrator. A skipped required workflow can produce confusing or
pending branch-protection behavior.

Use:

```text
one always-triggered orchestrator
    -> classifier
    -> conditional jobs
    -> one always-evaluated required gate
```

Individual expensive jobs may use `if:`. The required workflow itself always
starts.

The orchestrator must trigger on:

```yaml
pull_request:
push:
  branches: [main]
schedule:
workflow_dispatch:
```

For `push` to `main`, `schedule`, and force-full dispatch, do not use path
classification to skip normal critical suites.

## 4. Embedded tooling requires a fresh Hull link

This is a load-bearing Hull-specific rule.

Changes under:

```text
stdlib/cli/lua/**
stdlib/cli/js/**
```

alter bytes embedded in the Hull executable. Tooling-only CI therefore may not
restore an old complete Hull binary and claim it tested the changed tooling.

At minimum it must:

```text
regenerate affected tooling registries
compile/embed changed Lua or JS tooling
link a fresh Hull executable
run focused tests through that executable
```

V1 should use one clean representative Linux host build. The primary saving
comes from not launching unrelated platform/native jobs.

A future optimization may cache native core objects/archives and relink them
with fresh tooling registries only if ABI, flags, architecture, compiler, and
composition inputs are keyed exactly. Do not implement stale full-host reuse.

## 5. Classification is overlapping, not exclusive

Do not assign each PR exactly one of `tooling`, `domain`, or `core`. A change can
affect several surfaces simultaneously.

The classifier emits overlapping facts, conceptually:

```text
tooling_changed
lua_frontend_changed
js_frontend_changed
project_discovery_changed
agent_dev_changed
domain_changed
query_changed
compute_changed
production_core_changed
build_composition_changed
native_db_changed
wasm_changed
gpu_changed
tls_network_changed
ci_changed
docs_only
```

It then derives an explicit execution plan:

```text
focused_tooling
focused_lua_frontend
focused_js_frontend
focused_query
focused_compute
focused_db
focused_wasm
focused_gpu
full_core
full_all
```

If core and tooling/domain both change, run the full core plan plus the relevant
focused tests. Do not assume the broad native matrix necessarily includes every
new compiler-specific acceptance test.

## 6. Canonical repository-owned classifier

Introduce one classifier implementation. Ratified choice (Slice 1):

```text
scripts/ci/classify_changes.py   (Python 3)
```

Python 3 keeps the result portable and easily testable in Hull's existing CI
images, and - unlike POSIX `sh`, whose `read` cannot reliably consume
NUL-delimited paths - it parses the NUL-safe diff and emits deterministic JSON.

Do not duplicate path rules across job `if:` expressions. Do not add an
unpinned third-party path-filter action.

The classifier must:

- accept changed paths in a binary-safe form, preferably NUL-delimited;
- handle spaces, tabs, newlines, deletions, copies, and renames;
- produce deterministic JSON and/or documented GitHub outputs;
- classify unknown paths as core-sensitive;
- fail closed to `full_all` on parsing, diff, or rule failure;
- fail closed on an empty or ambiguous PR diff;
- keep all path rules in one reviewed source;
- expose both facts and the derived required-job plan;
- have direct fixture tests independent of GitHub Actions.

For pull requests, calculate changes from the real merge base:

```text
base SHA ... head SHA
```

Fetch enough history. Do not use only `HEAD^`. Avoid API-based file lists that
can silently truncate due to pagination.

For `main`, schedules, and force-full dispatch, set `full_all=true` directly.

## 7. Conservative path rules

### 7.1 Production core

At minimum classify these as core-sensitive after reconciling actual paths:

```text
src/**
include/**
production native C/C++
shared native headers
platform/runtime/Keel code
vendor/** and submodule/dependency revisions
Makefile and mk/**
compiler/linker flags and composition
generated native registry/build glue
C frontend/QuickJS bridge/session/generation code
WAMR integration
native GPU integration
native database adapters
TLS/network adapters
.github/workflows/**
scripts/ci/**
classifier fixtures
```

Unknown production/native/shared paths are core-sensitive. Workflow, classifier,
and shared build changes force broad/full verification.

### 7.2 Test-only C is not automatically production core

Do not classify every `.c` file as trusted runtime code.

Examples such as:

```text
tests/hull/frontend/**
fuzz/fuzz_js_source.c
fuzz/fuzz_lua_source.c
```

may remain focused test surfaces when they do not alter production code or
shared build configuration.

Rules:

- production C/public header: core;
- frontend C bridge/session/manager: core plus focused frontend tests;
- test-only frontend C harness: focused frontend verification;
- native Make/build recipe change: core;
- unknown C path: core.

### 7.3 Tooling

Likely tooling paths include:

```text
stdlib/cli/lua/**
stdlib/cli/js/**
source parsers/annotations/scope/bindings
ProjectDiscovery
agent inspection and dev tooling
tool-only tests/fixtures
```

Shared ProjectDiscovery or projection changes should run both frontend and mixed
discovery acceptance even when only one source file changed.

### 7.4 Domain compilers

Reserve stable directories/classes for Query, Compute, and future domain code.
Classify by architectural role, not filename substrings.

Examples:

```text
pure Query IR/types/validator
    -> focused Query tests

Lua/JS Query adapters
    -> Query + relevant frontend/discovery tests

SQLite Query backend
    -> Query + SQLite integration

runtime binding generation
    -> build pipeline + Lua/JS runtime E2E

production C db artifact support
    -> full core + DB/runtime tests

Compute IR
    -> focused Compute tests

WASM backend
    -> Compute + WAMR integration

GPU backend
    -> Compute + GPU integration
```

### 7.5 Documentation

`docs_only=true` only when every changed path is in the explicitly safe
documentation set. Verify that no such files are embedded or used as generated
build inputs. Files such as `stdlib/context/*.md` are not ordinary docs merely
because they end in `.md` if they are embedded into Hull.

Unknown Markdown/build-document inputs fail closed rather than being assumed
safe.

## 8. CI changes and classifier self-trust

Any change to workflow YAML, classifier logic, classifier tests, shared workflow
helpers, Makefiles, or build configuration forces full verification.

A PR can modify the workflow/classifier that evaluates that PR. CI cannot make
modified workflow code intrinsically trustworthy. Preserve repository review,
CODEOWNERS, and branch controls for workflow changes. Document this governance
boundary rather than claiming the classifier protects itself.

## 9. Pull-request plans

### 9.1 Focused JS frontend

For a parser/frontend-sensitive JS change, run at least:

```text
lint/format checks applicable to JS tooling
one clean Linux Hull build with the changed JS registry embedded
JS session/lexer/parser/annotations/scope/frontend tests
JS conformance tests
JS parser fuzz smoke when parser/fuzzer-sensitive paths changed
ProjectDiscovery tests
mixed Lua/JS discovery E2E
hull dev --agent lifecycle
hull agent inspect live + standalone
required gate
```

Do not run unrelated TLS, networking, WAMR, GPU, every DB, every platform,
reproducibility, or native-protocol fuzz matrix.

### 9.2 Focused Lua frontend

For Lua parser/frontend changes, run at least:

```text
Lua lint
one clean Linux Hull build with changed Lua tooling embedded
Lua lexer/parser/statements/annotations/scope/conformance
Lua parser fuzz smoke when parser/fuzzer-sensitive paths changed
ProjectDiscovery tests
mixed-language discovery E2E
hull dev --agent
hull agent inspect
required gate
```

### 9.3 General tooling

Choose focused unit/E2E targets based on the affected tool surface, plus one
fresh representative host build where embedded tooling changes. Do not run
parser fuzzing for unrelated agent/help/template changes.

### 9.4 Domain

Run relevant frontend/discovery, domain IR/lowering/validator tests, selected
runtime integration, one representative build, and dev/agent acceptance.

Do not trigger unrelated native suites. Production C changes independently
elevate the plan to core.

### 9.5 Core

Preserve the existing trusted-core coverage, including the actual supported
platforms and jobs: Linux variants, macOS, aarch64, Cosmopolitan, musl,
sanitizers, native integrations, reproducibility, build/package/feature tests,
and specialized acceptance where relevant.

Do not invent a generic Windows native matrix if Hull currently validates
Windows only through specialized Cosmopolitan workflows.

## 10. Main and scheduled behavior

Every push to `main` runs full normal tooling, domain, and core verification,
regardless of changed paths. Remove the current required-orchestrator
`paths-ignore` behavior that skips docs pushes if this policy is ratified.

Scheduled CI runs the full normal suite plus maximal/slow verification. Suitable
nightly work includes longer fuzzing, extended parser/Test262 conformance, rare
configuration permutations, slow sanitizers, optional integrations, and long
compatibility tests.

Document whether benchmarks are gating correctness checks or informational.
They need not block the required gate unless that is an explicit existing
policy.

## 11. Fuzzing and conformance split

The current combined fuzz job prevents useful path-aware selection. Split it at
least into:

```text
fuzz-native-security
fuzz-lua-source
fuzz-js-source
```

Split native groups further only when measured runtime and ownership justify it.

PR behavior:

- Lua parser/fuzzer change: Lua conformance and fuzz smoke;
- JS parser/fuzzer change: JS conformance and fuzz smoke;
- native protocol/security primitive change: relevant native fuzz targets;
- unrelated tooling/docs: no parser/native fuzz;
- main: all normal fuzz jobs;
- nightly: all extended campaigns.

Focused parser jobs retain their sanitizer instrumentation. A frontend-only PR
does not need the entire native sanitizer matrix, but it must still test its VM,
bridge where relevant, and parser harness under the intended sanitizers.

## 12. Native integration triggers

Map expensive integrations to actual invalidation surfaces:

```text
TLS/network code       -> TLS/network suites
native DB adapter      -> corresponding DB suite
WAMR/compute runtime   -> WASM/WAMR suites
GPU integration        -> GPU suites
Keel/platform core     -> networking/platform suites
shared core abstraction -> broad/full core
```

False positives cost time; false negatives can miss trusted-core regressions.
When uncertain, select core.

## 13. Parallelism and matrix reduction

Parallelize independent jobs when runner availability permits. Avoid dependency
chains used only to share a checkout or cache.

For selective PRs, distinguish representative coverage from exhaustive
compatibility coverage. Do not form unnecessary Cartesian products such as:

```text
OS x compiler x TLS x DB x sanitizer
```

Prefer orthogonal jobs that each test a meaningful boundary. Preserve exhaustive
or broad matrices for core changes, main, nightly, and release workflows.

## 14. Cache strategy

Audit every existing cache and record ownership, key inputs, hit/miss behavior,
size, and trust boundary.

Prioritize caching:

1. immutable downloaded toolchains/dependencies;
2. browser assets;
3. generated external dependency builds with exact keys;
4. only later, Hull native objects through carefully keyed archives or
   `ccache`/`sccache` if justified.

Keys for compiled/native content include:

```text
OS and architecture
compiler identity/version
sanitizer/build mode
runtime/features
all relevant flags
source/header/build-config hashes
submodule revisions
```

A JS parser edit must not invalidate WAMR, DuckDB, or unrelated native
dependency caches. Conversely, incompatible compiler/flag/config combinations
must never share compiled artifacts.

Do not allow an untrusted PR cache to become a trusted release artifact. Main
core/reproducibility/release jobs continue to perform clean verification builds
where required.

## 15. Force-full mechanism

Add a documented strengthening mechanism, at minimum:

```yaml
workflow_dispatch:
  inputs:
    force_full:
      type: boolean
```

A reviewer label or commit marker is optional only if secure and deterministic.
The mechanism may add coverage but never suppress required work.

## 16. Applicability-aware required gate

Create a stable final job such as `ci-success`, subject to the branch-protection
migration in section 17.

It uses `if: always()` and statically `needs` all orchestrated jobs. It must not
merely accept every `skipped` result.

The classifier emits which jobs are required:

```text
required.tooling_js=true
required.query=false
required.full_core=false
```

The final gate verifies:

- classifier completed successfully;
- every required job has result `success`;
- a required job with `skipped`, missing, failure, or cancellation fails;
- only jobs declared inapplicable may be skipped;
- unexpected failure or cancellation fails;
- unknown result/state fails closed.

Use a repository-owned result-validation script rather than a fragile, duplicated
long expression if that improves testability. Matrix jobs expose aggregate
results to dependent jobs; account for this explicitly.

`if: always()` only makes the gate evaluate. It does not itself turn failures
into success.

## 17. Branch-protection migration

Adding `ci-success` does not update GitHub branch protection automatically.

The design must record:

- current required check names;
- intended new required check;
- temporary overlap/transition plan;
- repository-admin action required;
- rollback procedure;
- which separate workflows remain independently required.

Do not remove old required check identities before branch protection accepts the
replacement. DCO, release, site deployment, and specialized on-demand/path
workflows may remain separate; document whether the final gate aggregates them.

## 18. Separate workflows and release safety

Inspect every PR-triggered workflow, not only `ci.yml`. Avoid leaving expensive
duplicate PR workflows outside the classifier unintentionally.

Specialized path-triggered diagnostic workflows can remain separate when they
are intentionally non-required and narrowly scoped. Release workflows must use
fully verified clean artifacts and must not depend on selective PR-only output.

Preserve existing raw Make/test commands and artifacts consumed by other
automation.

## 19. Classifier and gate tests

Add direct fixtures for at least:

### JS frontend only

```text
stdlib/cli/js/hull/source/parser.js
```

Expected: JS tooling/frontend plan, no full core, fresh representative host.

### Lua frontend only

```text
stdlib/cli/lua/hull/source/parser.lua
```

Expected: Lua tooling/frontend plan.

### Query compiler only

Expected: focused Query/domain plan without unrelated native suites.

### Test-only JS fuzzer C

```text
fuzz/fuzz_js_source.c
```

Expected: focused JS fuzz/frontend plan, not production core unless shared build
configuration also changes.

### C frontend bridge

```text
src/hull/frontend/js_session.c
```

Expected: full core plus focused JS frontend verification.

### Shared native header

Expected: full core.

### Workflow/classifier/Makefile edit

Expected: full all.

### True docs only

Expected: lightweight PR plan and successful required gate.

### Embedded Markdown/tooling data

Expected: appropriate tooling/core plan, not docs-only.

### Mixed tooling and core

Expected: full core plus relevant tooling tests.

Also test renames, deletions, unknown paths, malformed input, empty diff,
classifier failure, required-job skipped, allowed-job skipped, cancellation, and
matrix aggregate outcomes.

## 20. Hull-facing acceptance

Focused tooling and domain CI must validate Hull through its real interfaces,
not unit tests alone:

```text
hull dev
hull dev --agent
hull agent inspect
```

Use one representative Linux environment unless platform-specific behavior is
under test. Do not duplicate these expensive E2Es in every matrix member.

For source frontend/discovery changes, prove that the freshly linked Hull
contains and uses the changed embedded tooling.

## 21. Metrics and assessment

Before restructuring, record where measurable:

```text
critical-path duration
slowest jobs
job count on a representative tooling PR
cache hit/miss behavior
duplicated builds/tests
```

Afterward, report plans for tooling-only, domain-only, core, main, and nightly.
Do not invent runtime savings. Report verifiable structural changes, such as:

```text
tooling PR previously launched N native jobs; now launches M
```

Record remaining expensive bottlenecks and deliberate conservative
over-triggering.

## 22. Main safety and limitations

Selective PR CI can allow a cross-subsystem regression to be discovered by the
full `main` run after merge rather than before it. Mitigate this by conservative
classification, focused integration tests, full CI for shared/unknown changes,
and reviewer force-full capability. State this tradeoff honestly.

Path classification supplements semantic review; it cannot prove a source file
has no wider effect.

## 23. Documentation deliverable

Update this document after implementation with:

- CI goals and verified current inventory;
- change facts and path rules;
- PR/main/nightly plans;
- required gate semantics;
- cache/trust strategy;
- native integration triggers;
- why the orchestrator is never path-skipped;
- how to add a future subsystem;
- force-full procedure;
- branch-protection migration;
- debugging classification mistakes;
- metrics and remaining bottlenecks.

## 24. Review-gated implementation slices

### Slice 1: inventory and classifier

- verified job/runtime/coverage/cache map;
- repository-owned classifier;
- overlapping facts and derived plans;
- merge-base diff collection;
- fixtures and fail-closed behavior;
- no expensive-job skipping yet.

Stop for review.

### Slice 2: orchestration and final gate

- always-triggered orchestrator;
- conditional existing jobs;
- applicability-aware result gate;
- main/schedule/force-full behavior;
- branch-protection migration plan;
- initially preserve job contents.

Stop for review.

### Slice 3: focused tooling jobs

- clean representative embedded-host build;
- Lua/JS frontend targets;
- mixed discovery/dev/agent E2E;
- separated parser fuzzing/conformance;
- focused tooling plans.

Stop for review.

### Slice 4: domain and native integration mapping

- stable Query/Compute path classes;
- DB/WAMR/GPU focused triggers;
- domain plans;
- mixed domain/core behavior.

Stop for review.

### Slice 5: cache and matrix optimization

- cache audit and exact keys;
- safe dependency/tool caches;
- remove unnecessary Cartesian products;
- parallelize independent work;
- preserve clean reproducibility/release verification.

Stop for review.

### Slice 6: nightly and rollout

- scheduled maximal suite;
- extended fuzz/conformance;
- branch-protection transition;
- before/after metrics;
- operational documentation.

Stop for final review.

## 25. Explicit safety requirements

Never skip full core CI for changes to production native sources, shared headers,
build composition, compiler/linker flags, platform abstractions, C frontend
bridges, QuickJS embedding/session code, dependency versions, generated native
glue, or workflow/classifier logic.

When uncertain, classify as core. False positives cost CI time; false negatives
can miss trusted-core regressions.

## 26. Success criteria

A change confined to:

```text
stdlib/cli/js/hull/source/**
```

does not launch the complete native/platform/integration matrix, but does run:

```text
fresh Hull relink containing changed JS tooling
JS frontend/parser/conformance verification
relevant JS fuzz smoke
ProjectDiscovery and mixed-language tests
hull dev --agent
hull agent inspect
lint and required gate
```

A change to production `src/**`, public `include/**`, native build/runtime
configuration, or shared composition triggers the complete trusted-core plan.

Every push to `main` receives full normal verification. Scheduled CI preserves
or strengthens exhaustive testing. Required checks never remain pending because
an entire required workflow was path-skipped.

The optimization is:

> Avoid reproving unchanged trusted-core properties on every frontend/tooling
> pull request while preserving strong subsystem proof, frequent full proof, and
> fail-closed classification.

## 27. Completion report

At completion report:

- workflow and support files changed;
- jobs added, removed, split, or reorganized;
- classifier facts, path rules, and fail-closed behavior;
- required gate applicability logic;
- branch-protection migration status;
- cache and matrix changes;
- tooling, domain, core, main, and nightly plans;
- fresh embedded-host verification;
- dev/agent acceptance coverage;
- compatibility/release considerations;
- measured structural reductions;
- remaining bottlenecks and conservative over-triggering.

---

# Appendix A. Discovery findings — verified current inventory (2026-08-20)

Status of this appendix: **DISCOVERY COMPLETE (Slice-1 inventory half). Awaiting
ratification. No workflow changes made.** Produced per the section-1 cadence
steps 1-3; the proposed plans (Appendix B) are for ratification before any
implementation (section 24, Slice 1 = classifier + tests only).

## A.1 Workflow triggers (verified)

`ci.yml` `on:`:
```yaml
push:         { branches: [main], paths-ignore: [site/**, docs/**, **.md, CHANGELOG.md, README.md, CONTRIBUTING.md, LICENSE, LICENSING.md] }
pull_request: { branches: [main], paths-ignore: [ ...identical... ] }
```
- **No `schedule`, no `workflow_dispatch`.** No top-level `paths:` allowlist (only `paths-ignore`).
- Every job runs on BOTH pull_request-to-main and push-to-main, EXCEPT `benchmark`
  (`if: github.event_name == 'push'`, push-only).
- **No final aggregation / required-gate job exists** (the only `needs:` is the
  narrow `reproducibility-cosmo-compare` -> `reproducibility-cosmo` artifact diff).
  This is the single biggest gap vs the target architecture (section 16).

## A.2 ci.yml job inventory — 41 jobs, grouped by coverage

| Group | Jobs (job-id) | Runner / matrix | Notes |
|---|---|---|---|
| **Core build+e2e (long pole)** | `build` (matrix: Linux, Linux-clang, Linux-aarch64, macOS = 4) | ubuntu-24.04 / -arm / macos-15 | each runs full `make` + `test` + `e2e` + ~60 `e2e-*` steps + registry/hardening checks. THE critical path. |
| **Build pipeline** | `build-pipeline` (Linux, macOS = 2) | | `make e2e-build` (platform/app/sign/self-build) |
| **Flavors** | `flavors` (8 include entries: DB=0, HTTP=0, POSTGRES, postgres-only, MYSQL, mysql-only, IMAGE=0, …) | ubuntu-24.04 | link-flavor smokes |
| **Sanitizers** | `sanitizers` (ASan+UBSan), `msan` (30m), `tsan`, `tsan-shared-heap` | ubuntu-24.04 | msan is a slow long-pole |
| **WASM/AOT (redundant wamrc)** | `wasm-readonly-heap-aot`, `compute-aot-shared-heap`, `compute-memops-freestanding`, `stream-meta`, `spans-example`, `spans-multi`, `spans-hugefile`, `wasm-guarded-aot-arm64`, `mapped-span-bench` | ubuntu-24.04 (+ 1 arm) | **8-9 jobs each rebuild `wamrc` (LLVM) from scratch, no cache** — the largest redundant cost. |
| **Fuzz (combined)** | `fuzz` | ubuntu-24.04 | one job runs 13 native fuzzers + `fuzz-lua-source` + `fuzz-js-source`. Must be SPLIT (section 11). |
| **DB (real engines)** | `postgres` (PG16), `mysql` (MySQL8), `valkey` (redis), `duckdb` | ubuntu-24.04 | Docker/apt engines |
| **Composable features** | `duckdb-feature`, `gpu-feature`, `tui-feature`, `postgres-feature`, `mysql-feature` | ubuntu-24.04 | `e2e_feature_*.sh` |
| **Reproducibility** | `reproducibility` (Linux, macOS=2), `reproducibility-container`, `reproducibility-container-interleave`, `reproducibility-cosmo` (a,b=2), `reproducibility-cosmo-compare` | ubuntu-24.04 + macos + containers | build-twice+cmp; **deliberately un-cached** (a build cache would defeat the byte-identical check). |
| **Cosmo** | `cosmo` | ubuntu-24.04 | fetch cosmocc + platform-cosmo + full test/e2e under APE |
| **GPU** | `gpu` (macOS Metal) | macos-15 | fetch-wgpu, GPU=1 test/e2e |
| **Browser** | `htmx-browser` (Linux, macOS=2) | | Playwright/Chromium, dev+build modes. **Only cached job.** |
| **Coverage / static** | `coverage` (lcov), `analyze` (scan-build), `cppcheck` (inside analyze) | ubuntu-24.04 | heavy |
| **Lint** | `lint` | ubuntu-24.04 | luacheck + Biome + sdk-header checks |
| **Embedders** | `embed-rust`, `embed-zig` | ubuntu-24.04 | `hl_embed_*` ABI smokes |
| **musl** | `musl` | ubuntu-24.04 (alpine docker) | |
| **Discovery** | `project-discovery-lua` | ubuntu-24.04 | `e2e-project-discovery-lua` |
| **Bench (push-only)** | `benchmark` (lua, js=2) | ubuntu-24.04 | informational; not on PRs |

**Key structural finding:** the JS/Lua frontend + conformance + fuzz-entry unit
suites currently run INSIDE the monolithic `build` job's `make test` — there is
**no independently-addressable "frontend tests only" job today**. A focused
tooling plan (section 9) therefore REQUIRES a new focused job (Slice 3) that does
a fresh embedded-host Linux build + targeted test binaries + the
discovery/dev/agent E2Es; it cannot be assembled from existing jobs by `if:`
alone.

## A.3 Other PR-relevant workflows (verified)

| Workflow | Trigger | PR? | Required? | Disposition for the classifier |
|---|---|---|---|---|
| `dco.yml` (DCO / Sign-off) | pull_request [opened,synchronize,reopened], no paths | yes (every PR) | **yes** | KEEP SEPARATE + always-required. Cheap, universal. Not classifier-gated. |
| `cosmocc-windows-e2e.yml` | workflow_dispatch + pull_request `paths:` (cosmocc/tool/tar/fs/sandbox/build.lua) | yes (path-scoped) | no ("non-required / on demand") | Expensive (incl. Windows). Already self-limited by paths; candidate to fold under the classifier or leave as-is (non-required). |
| `cosmocc-bbox-probe.yml` | workflow_dispatch + pull_request `paths:` (only itself) | yes (self-scoped) | no | Trivially scoped; leave. |
| `bench_mapped_span_1g.yml` | workflow_dispatch only | no | no | Manual; leave. |
| `windows-cosmocc.yml` | workflow_dispatch only | no | no | Manual investigation; leave. |
| `deploy-site.yml` | push [main] `paths:` (site/**, install.sh, minisig, itself) | no | no | Deploy. Trust-bearing (minisign-verifies install.sh). Must NOT consume PR caches. |
| `release.yml` | push tags `v*` | no | no | **Trust anchor** — signs the platform manifest, reproducible-container builds, `TRUST_PLATFORM_LIB`/`TRUST_FEATURE_LIBS` embed exact signed bytes. **Must NEVER consume selective/PR caches** (section 14). |

## A.4 Cache audit (verified)

- **The ONLY `actions/cache` in the entire repo** is `htmx-browser`'s
  `playwright-${{ runner.os }}-v1.48.0` (Chromium bundle, ~150 MB). Key is a
  hardcoded version literal (not lockfile-hashed) — can drift from the pin in
  `e2e_htmx_playwright.sh`; low risk (Playwright re-fetches on mismatch).
- **No native-object / built-artifact caching anywhere.** Every toolchain/dep
  (cosmocc, wgpu, DuckDB, zig, LLVM/wamrc, WAMR submodule) is fetched fresh per
  job (`make fetch-*`, SHA-verified) or apt/curl. Cross-job data uses
  `upload/download-artifact` (within-run only), not `cache`.
- Consequence: **no existing cache can cross a trust boundary into a signed
  release** (the section-14 risk is purely forward-looking). Reproducibility jobs
  intentionally avoid caching so the byte-identical `cmp` stays honest.

## A.5 Measurements (verified where access permitted)

- Recent `ci.yml` wall-clock (from `gh run list`, run-created→updated):
  - PR runs: ~24m21s, ~25m22s, ~28m42s (feat branches).
  - push-main runs: ~22m30s, ~22m38s, ~23m48s.
- Representative **tooling PR (this session's #369/#370/#371) launched ALL ~41
  ci.yml jobs + DCO** — including the entire native/AOT/sanitizer/repro/cosmo/
  gpu/db/browser matrix — despite touching only `stdlib/cli/**`, `src/hull/
  source` Lua/JS, test harnesses, and fixtures. This is the exact waste the
  redesign targets: e.g. #371 (a 2-line dead-code deletion in a test file) ran
  the full ~41-job matrix.
- **ACCESS LIMIT (honest):** `GET /repos/artalis-io/hull/branches/main/
  protection/required_status_checks` returned empty/!authorized with the
  available token — I could NOT authoritatively enumerate the current required
  status-check names. This is required for the section-17 branch-protection
  migration and must be obtained by a repo admin before Slice 2. Inferred (from
  job display names + DCO) but UNVERIFIED required set: the `ci.yml` job display
  names (e.g. "ASan + UBSan", "MSan + UBSan", "Cosmopolitan (APE)", "Build
  Pipeline (Linux/macOS)", …), "DCO", and "Sign-off check". Slice 2 must not
  proceed on the inferred list.

## A.6 Per-job "invalidation surface" (proposed mapping, for ratification)

Which change-facts (section 5) should trigger each existing job. This is the
raw material for the classifier→plan mapping (Appendix B); it is a PROPOSAL.

| Existing job(s) | Should run when (fact) | On tooling/docs-only PR? |
|---|---|---|
| `build` (full 4-matrix) | production_core_changed OR build_composition_changed OR full_all | NO (replace with 1 focused embedded-host Linux build for tooling) |
| `sanitizers`/`msan`/`tsan`/`tsan-shared-heap` | production_core_changed, wasm_changed (tsan-shared-heap) | NO |
| WASM/AOT cluster (9 jobs) | wasm_changed OR compute_changed OR production_core_changed | NO |
| `fuzz` (after split) | native security/protocol (native-security), lua_frontend (lua fuzz), js_frontend (js fuzz) | only the matching split target |
| `postgres`/`mysql`/`valkey`/`duckdb` + `*-feature` | native_db_changed / matching backend / build_composition | NO |
| `gpu`/`gpu-feature` | gpu_changed | NO |
| `cosmo`/`reproducibility*` | production_core_changed OR build_composition_changed | NO |
| `htmx-browser` | web stdlib / http-server / template / htmx examples changed | NO (unless those change) |
| `coverage`/`analyze` | core; or nightly | NO (nightly candidate) |
| `lint` | ALWAYS (cheap) | YES |
| `project-discovery-lua` + (new) JS/mixed discovery | lua_frontend / js_frontend / project_discovery_changed | YES (matching) |
| `embed-rust`/`embed-zig` | libhull ABI / production_core | NO |
| `musl` | production_core / build_composition | NO |

---

# Appendix B. Proposed classifier facts → plan mapping (for ratification)

This is the DESIGN to ratify. No code written yet. Slice 1 (section 24) is
classifier + fixtures ONLY; it does not yet skip any job.

## B.1 Classifier shape (RATIFIED; implemented in Slice 1)

- `scripts/ci/classify_changes.py` (**Python 3**, not POSIX sh: POSIX `read`
  cannot reliably consume NUL-delimited paths, and Python gives deterministic
  JSON + adversarial-path fixtures). Portable, testable in the ubuntu-24.04
  image; no third-party path-filter action.
- Input: NUL-delimited changed paths from the real merge base
  (`git diff --name-only -z --no-renames $(git merge-base origin/main HEAD)..HEAD`
  with sufficient fetch depth; NOT `HEAD^`; NOT the paginated API file list).
  **`--no-renames` is deliberate**: a rename shows as delete(old)+add(new), so
  BOTH paths classify and the broader plan wins - a rename across a trust
  boundary can never escape into a narrower plan (fixture-proven).
- Output: deterministic JSON (stdout) + `$GITHUB_OUTPUT` flat `facts_*` /
  `plan_*` booleans + a compact `plan_json` for the Slice-2 orchestrator.
- Fail-closed: unknown path → core-sensitive (`full_core`); empty/ambiguous diff,
  read failure, or rule error → `full_all`; `--event push_main`/`schedule` or
  `--force-full` → `full_all` directly (no path classification).
- Tests: `scripts/ci/test_classify_changes.py` (pure Python, no GitHub Actions)
  covers the section-19 matrix + fail-closed + the cross-trust-boundary rename
  cases. The ci-success GATE cases (required/allowed-job skipped, cancellation,
  matrix aggregates) belong to the Slice-2 gate, not the classifier.

## B.2 Fact → plan derivation (initial rules, conservative)

| Fact (from path rules §7) | Adds to plan |
|---|---|
| `docs_only` (every path in the safe docs set, none embedded) | lint + required gate only |
| `js_frontend_changed` (`stdlib/cli/js/hull/source/**`) | focused_js_frontend (§9.1) |
| `lua_frontend_changed` (`stdlib/cli/lua/hull/source/**`) | focused_lua_frontend (§9.2) |
| `project_discovery_changed` (`stdlib/cli/**/project/**`, projection) | both frontends + mixed discovery E2E |
| `tooling_changed` (other `stdlib/cli/**`) | focused tooling + 1 embedded-host build |
| `js/lua parser or fuzz harness` (`fuzz/fuzz_{js,lua}_source.c`, parser files) | + matching fuzz-smoke |
| `compute_changed` / `wasm_changed` / `gpu_changed` / `native_db_changed` / `tls_network_changed` | matching focused native integration (§12) |
| `production_core_changed` / `build_composition_changed` / `ci_changed` / unknown | **full_core / full_all** (§7.1, §8, §25) |

Overlap is additive (§5): core + tooling ⇒ full_core PLUS the focused tooling
tests. `ci_changed` (any workflow/classifier/Makefile/`mk/**`) ⇒ full_all and
self-trust caveat (§8) — governance stays with CODEOWNERS/branch protection.

## B.3 Immediate prerequisites this discovery surfaced

1. **Add a final `ci-success` gate job** (§16) — none exists today; required
   before any `if:`-gating so branch protection can point at ONE stable check.
2. **Carve focused frontend/tooling test jobs** — the JS/Lua frontend suites are
   trapped inside `build`'s `make test`; Slice 3 must expose them as a focused
   embedded-host job (§4, §20).
3. **Split `fuzz`** into `fuzz-native-security` / `fuzz-lua-source` /
   `fuzz-js-source` (§11).
4. **Obtain the authoritative required-check list** (repo admin) before Slice 2
   (§17) — currently access-limited.
5. **wamrc redundancy** (9 jobs rebuild it) is the top cache-optimization target
   for Slice 5 (immutable keyed toolchain cache), independent of classification.

## B.4 Proposed slice ordering (matches §24, with this repo's specifics)

- **Slice 1 (DONE, this change):** `scripts/ci/classify_changes.py` + NUL-safe
  merge-base diff + `scripts/ci/test_classify_changes.py` fixtures (§19 cases
  incl. this repo's `js_session.c`, `parser.js`, `parser.lua`, `fuzz_js_source.c`,
  `mk/**`, docs-only, mixed, plus cross-trust-boundary renames). NO job skipping
  yet. **All `.github/**`** (workflows, actions, CODEOWNERS, governance) +
  `scripts/ci/**` + `Makefile`/`mk/**` are in the self-trust set so those changes
  force full_all. Generic `tests/**` / `examples/**` FAIL CLOSED to full_core
  (no focused generic-test job yet - a test change must not skip its own tests);
  known frontend test/fixture paths (`tests/hull/frontend/**`, `tests/hull/
  source/**`, `tests/fixtures/{test262,lua54-tests}/**`, the discovery E2E, the
  js/lua fuzz seeds) take the focused route. The NUL stream is byte-validated
  (missing terminal NUL / empty interior / absolute / `.`/`..` component ->
  full_all; empty component (docs//x, trailing docs/) also rejected; spaces/tabs/newlines inside a path are valid), with pure-Python
  classify() fixtures AND subprocess/CLI tests of the byte decoder. 67/67
  fixtures green. Stops for review before Slice 2.
- **Slice 2 (DONE, this change):** a `classify` orchestrator job (runs the
  classifier + `check_gate_completeness.py` + all fixture self-tests, then
  classifies the merge-base diff) + an applicability-aware `ci-success` gate
  (`if: always()`, static `needs` on EVERY job, repo-owned
  `scripts/ci/ci_gate.py`: success required, only a declared-inapplicable skip
  - the push-only `benchmark` on a PR - is permitted; failure / cancellation /
  disallowed-skip / missing / unknown fail closed; a matrix job's aggregate
  result is handled). Adds `workflow_dispatch` with a `force_full` input (§15).
  **PRESERVES current job execution** - no expensive job is classifier-skipped
  yet, `paths-ignore` is retained, and `ci-success` is NOT yet a required check.
  Proven by `test_ci_gate.py` (success / failure / cancellation / legit + illegit
  skip / missing-required / unknown / matrix-aggregate). The branch-protection
  cutover (make `ci-success` required, likely alongside DCO) is a SEPARATE,
  explicitly authorized step - `main` currently has NO protection/rulesets/
  required checks, so it is a clean, reversible addition, not a migration.
  Stops for review before Slice 3.
- **Slice 3 (IN PROGRESS):** two review checkpoints.
  - **Checkpoint 3a (DONE, this change):** ADD + prove, preserving all existing
    execution. Split the combined `fuzz` job into `fuzz-native-security` /
    `fuzz-lua-source` / `fuzz-js-source` (§11), and add `focused-js-frontend` /
    `focused-lua-frontend` jobs (a FRESH embedded-host build via the new
    `make test-js-frontend` / `test-lua-frontend` targets + the discovery /
    dev-agent / inspect lifecycle, §4/§9/§20). These run on EVERY PR/push
    alongside the full matrix (nothing skipped); `ci-success.needs` updated
    (48 jobs, 47 gated). No classifier skipping, no branch-protection change.
  - **Checkpoint 3b (DONE, this change):** classifier-based skipping, driven by a
    SINGLE job-applicability map (`scripts/ci/job_plan.py`). The `classify` job
    emits per-group run-flags; each expensive job gates on
    `if: needs.classify.outputs.run_*`; the `ci-success` gate derives its
    allow-skip from the SAME map (`ci_gate.py --plan`), and
    `check_job_plan_consistency.py` (run in `classify`) proves the `if:`
    conditions and the map never drift. Only PROVEN narrow classes skip -
    docs-only, JS frontend/fuzz, Lua frontend/fuzz; every other plan (core,
    tooling, project-discovery, query, compute, db, gpu, tls, examples, generic
    tests, unknown) and every main/full/force-full run the broad suite. Narrowness is a POSITIVE, fail-closed allowlist: a plan may skip the broad
    matrix only when it is a well-formed plan dict whose true flags all lie in
    the approved narrow set (docs_only / focused_js/lua_frontend / _fuzz + lint)
    and include a real selector - an empty/unknown/non-boolean/newly-added or
    otherwise non-approved flag (e.g. focused_wasm) yields BROAD. Every job
    DEFAULTS applicable (an unmapped job -> `always` -> never skips); mixed
    core+frontend runs full core PLUS focused; `benchmark` stays push-only.
    `paths-ignore` removed so `classify` + `ci-success` always appear. Fixtures:
    `test_job_plan.py` (docs-only / pure-JS / pure-Lua / fuzz-only / core / mixed
    / main / force-full / unexpected-skip / missing-applicability) + updated
    `ci_gate` plan-derived path. Branch protection UNCHANGED (`ci-success` still
    not required). Proven live: a pure-frontend PR skips ~43 jobs while the gate
    passes; an unexpected skip of an applicable job fails the gate.
- **Slice 4 (DESIGN in Appendix C; awaiting review):** DB/GPU/compute
  native-integration triggers - carve the expensive per-subsystem integrations
  out of an always-run core-common floor. Three checkpoints: design -> additive
  proof -> skip activation. NOT implemented.
- **Slices 5-6:** cache+matrix (wamrc cache, kill the 9x rebuild); nightly
  (schedule) + rollout. Each stops for review.

---

# Appendix C. Slice 4 design - DB / GPU / compute native-integration triggers

Status: **DESIGN (awaiting review). NOT implemented.** First of Slice 4's three
review checkpoints: **design -> additive proof -> skip activation.** No
implementation and no branch-protection change until the design is ratified.

## C.0 Locked constraints (ratified)

1. Every production C change still runs a **core-common floor**: representative
   builds, sanitizers / static analysis, reproducibility, lint, and applicable
   platform checks.
2. Classification follows **actual compilation / link / dependency closure**, not
   filename intuition.
3. **Shared DB code fans out to every DB backend;** backend-specific code selects
   that backend plus core-common.
4. **Shared runtime, public headers (`include/**`), vendor changes, `Makefile` /
   `mk/**`, feature composition, unknown paths, `main`, and force-full remain
   fully broad.**
5. Per-subsystem plans are **additive**: a DB + GPU change runs both sets; a
   shared/core change runs all.
6. Include **matching protocol fuzzers** where relevant.
7. Keep the existing **four-platform `make test` build as part of core-common**
   for now; Slice 4 only removes redundant *secondary integrations*.
8. **TLS and generic tooling remain broad** (no proven isolated suite yet).
9. **No skipping** is permitted until each mapping is proven additively (see C.7).

## C.1 Classification method (dependency closure, not filenames)

A source file narrows to a subsystem ONLY if its compilation/link closure is
confined to that subsystem. The closure is read from the Makefile / `mk/**` and
the `HL_ENABLE_*` gating, NOT from the filename:

- A **backend `.c`** (e.g. `cap/db_postgres.c`, `cap/pgwire.c`, `cap/pg_conn.c`)
  compiles only into the `HL_ENABLE_POSTGRES` build + its unit tests
  (`test_pgwire`, `test_pg_conn`) + its protocol fuzzers (`fuzz_pgwire`,
  `fuzz_pg_dsn`, `fuzz_pg_rewrite`). It does not enter the MySQL/GPU/WASM closures
  -> it may narrow to the `db-postgres` subsystem.
- The **shared DB selector** `cap/db_select.c` `#include`s every backend header
  and populates `BACKENDS[]`; `cap/db.c` / `db_common.c` / `db_registry.c` /
  `db_dynamic.c` / `db_udf.c` are on every backend's link path. Their closure is
  ALL DB backends -> they fan out to the whole `db` subsystem (constraint 3).
- **Any public header** (`include/**`) is on an unbounded consumer closure -> broad
  (constraint 4), even a `cap/db_postgres.h`.
- The curated isolated-file allowlist (C.3) is **verified against the actual
  compile targets** during the checkpoint-1 implementation; a file whose closure
  cannot be confirmed subsystem-local stays broad (fail closed).

## C.2 Job buckets - core-common vs each subsystem (exact, with rationale)

The current single `full-matrix` group is split into **core-common + native
subsystems**. Unit coverage stays FULL for every production-C change (core-common
runs `build` = `make test` = all ~90 unit binaries on 4 platforms); only the
expensive *secondary integrations* of the untouched subsystems are skipped.

**always** (unchanged): `classify`, `lint`, `ci-success`.

**core-common** (runs for every production-C change AND every broad run) - the
subsystem-AGNOSTIC floor:
| job | why core-common |
|---|---|
| `build` (Linux/clang/aarch64/macOS) | `make test` runs ALL unit tests (incl. `test_db*`, `test_wasm*`, `test_gpu`, `test_js_*`) on 4 platforms - constraint 7 |
| `build-pipeline` (Linux/macOS) | platform+package build path |
| `flavors` | link-flavor floor (DB=0/HTTP=0/IMAGE=0/…) |
| `sanitizers` (ASan+UBSan), `msan`, `tsan` | memory/UB/data-race floor - constraint 1 |
| `analyze` (scan-build + cppcheck) | static-analysis floor |
| `coverage` | metric floor |
| `reproducibility`, `reproducibility-container`, `-interleave`, `-cosmo`, `-cosmo-compare` | byte-reproducibility floor - constraint 1 |
| `cosmo`, `musl` | applicable platform checks - constraint 1 |
| `embed-rust`, `embed-zig` | libhull ABI floor |
| `fuzz-core-security` (NEW - see C.4) | net/fs/config security fuzzers (sh_json, path_normalize, mime_sniff, host_match) |
| `benchmark` | push-only; core-common on push |

**db** subsystem (secondary integrations; sub-grouped per backend so a
backend-specific change selects only its backend - constraint 3):
| sub-group | jobs | source closure |
|---|---|---|
| `db-postgres` | `postgres` (real PG16), `postgres-feature`, `fuzz-db-wire` (pg part) | `cap/db_postgres.c`, `cap/pgwire.c`, `cap/pg_conn.c` |
| `db-mysql` | `mysql` (real MySQL8), `mysql-feature`, `fuzz-db-wire` (mysql part) | `cap/db_mysql.c`, `cap/mysqlwire.c`, `cap/mysql_conn.c` |
| `db-valkey` | `valkey` (redis + feature), `fuzz-db-wire` (resp part) | `cap/valkey.c`, `cap/valkey_conn.c`, `cap/valkey_register.c`, `cap/respwire.c` |
| `db-duckdb` | `duckdb`, `duckdb-feature` | `cap/db_duckdb.c` |
| `db-shared` -> ALL of the above | | `cap/db.c`, `db_common.c`, `db_registry.c`, `db_select.c`, `db_dynamic.c`, `db_udf.c`, `kv.c`, `kv_dynamic.c`, `kv_feature.c` |

(SQLite has no separate integration job - it is the default backend, exercised by
`make test` in core-common - so `cap/db_sqlite.c` maps to core-common only.)

**gpu** subsystem: `gpu` (macOS Metal), `gpu-feature` (Linux). Source:
`cap/gpu_wgpu.c`, `cap/gpu_feature.c`, and the base dispatch `cap/gpu.c`.

**compute** subsystem: the AOT cluster + shared-heap TSan + the compute fuzzer -
`wasm-readonly-heap-aot`, `mapped-span-bench`, `compute-aot-shared-heap`,
`compute-memops-freestanding`, `stream-meta`, `spans-example`, `spans-multi`,
`spans-hugefile`, `wasm-guarded-aot-arm64`, `tsan-shared-heap`, `fuzz-compute-span`
(NEW). Source: `cap/wasm.c`, `wasm_buffer.c`, `wasm_data.c`, `wasm_spans.c`,
`wasm_stream.c`, `worker_wasm.c`, `runtime/{lua,js}/mod_compute.c`.

**frontend/web** subsystem (from Slice 3b, extended): `focused-js-frontend`,
`focused-lua-frontend`, `fuzz-js-source`, `fuzz-lua-source`, `htmx-browser`,
`project-discovery-lua`. A **native** (DB/GPU/compute) change does NOT touch the
frontend/web integrations, so they skip (their unit coverage still runs via
core-common `make test`); a broad change runs them.

## C.3 Path classification (curated isolated allowlist + fail-closed default)

`classify_changes.py` gains, for `src/**`, a POSITIVE isolated-subsystem allowlist
(the C.2 backend/gpu/compute `.c` sets). A matched file emits ONLY its subsystem
fact (`native_db_changed` + a per-backend fact / `gpu_changed` / `compute_changed`)
- NOT `production_core_changed`. **Every other `src/**` path, and all of
`include/**`, `vendor/**`, `Makefile`, `mk/**`, feature composition, and unknown
paths, keep emitting `production_core_changed` / `build_composition_changed` ->
BROAD** (constraint 4). Shared-DB files emit `native_db_changed` with ALL per-backend
facts (constraint 3). Anything ambiguous stays broad (fail closed).

## C.4 Fuzz split (matching protocol fuzzers - constraint 6)

`fuzz-native-security` (today one job of 13 fuzzers) splits into three, mirroring
Slice 3b's parser-fuzz split, so a subsystem change runs its own protocol fuzzers:
- `fuzz-core-security` (core-common): `sh_json`, `path_normalize`, `mime_sniff`, `host_match`.
- `fuzz-db-wire` (db subsystem): `pgwire`, `pg_dsn`, `pg_rewrite`, `mysqlwire`, `mysql_dsn`, `respwire`, `valkey_dsn`.
- `fuzz-compute-span` (compute subsystem): `span_sdk`, `span_window`.

## C.5 Applicability-map extension (`job_plan.py`)

New groups (`core-common`, `db-postgres`, `db-mysql`, `db-valkey`, `db-duckdb`,
`gpu`, `compute`, `fuzz-core-security`, `fuzz-db-wire`, `fuzz-compute-span`) added
to the SAME single map that drives both job `if:` and the gate allow-skip.
Applicable-groups derivation (fail-closed positive allowlist, mirroring 3b):
- `full_all` / `full_core` / `build_composition` / unknown -> **every** group (broad).
- narrow native (an isolated subsystem fact set, no `production_core`) ->
  `always` + `core-common` + the specific subsystem group(s). **Additive**
  (DB+GPU -> both).
- narrow frontend / docs (Slice 3b) -> unchanged (no core-common).
- `benchmark` stays push-only.
Every job DEFAULTS applicable (unmapped -> `always`); `check_job_plan_consistency.py`
continues to require each job's `if:` to reference EXACTLY its group flag.

## C.6 Non-scope (explicit)

- **No `make test` split.** The 4-platform `build`/`make test` stays core-common
  and runs for every production-C change (constraint 7). Slice 4 removes only the
  redundant *secondary integrations* (real engines, GPU hardware, wamrc AOT,
  browser, feature composes) of untouched subsystems.
- **TLS and generic tooling remain broad** (constraint 8) - no isolated suite.
- No new narrow class for web/tooling; the frontend/web jobs only *skip for
  native changes*, they are not a new PR-narrow selector.
- No branch-protection change; `ci-success` stays reported-but-not-required.

## C.7 Proof plan + review checkpoints

**Checkpoint 1 - design (this section).** Stop for review.

**Checkpoint 2 - additive proof (no skipping yet).** Land the classification +
map + fuzz split with the new subsystem jobs, but with NOTHING skipped (jobs still
`if: true`-equivalent), and prove additively via fixtures + a live run:
- broad change -> all subsystems applicable;
- backend-specific: `cap/db_postgres.c` -> `db-postgres` only (not mysql/valkey/duckdb/gpu/compute); likewise a `cap/gpu_wgpu.c`-only and a `cap/wasm.c`-only case;
- mixed-subsystem: a DB + GPU change -> both sets;
- shared-change: `cap/db_select.c` -> ALL db backends; `Makefile`/`include/**`/`vendor/**` -> broad;
- malformed-plan -> broad;
- unexpected-skip -> gate fails.
Stop for review.

**Checkpoint 3 - skip activation.** Turn on the `if:` skipping + gate allow-skip
for the proven subsystem groups, and prove live: a backend-specific PR skips the
other subsystems' integrations while core-common + its subsystem run and the gate
passes; a shared/broad PR runs everything. Stop for review.
