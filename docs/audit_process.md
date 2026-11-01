# Audit process

How Hull's security audit rounds are rated, fixed, reviewed and scheduled.
This is phase 1 of [design/audit_convergence.md](design/audit_convergence.md)
(sections 4, 5.1, 5.5 and 8). The tooling it relies on is the invariant lint
([invariants/README.md](invariants/README.md)), the regression-test check
(`scripts/check_regression_test.sh`) and the nightly fuzzers
(`.github/workflows/fuzz-nightly.yml`).

## 1. Rating a finding: the A1 / A2 / A3 rubric

Every finding names the attacker who reaches it, and its severity follows from
that attacker. Three attacker classes are rated separately:

| Attacker | Capabilities | High / Medium when | Otherwise |
|---|---|---|---|
| **A1. Unauthenticated remote client** | Sends arbitrary requests to a deployed app | It bypasses a capability, reads `_hull_*` or other users' data, executes code, or exceeds the per-request bound (the run deadline today; design section 6.2 later) through ordinary app code | Low |
| **A2. Malicious repo / planted file** | Controls files in a cloned app tree or the deploy directory | It gains code execution, data exposure, or a weakened sandbox / signature check | Low (slow-downs, crashes of the local tool) |
| **A3. App author** | Writes the app's own Lua / JS / SQL | It escapes the sandbox or capability layer, or reads `_hull_*` data | Low (self-inflicted resource use bounded by the run deadline) |

Rules:

- A report lists, for each finding, the attacker (A1, A2 or A3), the
  severity, and the one-line reason the rubric gives that severity.
- A finding reachable only by A3, and bounded by the run deadline, is Low by
  definition, however long the deadline is.
- When several attackers reach a finding, the highest severity wins; say which
  attacker produces it.
- Fuzzer crashes (`fuzz-nightly: fuzz_<target> crashed` issues) are rated the
  same way: which attacker controls the bytes that parser reads.

## 2. Fixing a finding

### 2.1 A regression test per finding (mandatory)

Every audit fix ships a test that **fails on the pre-fix code**, named
`<area>_audit<N>.<case>` (a UTEST suite `<area>_audit<N>`, or an e2e / stdlib
case using the same name in its description). The reviewer confirms the test
failed before the fix.

For UTEST cases this is checked mechanically: `.github/workflows/audit-regression.yml`
runs `scripts/check_regression_test.sh` on every PR whose title contains
"audit". It reverts the PR's non-test files to the merge-base, builds, runs
only the added cases, and expects at least one to fail AT RUNTIME. A test
that only fails to build on the pre-fix code is not enough: the gate retries
with the PR's headers kept and its `tests/regression_stubs/*.c` (pre-fix
stand-ins for new functions) linked in, and a fix that is nothing but new API
needs an explicit `REGRESSION-GATE: build-only <reason>` trailer (commit
message or PR body; audit 43). The job is
**non-blocking** (`continue-on-error`, outside the `ci-success` gate) until a
few rounds show it gives no false failures; until then the PR template's
checklist and the review pass in 2.3 are the requirement. Run it locally in a
clean checkout with `sh scripts/check_regression_test.sh [base-ref]`.

### 2.2 Invariants

A fix that establishes a new rule ("a new X must Y") adds it to CLAUDE.md
**and** a lint rule to `make check-invariants` in the same PR, with good and
bad fixtures ([invariants/README.md](invariants/README.md)). A fix that needs
an exception to an existing rule adds an allowlist entry with its reason, and
the reviewer agrees with the reason.

### 2.3 Independent regression review

Each audit fix PR gets a review pass by someone (or an agent session) that did
not write it, before merge. The pass checks:

- **Regressions** in the fix itself: the previous rounds' new Mediums came
  mostly from the previous round's own fixes. Re-read the changed paths as an
  auditor would: error paths, partial failure, the same pattern in the twin
  runtime (Lua / JS) and the other backends.
- **False refusals of ordinary use**: run the affected examples and stdlib
  suites; a fix that refuses something a normal app does is a regression.
- **Flavor builds**: the `flavors` CI matrix (pure-compute, DB-less,
  image-less, single-backend) and cosmo where the change touches shared code.
- **Docs drift**: CLAUDE.md, docs/*, CHANGELOG describe what the code now does.

## 3. Landing fixes: batch cap, solo patches, freeze

- **Batch cap.** At most 5 audit fix PRs are merged per round before CI on
  main is green again. Merge, wait for main to go green, then merge the next
  batch.
- **Engine and protocol patches land alone.** A change to a vendored engine
  (`vendor/lua`, `vendor/quickjs`, `vendor/tweetnacl`, `patches/wamr`) or to
  protocol state (the Postgres / MySQL / Valkey wire clients, transaction
  tracking, the async run machinery) is merged on its own, with nothing else
  in flight, so a regression can be bisected to it.
- **Feature freeze.** From the start of an audit round until its fixes are
  merged, no feature work merges. Fixes are reviewed against a stable tree.

## 4. Cadence: converged areas

An area is **converged** when it produced no High / Medium finding for three
rounds in a row. Today that is: core and tooling (serve, sandbox, signatures,
build, deploy, agent / MCP); the stdlib auth, session, CSRF, JWT and OAuth
modules; the Lua coroutine and transaction machinery; and memory safety and
capability bypass across the C layer.

- Converged areas get a **diff-only review** each round (what changed since
  the last round) and a **full sweep every fourth round**.
- One High / Medium finding takes an area out of converged status; it
  re-enters after three clean rounds.
- Non-converged areas get a full review every round.

## 5. Related

- [design/audit_convergence.md](design/audit_convergence.md) - the design,
  including the later phases (dispatcher charging, HTTP deadline, session
  state, internal database, build-time shaders).
- [invariants/README.md](invariants/README.md) - the invariant lint rules and
  the allowlist.
- [security.md](security.md) - the threat model and capability invariants.
- `CONTRIBUTING.md` - "Audit fixes" summarises this page for contributors.
